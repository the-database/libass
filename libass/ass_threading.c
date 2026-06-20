/*
 * Copyright (C) 2026 libass contributors
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#if CONFIG_THREADS

#include <stdlib.h>
#include <stdbool.h>

#include "ass_threading.h"

#ifdef _WIN32
typedef HANDLE ass_native_thread_t;
#else
#include <unistd.h>
typedef pthread_t ass_native_thread_t;
#endif

struct ass_thread_pool {
    ass_mutex_t lock;
    ass_cond_t  work_cond;   // wake workers when a job is posted
    ass_cond_t  done_cond;   // notify the dispatcher when a job completes

    size_t n_threads;        // total parallelism, including the dispatcher
    size_t n_workers;        // spawned worker threads (== n_threads - 1)
    ass_native_thread_t *threads;

    // Current job. next_index is claimed lock-free via fetch-add.
    void (*task)(void *arg, size_t index, size_t worker_id);
    void *arg;
    size_t count;
    ass_atomic_size_t next_index;

    size_t generation;       // bumped once per dispatched job
    size_t active_workers;   // spawned workers still processing this job
    size_t ready_workers;    // workers that have entered their wait loop
    bool   shutdown;
};

struct worker_arg {
    ASS_ThreadPool *pool;
    size_t worker_id;
};

// Claim and run indices until the job is exhausted.
static void run_job(ASS_ThreadPool *pool, size_t worker_id)
{
    for (;;) {
        size_t idx = ass_atomic_inc_size(&pool->next_index) - 1;
        if (idx >= pool->count)
            break;
        pool->task(pool->arg, idx, worker_id);
    }
}

static void worker_loop(ASS_ThreadPool *pool, size_t worker_id)
{
    ass_mutex_lock(&pool->lock);
    size_t seen = pool->generation;

    // Announce readiness so pool creation can't return (and thus no job can
    // be posted) until every worker is parked in the wait below. This pins
    // each worker's `seen` baseline before the first generation bump. The
    // creator re-checks the count, so we just bump and signal here (never
    // reading n_workers, which the creator may still be finalizing).
    pool->ready_workers++;
    ass_cond_signal(&pool->done_cond);

    for (;;) {
        while (!pool->shutdown && pool->generation == seen)
            ass_cond_wait(&pool->work_cond, &pool->lock);
        if (pool->shutdown)
            break;
        seen = pool->generation;
        ass_mutex_unlock(&pool->lock);

        run_job(pool, worker_id);

        ass_mutex_lock(&pool->lock);
        if (--pool->active_workers == 0)
            ass_cond_signal(&pool->done_cond);
    }
    ass_mutex_unlock(&pool->lock);
}

#ifdef _WIN32
static unsigned __stdcall worker_entry(void *data)
#else
static void *worker_entry(void *data)
#endif
{
    struct worker_arg *wa = data;
    ASS_ThreadPool *pool = wa->pool;
    size_t worker_id = wa->worker_id;
    free(wa);
    worker_loop(pool, worker_id);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static bool thread_start(ass_native_thread_t *t, struct worker_arg *wa)
{
#ifdef _WIN32
    HANDLE h = (HANDLE) _beginthreadex(NULL, 0, worker_entry, wa, 0, NULL);
    if (!h)
        return false;
    *t = h;
    return true;
#else
    return pthread_create(t, NULL, worker_entry, wa) == 0;
#endif
}

static void thread_join(ass_native_thread_t t)
{
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

ASS_ThreadPool *ass_thread_pool_create(size_t n_threads)
{
    if (n_threads < 1)
        n_threads = 1;

    ASS_ThreadPool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->n_threads = n_threads;
    pool->n_workers = n_threads - 1;
    pool->generation = 0;

    bool lock_ok = ass_mutex_init(&pool->lock) == 0;
    bool work_ok = lock_ok && ass_cond_init(&pool->work_cond) == 0;
    bool done_ok = work_ok && ass_cond_init(&pool->done_cond) == 0;
    if (!done_ok) {
        if (work_ok)
            ass_cond_destroy(&pool->work_cond);
        if (lock_ok)
            ass_mutex_destroy(&pool->lock);
        free(pool);
        return NULL;
    }

    if (pool->n_workers) {
        pool->threads = calloc(pool->n_workers, sizeof(*pool->threads));
        if (!pool->threads) {
            ass_thread_pool_destroy(pool);
            return NULL;
        }

        size_t started = 0;
        for (size_t i = 0; i < pool->n_workers; i++) {
            struct worker_arg *wa = malloc(sizeof(*wa));
            if (!wa)
                break;
            wa->pool = pool;
            wa->worker_id = i;
            if (!thread_start(&pool->threads[i], wa)) {
                free(wa);
                break;
            }
            started++;
        }

        // If not all workers started, run with however many we got rather
        // than fail outright; correctness does not depend on the count.
        // Finalize n_workers and wait for every started worker to park in its
        // wait loop, all under the lock (workers never read n_workers).
        ass_mutex_lock(&pool->lock);
        pool->n_workers = started;
        while (pool->ready_workers < started)
            ass_cond_wait(&pool->done_cond, &pool->lock);
        ass_mutex_unlock(&pool->lock);

        pool->n_threads = started + 1;
    }

    return pool;
}

void ass_thread_pool_destroy(ASS_ThreadPool *pool)
{
    if (!pool)
        return;

    if (pool->n_workers) {
        ass_mutex_lock(&pool->lock);
        pool->shutdown = true;
        ass_cond_broadcast(&pool->work_cond);
        ass_mutex_unlock(&pool->lock);

        for (size_t i = 0; i < pool->n_workers; i++)
            thread_join(pool->threads[i]);
    }
    free(pool->threads);

    ass_cond_destroy(&pool->done_cond);
    ass_cond_destroy(&pool->work_cond);
    ass_mutex_destroy(&pool->lock);
    free(pool);
}

size_t ass_thread_pool_nthreads(ASS_ThreadPool *pool)
{
    return pool->n_threads;
}

void ass_thread_pool_run(ASS_ThreadPool *pool, size_t count,
                         void (*task)(void *arg, size_t index, size_t worker_id),
                         void *arg)
{
    if (!count)
        return;

    // Without spawned workers, run inline on the calling thread.
    if (!pool->n_workers) {
        pool->task = task;
        pool->arg = arg;
        pool->count = count;
        ass_atomic_store_size(&pool->next_index, 0);
        run_job(pool, 0);
        return;
    }

    ass_mutex_lock(&pool->lock);
    pool->task = task;
    pool->arg = arg;
    pool->count = count;
    ass_atomic_store_size(&pool->next_index, 0);
    pool->active_workers = pool->n_workers;
    pool->generation++;
    ass_cond_broadcast(&pool->work_cond);
    ass_mutex_unlock(&pool->lock);

    // The dispatcher participates as the highest-numbered worker.
    run_job(pool, pool->n_workers);

    ass_mutex_lock(&pool->lock);
    while (pool->active_workers > 0)
        ass_cond_wait(&pool->done_cond, &pool->lock);
    ass_mutex_unlock(&pool->lock);
}

size_t ass_get_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t n = si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n >= 1 ? (size_t) n : 1;
}

#endif // CONFIG_THREADS
