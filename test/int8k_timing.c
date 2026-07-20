/*
 * int8k_timing.c -- WP-K1 per-frame render cost profiler
 *
 * Renders one .ass track at a given frame size across a timestamp window,
 * timing ass_render_frame() per frame and reporting the distribution.
 * Fonts are loaded from the same directory (ASS_FONTPROVIDER_NONE).
 *
 * Build (from the repo root, BUILD = a configured meson build dir):
 *   gcc -O2 -I libass -I $BUILD test/int8k_timing.c \
 *       $BUILD/libass/libass.a $(pkg-config --cflags --libs freetype2 \
 *       harfbuzz fribidi fontconfig) -lm -lpthread -o int8k_timing
 *
 * Usage: int8k_timing [-t threads] [-W w] [-H h] [-s start_s] [-e end_s]
 *                     [-f fps] [-w warmup_frames] [-v] dir file.ass
 */

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "ass.h"

static ASS_Library *g_lib;
static int g_verbose;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    if (level > 4)
        return;
    vfprintf(stderr, fmt, va);
    fprintf(stderr, "\n");
}

static int has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext);
    return nl > el && !strcasecmp(name + nl - el, ext);
}

static int g_nfonts;

static void load_fonts(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "cannot open dir '%s'\n", dir);
        exit(2);
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_ext(e->d_name, ".ttf") && !has_ext(e->d_name, ".otf") &&
            !has_ext(e->d_name, ".pfb"))
            continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc(size);
        if (buf && fread(buf, 1, size, f) == (size_t) size) {
            ass_add_font(g_lib, e->d_name, buf, size);
            g_nfonts++;
        }
        free(buf);
        fclose(f);
    }
    closedir(d);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *) a, y = *(const double *) b;
    return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv)
{
    int threads = -1, fw = 7680, fh = 4320, warmup = 0, cachemb = 0;
    /* WP-K4: -O selects the deferred-OUTLINE path, which is what the rig
     * actually runs (mpv --sub-gpu-raster=yes -> ass_set_outline_deferred).
     * WP-K1 profiled the default CPU-raster path and its conclusions did not
     * transfer; this flag exists so that mistake is not repeated. */
    int outline_deferred = 0;
    /* WP-K4: libass scales by frame_size/storage_size. mpv sets storage to the
     * VIDEO size (sd_ass.c:862, here 1920x1080) and frame to the OUTPUT size,
     * so at 8K everything is scaled 4x. A harness that leaves storage == frame
     * renders at scale 1 and measures the wrong work entirely -- -S sets it. */
    int sw = 0, sh = 0;
    double t0 = 1130.0, t1 = 1140.0, fps = 24000.0 / 1001.0;
    int argi = 1;
    for (; argi < argc && argv[argi][0] == '-'; argi++) {
        if (!strcmp(argv[argi], "-t") && argi + 1 < argc)
            threads = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-W") && argi + 1 < argc)
            fw = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-H") && argi + 1 < argc)
            fh = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-s") && argi + 1 < argc)
            t0 = atof(argv[++argi]);
        else if (!strcmp(argv[argi], "-e") && argi + 1 < argc)
            t1 = atof(argv[++argi]);
        else if (!strcmp(argv[argi], "-f") && argi + 1 < argc)
            fps = atof(argv[++argi]);
        else if (!strcmp(argv[argi], "-w") && argi + 1 < argc)
            warmup = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-c") && argi + 1 < argc)
            cachemb = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-v"))
            g_verbose = 1;
        else if (!strcmp(argv[argi], "-O"))
            outline_deferred = 1;
        else if (!strcmp(argv[argi], "-S") && argi + 2 < argc) {
            sw = atoi(argv[++argi]);
            sh = atoi(argv[++argi]);
        }
        else {
            fprintf(stderr, "unknown option '%s'\n", argv[argi]);
            return 2;
        }
    }
    if (argi + 1 >= argc) {
        fprintf(stderr, "usage: %s [-t n] [-W w] [-H h] [-s t0] [-e t1] "
                        "[-f fps] [-w warmup] [-v] fontdir file.ass\n", argv[0]);
        return 2;
    }
    const char *fontdir = argv[argi];
    const char *assfile = argv[argi + 1];

    g_lib = ass_library_init();
    if (!g_lib)
        return 1;
    ass_set_message_cb(g_lib, msg_cb, NULL);
    ass_set_extract_fonts(g_lib, 1);
    load_fonts(fontdir);
    fprintf(stderr, "loaded %d fonts from %s\n", g_nfonts, fontdir);

    ASS_Renderer *r = ass_renderer_init(g_lib);
    if (!r)
        return 1;
    ass_set_fonts(r, NULL, NULL, ASS_FONTPROVIDER_NONE, NULL, 0);
    ass_set_storage_size(r, sw ? sw : fw, sh ? sh : fh);
    ass_set_frame_size(r, fw, fh);
    if (threads >= 0)
        ass_set_render_thread_count(r, threads);
    if (cachemb)
        ass_set_cache_limits(r, 0, cachemb);
    if (outline_deferred)
        ass_set_outline_deferred(r, 1);
    fprintf(stderr, "outline-deferred mode: %s  storage %dx%d -> frame %dx%d (scale %.2f)\n",
            outline_deferred ? "ON" : "off", sw ? sw : fw, sh ? sh : fh, fw, fh,
            (double) fh / (sh ? sh : fh));

    ASS_Track *track = ass_read_file(g_lib, (char *) assfile, NULL);
    if (!track) {
        fprintf(stderr, "failed to load '%s'\n", assfile);
        return 1;
    }

    int nframes = (int) ((t1 - t0) * fps) + 1;
    double *ms = malloc(nframes * sizeof(double));
    int n = 0;
    long long total_imgs = 0;
    long long total_blob = 0, max_blob = 0;

    /* warmup frames at t0 (populate caches) are timed but excluded */
    for (int i = 0; i < warmup; i++) {
        int ch;
        ass_render_frame(r, track, (long long) (t0 * 1000.0), &ch);
    }

    for (int i = 0; i < nframes; i++) {
        double t = t0 + i / fps;
        long long tms = (long long) (t * 1000.0 + 0.5);
        int ch = 0;
        double a = now_ms();
        ASS_Image *img = ass_render_frame(r, track, tms, &ch);
        double b = now_ms();
        int nimg = 0;
        /* WP-K4: in outline-deferred mode every ASS_Image carries an int32
         * tile/segment blob that the CONSUMER must copy (mpv packer.c:351
         * ta_memdup) and later byte-compare (vo_gpu_next rkey). libass's own
         * render is nearly free here, so the blob VOLUME -- not libass time --
         * is what the downstream pays per frame. Measure it. */
        long long blob_b = 0;
        for (ASS_Image *p = img; p; p = p->next) {
            nimg++;
            blob_b += (long long) p->n_outline * (long long) sizeof(int32_t);
        }
        total_imgs += nimg;
        total_blob += blob_b;
        if (blob_b > max_blob)
            max_blob = blob_b;
        ms[n++] = b - a;
        if (g_verbose)
            printf("t=%.3f  %8.3f ms  imgs=%d blobMB=%.2f changed=%d\n",
                   t, b - a, nimg, blob_b / 1048576.0, ch);
    }

    double sum = 0, max = 0;
    for (int i = 0; i < n; i++) {
        sum += ms[i];
        if (ms[i] > max)
            max = ms[i];
    }
    double *sorted = malloc(n * sizeof(double));
    memcpy(sorted, ms, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);

    printf("=== %dx%d  t=%.2f..%.2f  fps=%.3f  threads=%d  frames=%d ===\n",
           fw, fh, t0, t1, fps, threads, n);
    printf("mean %.3f  p50 %.3f  p90 %.3f  p95 %.3f  p99 %.3f  max %.3f ms\n",
           sum / n, sorted[n / 2], sorted[(int) (n * 0.90)],
           sorted[(int) (n * 0.95)], sorted[(int) (n * 0.99)], max);
    printf("avg images/frame %.1f\n", (double) total_imgs / n);
    printf("outline blob: mean %.2f MB/frame  max %.2f MB/frame\n",
           total_blob / 1048576.0 / n, max_blob / 1048576.0);

    ass_free_track(track);
    ass_renderer_done(r);
    ass_library_done(g_lib);
    return 0;
}
