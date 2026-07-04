/*
 * int8k_harness.c -- verification harness for the integration-8k port
 *
 * Renders every *.ass file found in the given directories (fonts are loaded
 * from the same directories, ASS_FONTPROVIDER_NONE, like compare/) at a
 * deterministic set of timestamps derived from the track's events, and:
 *
 *   hash mode (default):
 *       blends each ASS_Image chain into an RGBA8 canvas (same blend as
 *       compare/compare.c) and prints an FNV-1a-64 hash per (file, time).
 *       Used for bitwise A/B between two libass builds and for verifying
 *       thread-count bit-exactness (-t N calls ass_set_render_thread_count).
 *
 *   outline mode (-m outline; not compiled with -DINT8K_NO_OUTLINE):
 *       enables ass_set_outline_deferred(r, 1) and asserts for every emitted
 *       ASS_Image: bitmap == NULL, outline != NULL, n_outline >= 2, and the
 *       tile blob layout n_outline == 2 + n_tiles*TILE_EXPORT_W +
 *       n_segs*SEG_EXPORT_W with n_tiles = outline[0] > 0 and
 *       n_segs = outline[1] > 0.
 *
 * Build (from the repo root, BUILD = a configured meson build dir):
 *   gcc -O2 -I libass -I $BUILD [-DINT8K_NO_OUTLINE] test/int8k_harness.c \
 *       $BUILD/libass/libass.a $(pkg-config --cflags --libs freetype2 \
 *       harfbuzz fribidi fontconfig) -lm -lpthread -o int8k_harness
 *
 * Usage: int8k_harness [-t threads] [-m hash|outline] [-W w] [-H h] dir...
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../libass/ass.h"
#ifndef INT8K_NO_OUTLINE
#include "../libass/ass_rasterizer.h"   /* TILE_EXPORT_W / SEG_EXPORT_W */
#include "../libass/ass_types.h"
#endif

#define MAX_TIMES 64

static ASS_Library *g_lib;

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) fmt; (void) va; (void) data;
    if (level > 4)  /* keep output deterministic: errors/warnings only */
        return;
    fprintf(stderr, "libass: ");
    vfprintf(stderr, fmt, va);
    fputc('\n', stderr);
}

static int has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext);
    return nl > el && !strcasecmp(name + nl - el, ext);
}

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
        if (buf && fread(buf, 1, size, f) == (size_t) size)
            ass_add_font(g_lib, e->d_name, buf, size);
        free(buf);
        fclose(f);
    }
    closedir(d);
}

/* deterministic sample times: 5 points across every event, sorted, deduped */
static int collect_times(ASS_Track *track, long long *times)
{
    int n = 0;
    for (int i = 0; i < track->n_events && n < MAX_TIMES - 5; i++) {
        ASS_Event *ev = track->events + i;
        long long s = ev->Start, d = ev->Duration;
        long long pts[5] = { s, s + d / 4, s + d / 2, s + 3 * d / 4,
                             s + (d > 0 ? d - 1 : 0) };
        for (int k = 0; k < 5; k++)
            times[n++] = pts[k];
    }
    if (!n)
        times[n++] = 0;
    /* sort + dedup */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && times[j - 1] > times[j]; j--) {
            long long t = times[j]; times[j] = times[j - 1]; times[j - 1] = t;
        }
    int m = 1;
    for (int i = 1; i < n; i++)
        if (times[i] != times[m - 1])
            times[m++] = times[i];
    return m;
}

/* --- hash mode ----------------------------------------------------------- */

/* same blend arithmetic as compare/compare.c (blend_image) */
static void blend_image(uint8_t *frame, int32_t fw, int32_t fh,
                        const ASS_Image *img)
{
    if (!img->bitmap)
        return;
    int32_t x0 = img->dst_x > 0 ? img->dst_x : 0;
    int32_t y0 = img->dst_y > 0 ? img->dst_y : 0;
    int32_t x1 = img->dst_x + img->w < fw ? img->dst_x + img->w : fw;
    int32_t y1 = img->dst_y + img->h < fh ? img->dst_y + img->h : fh;
    if (x0 >= x1 || y0 >= y1)
        return;

    uint8_t r = img->color >> 24, g = img->color >> 16;
    uint8_t b = img->color >> 8,  a = img->color;
    int32_t mul = 129 * (255 - a);
    const int32_t offs = (int32_t) 1 << 22;

    int32_t stride = 4 * fw;
    uint8_t *dst = frame + y0 * stride + 4 * x0;
    const uint8_t *src = img->bitmap + (y0 - img->dst_y) * img->stride
                                     + (x0 - img->dst_x);
    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = 0; x < x1 - x0; x++) {
            int32_t k = src[x] * mul;
            dst[4 * x + 0] -= ((dst[4 * x + 0] - r) * k + offs) >> 23;
            dst[4 * x + 1] -= ((dst[4 * x + 1] - g) * k + offs) >> 23;
            dst[4 * x + 2] -= ((dst[4 * x + 2] - b) * k + offs) >> 23;
            dst[4 * x + 3] -= ((dst[4 * x + 3] - 0) * k + offs) >> 23;
        }
        dst += stride;
        src += img->stride;
    }
}

