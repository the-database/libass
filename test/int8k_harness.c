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
 *   blurdefer mode (-m blurdefer):
 *       self-contained fix check for ass_set_blur_deferred: renders a \be2-only
 *       event and asserts the deferred bitmap matches the non-deferred render
 *       with blur_x==blur_y==0, plus a \be2\blur3 event asserting the gaussian
 *       is deferred (blur_x/y>0) and \be is still applied. Needs a font dir
 *       only (for "Aileron"); the .ass corpus is ignored.
 *
 * Usage: int8k_harness [-t threads] [-m hash|outline|blurdefer] [-W w] [-H h] dir...
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

/* --- blur-deferred mode (verifies the \be fix in blur-deferred-only mode) --
 *
 * ass_set_blur_deferred(1) defers ONLY the gaussian \blur (recorded as
 * ASS_Image.blur_x/blur_y); the box blur \be must still be applied on the CPU.
 * Self-contained: builds one borderless white event in memory, so it needs no
 * .ass corpus -- only a font dir (for "Aileron") on the command line. */

static const char *BLURDEFER_TEMPLATE =
    "[Script Info]\n"
    "PlayResX: 1280\nPlayResY: 720\nScaledBorderAndShadow: yes\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
    "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
    "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
    "MarginR, MarginV, Encoding\n"
    "Style: T,Aileron,72,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,"
    "100,100,0,0,1,0,0,5,10,10,10,1\n\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, "
    "Text\n"
    "Dialogue: 0,0:00:00.00,0:00:10.00,T,,0,0,0,,{%s}Blur Wg\n";

/* Render the one-event track built from `override` at t=1000ms with the given
 * blur_deferred setting; returns the blended-canvas hash and reports the max
 * emitted blur_x/blur_y over the image chain. */
static uint64_t blurdefer_render(ASS_Renderer *r, int fw, int fh,
                                 const char *override, int deferred,
                                 double *max_bx, double *max_by)
{
    char doc[1024];
    int n = snprintf(doc, sizeof(doc), BLURDEFER_TEMPLATE, override);
    ASS_Track *track = ass_read_memory(g_lib, doc, (size_t) n, NULL);
    if (!track) {
        fprintf(stderr, "blurdefer: ass_read_memory failed\n");
        exit(1);
    }
    ass_set_blur_deferred(r, deferred);
    ASS_Image *img = ass_render_frame(r, track, 1000, NULL);
    uint8_t *canvas = calloc(1, (size_t) 4 * fw * fh);
    double bx = 0, by = 0;
    for (ASS_Image *im = img; im; im = im->next) {
        if (im->blur_x > bx) bx = im->blur_x;
        if (im->blur_y > by) by = im->blur_y;
        blend_image(canvas, fw, fh, im);
    }
    uint64_t h = fnv1a64(canvas, (size_t) 4 * fw * fh);
    free(canvas);
    ass_free_track(track);
    if (max_bx) *max_bx = bx;
    if (max_by) *max_by = by;
    return h;
}

static int run_blurdefer(ASS_Renderer *r, int fw, int fh)
{
    int fails = 0;
    double bx, by, bx2, by2;

    /* Case 1 (\be only): the deferred render must match the non-deferred render
     * bit-for-bit -- the box blur is applied on the CPU in both -- and no
     * gaussian may be deferred (blur_x == blur_y == 0). */
    uint64_t ref = blurdefer_render(r, fw, fh, "\\be2", 0, &bx,  &by);
    uint64_t def = blurdefer_render(r, fw, fh, "\\be2", 1, &bx2, &by2);
    int c1 = (def == ref) && bx2 == 0.0 && by2 == 0.0;
    printf("BLURDEFER be-only: ref=%016llx def=%016llx blur=(%.3f,%.3f) -> %s\n",
           (unsigned long long) ref, (unsigned long long) def, bx2, by2,
           c1 ? "PASS" : "FAIL");
    fails += !c1;

    /* Case 2 (\be + \blur): the gaussian must be deferred (blur_x/blur_y > 0)
     * and \be must still be applied. The exact pre-gaussian intermediate is not
     * reachable through the public API, so the documented discriminator is:
     * with the fix the deferred bitmap differs from the pure-unblurred coverage;
     * without the fix \be is dropped and it would equal that raw coverage. */
    uint64_t plain  = blurdefer_render(r, fw, fh, "",           1, &bx,  &by);
    uint64_t beblur = blurdefer_render(r, fw, fh, "\\be2\\blur3", 1, &bx2, &by2);
    int c2 = (bx2 > 0.0) && (by2 > 0.0) && (beblur != plain);
    printf("BLURDEFER be+blur: plain=%016llx beblur=%016llx blur=(%.3f,%.3f) -> %s\n",
           (unsigned long long) plain, (unsigned long long) beblur, bx2, by2,
           c2 ? "PASS" : "FAIL");
    fails += !c2;

    printf("BLURDEFER: %d failures -> %s\n", fails, fails ? "FAIL" : "PASS");
    return fails != 0;
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int threads = -1, fw = 1280, fh = 720;
    int mode = 0;   /* 0 = hash, 1 = outline, 2 = blurdefer */
    int argi = 1;
    for (; argi < argc && argv[argi][0] == '-'; argi++) {
        if (!strcmp(argv[argi], "-t") && argi + 1 < argc)
            threads = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-W") && argi + 1 < argc)
            fw = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-H") && argi + 1 < argc)
            fh = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "-m") && argi + 1 < argc) {
            const char *mv = argv[++argi];
            mode = !strcmp(mv, "outline")   ? 1 :
                   !strcmp(mv, "blurdefer") ? 2 : 0;
        } else {
            fprintf(stderr, "unknown option '%s'\n", argv[argi]);
            return 2;
        }
    }
    if (argi >= argc) {
        fprintf(stderr, "usage: %s [-t threads] [-m hash|outline|blurdefer] "
                        "[-W w] [-H h] dir...\n", argv[0]);
        return 2;
    }
#ifdef INT8K_NO_OUTLINE
    if (mode == 1) {
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
    if (mode == 1)
        ass_set_outline_deferred(r, 1);
#endif

    if (mode == 2) {   /* self-contained; ignores the .ass corpus */
        int brc = run_blurdefer(r, fw, fh);
        ass_renderer_done(r);
        ass_library_done(g_lib);
        return brc;
    }

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
            if (mode == 1)
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