static uint64_t fnv1a64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int run_hash(ASS_Renderer *r, ASS_Track *track, const char *name,
                    int fw, int fh)
{
    long long times[MAX_TIMES];
    int nt = collect_times(track, times);
    uint8_t *canvas = malloc((size_t) 4 * fw * fh);
    if (!canvas)
        return 1;
    for (int i = 0; i < nt; i++) {
        ASS_Image *img = ass_render_frame(r, track, times[i], NULL);
        memset(canvas, 0, (size_t) 4 * fw * fh);
        for (ASS_Image *im = img; im; im = im->next)
            blend_image(canvas, fw, fh, im);
        printf("HASH %s %lld %016llx\n", name, times[i],
               (unsigned long long) fnv1a64(canvas, (size_t) 4 * fw * fh));
    }
    free(canvas);
    return 0;
}

/* --- outline mode -------------------------------------------------------- */

#ifndef INT8K_NO_OUTLINE
static int run_outline(ASS_Renderer *r, ASS_Track *track, const char *name)
{
    long long times[MAX_TIMES];
    int nt = collect_times(track, times);
    int failures = 0, images = 0, frames_with_images = 0;
    for (int i = 0; i < nt; i++) {
        ASS_Image *img = ass_render_frame(r, track, times[i], NULL);
        if (img)
            frames_with_images++;
        for (ASS_Image *im = img; im; im = im->next) {
            images++;
            const char *bad = NULL;
            if (im->bitmap)
                bad = "bitmap != NULL";
            else if (!im->outline)
                bad = "outline == NULL";
            else if (im->n_outline < 2)
                bad = "n_outline < 2";
            else {
                int32_t n_tiles = im->outline[0], n_segs = im->outline[1];
                if (n_tiles <= 0)
                    bad = "n_tiles <= 0";
                else if (n_segs <= 0)
                    bad = "n_segs <= 0";
                else if ((int64_t) im->n_outline !=
                         2 + (int64_t) n_tiles * TILE_EXPORT_W
                           + (int64_t) n_segs * SEG_EXPORT_W)
                    bad = "n_outline != 2 + n_tiles*TILE_EXPORT_W + n_segs*SEG_EXPORT_W";
            }
            if (bad) {
                failures++;
                printf("OUTLINE-FAIL %s %lld img#%d: %s (bitmap=%p outline=%p "
                       "n_outline=%d)\n", name, times[i], images, bad,
                       (void *) im->bitmap, (void *) im->outline,
                       im->n_outline);
            }
        }
    }
    printf("OUTLINE %s: %d frames rendered, %d with images, %d images, "
           "%d failures -> %s\n", name, nt, frames_with_images, images,
           failures, failures || !images ? "FAIL" : "PASS");
    return failures || !images;
}
#endif

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int threads = -1, fw = 1280, fh = 720, outline = 0;
    int argi = 1;
    for (; argi < argc && argv[argi][0] == '-'; argi++) {
        if (!strcmp(argv[argi], "-t") && argi + 1 < argc)
            threads = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-W") && argi + 1 < argc)
            fw = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-H") && argi + 1 < argc)
            fh = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-m") && argi + 1 < argc)
            outline = !strcmp(argv[++argi], "outline");
        else {
            fprintf(stderr, "unknown option '%s'\n", argv[argi]);
            return 2;
        }
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: %s [-t threads] [-m hash|outline] "
                        "[-W w] [-H h] dir...\n", argv[0]);
        return 2;
    }
#ifdef INT8K_NO_OUTLINE
    if (outline) {
        fprintf(stderr, "outline mode not compiled in\n");
        return 2;
    }
#endif

    g_lib = ass_library_init();
    if (!g_lib)
        return 1;
    ass_set_message_cb(g_lib, msg_cb, NULL);
    ass_set_extract_fonts(g_lib, 1);

    for (int d = argi; d < argc; d++)
        load_fonts(argv[d]);

    ASS_Renderer *r = ass_renderer_init(g_lib);
    if (!r)
        return 1;
    ass_set_fonts(r, NULL, NULL, ASS_FONTPROVIDER_NONE, NULL, 0);
    ass_set_storage_size(r, fw, fh);
    ass_set_frame_size(r, fw, fh);
    if (threads >= 0)
        ass_set_render_thread_count(r, threads);
#ifndef INT8K_NO_OUTLINE
    if (outline)
        ass_set_outline_deferred(r, 1);
#endif

    int rc = 0, nsubs = 0;
    for (int d = argi; d < argc; d++) {
        DIR *dir = opendir(argv[d]);
        if (!dir)
            continue;
        /* collect + sort names for deterministic order */
        char *names[256];
        int nn = 0;
        struct dirent *e;
        while ((e = readdir(dir)) && nn < 256)
            if (has_ext(e->d_name, ".ass"))
                names[nn++] = strdup(e->d_name);
        closedir(dir);
        for (int i = 1; i < nn; i++)
            for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
                char *t = names[j]; names[j] = names[j - 1]; names[j - 1] = t;
            }
        for (int i = 0; i < nn; i++) {
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", argv[d], names[i]);
            ASS_Track *track = ass_read_file(g_lib, path, NULL);
            if (!track) {
                fprintf(stderr, "failed to load '%s'\n", path);
                rc = 1;
                continue;
            }
            nsubs++;
#ifndef INT8K_NO_OUTLINE
            if (outline)
                rc |= run_outline(r, track, names[i]);
            else
#endif
                rc |= run_hash(r, track, names[i], fw, fh);
            ass_free_track(track);
            free(names[i]);
        }
    }
    if (!nsubs) {
        fprintf(stderr, "no .ass files found\n");
        rc = 2;
    }
    ass_renderer_done(r);
    ass_library_done(g_lib);
    return rc;
}
