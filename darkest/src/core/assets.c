/*
 * assets.c - image (.ddi) loading, the sprite cache, and colour grading.
 *
 * .ddi layout (little-endian): "DDI1" | w u16 | h u16 | tile u16 | ntx u16 |
 *   nty u16 | 2 pad bytes | CLUT (256 * RGBA = 1024 bytes) | pixel bytes of
 *   ntx*nty tiles, each tile*tile bytes of palette indices.
 *
 * COLOUR GRADING: the original game tints each dungeon by how bright the torch
 * is (a "colour grade" lookup table, LUT).  We can't regrade every pixel each
 * frame on a PSP, but our textures are paletted, so we only regrade the 256
 * palette entries and upload the new palette.  Cheap!
 */
#include "assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pack.h"
#include "platform.h"

/* Little-endian u16 read. */
static uint16_t u16_at(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* ---------------------------------------------------------------- colour grading */

/* The active LUT (NULL = no grading). LUT is 16x16x16 RGB entries: for each
 * (r,g,b) quantised to 16 levels it stores the graded RGB triple. */
static const uint8_t *grade_lut;
/* Images that must be re-palettised when the grade changes (only dungeon art). */
#define GRADED_MAX 48
static Image *graded[GRADED_MAX];
static int ngraded;

/* One channel c of the LUT cell (r,g,b); index = ((b*16+g)*16+r)*3+c. */
static float lut_at(const uint8_t *lut, int r, int g, int b, int c) { return lut[((b * 16 + g) * 16 + r) * 3 + c]; }

/* Run a whole 256-entry RGBA palette through the LUT.
 * A colour rarely lands exactly on a LUT cell, so we blend the 8 surrounding
 * cells (trilinear interpolation): i0/i1 = the cell below/above per channel,
 * f = how far between them, weight w = product of the three per-axis weights.
 * Alpha is copied unchanged. */
static void grade_clut(const uint8_t *src, uint8_t *dst, const uint8_t *lut) {
    for (int i = 0; i < 256; i++) {
        const uint8_t *s = src + i * 4;
        uint8_t *d = dst + i * 4;
        d[3] = s[3];
        if (!lut) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            continue;
        }
        float f[3];
        int i0[3], i1[3];
        for (int c = 0; c < 3; c++) {
            float v = s[c] * 15.0f / 255.0f;
            i0[c] = (int)v;
            i1[c] = i0[c] < 15 ? i0[c] + 1 : 15;
            f[c] = v - i0[c];
        }
        for (int c = 0; c < 3; c++) {
            float acc = 0;
            for (int k = 0; k < 8; k++) {
                int r = k & 1 ? i1[0] : i0[0], g = k & 2 ? i1[1] : i0[1], b = k & 4 ? i1[2] : i0[2];
                float w = (k & 1 ? f[0] : 1 - f[0]) * (k & 2 ? f[1] : 1 - f[1]) * (k & 4 ? f[2] : 1 - f[2]);
                acc += w * lut_at(lut, r, g, b, c);
            }
            d[c] = (uint8_t)(acc + 0.5f);
        }
    }
}

/* Apply the active grade to every tile of an image (they share one palette). */
static void grade_image(Image *img) {
    if (!img->clut0 || !img->tiles) return;
    uint8_t c[1024];
    grade_clut(img->clut0, c, grade_lut);
    for (int i = 0; i < img->ntx * img->nty; i++) gfx_texture_set_clut(img->tiles[i], c);
}

/* Load a .ddi into GPU tiles. Returns 0 on success, negative on error
 * (-1 missing, -2 not a DDI1 file, -3 truncated). */
int image_load(Image *img, const char *path) {
    memset(img, 0, sizeof(*img));
    int size = 0;
    uint8_t *d = asset_read(path, &size);
    if (!d) {
        plat_log("missing %s", path);
        return -1;
    }
    if (size < 16 || memcmp(d, "DDI1", 4) != 0) {
        free(d);
        return -2;
    }
    img->w = u16_at(d + 4);
    img->h = u16_at(d + 6);
    img->tile = u16_at(d + 8);
    img->ntx = u16_at(d + 10);
    img->nty = u16_at(d + 12);
    const uint8_t *clut = d + 16;
    const uint8_t *pix = clut + 1024;
    int n = img->ntx * img->nty, ts = img->tile * img->tile;
    if (16 + 1024 + n * ts > size) {
        free(d);
        return -3;
    }
    img->tiles = calloc(n, sizeof(GfxTexture *));
    for (int i = 0; i < n; i++) img->tiles[i] = gfx_texture_t8(img->tile, img->tile, pix + i * ts, clut);
    /* Remember the original palette so regrading always starts from it. */
    if (strstr(path, "assets/dungeons/") && ngraded < GRADED_MAX) { /* dungeon art follows the colour grade */
        img->clut0 = malloc(1024);
        if (img->clut0) {
            memcpy(img->clut0, clut, 1024);
            graded[ngraded++] = img;
            if (grade_lut) grade_image(img);
        }
    }
    free(d);
    return 0;
}

/* Release GPU tiles. The graded[] list is a swap-remove: overwrite our slot
 * with the last entry (graded[i--] re-checks that slot). */
void image_free(Image *img) {
    for (int i = 0; i < ngraded; i++)
        if (graded[i] == img) graded[i--] = graded[--ngraded];
    free(img->clut0);
    img->clut0 = NULL;
    if (!img->tiles) return;
    for (int i = 0; i < img->ntx * img->nty; i++) gfx_texture_free(img->tiles[i]);
    free(img->tiles);
    memset(img, 0, sizeof(*img));
}

/* Draw source columns [sx, sx+sw) of the image into the dest rect. Needed for
 * scrolling backgrounds. Loops over tiles, clips each to the window, and
 * scales by kx/ky (dest size / source size). */
void image_draw_part(const Image *img, float sx, float sw, float x, float y, float w, float h, uint32_t rgba) {
    if (!img->tiles || sw <= 0) return;
    float kx = w / sw, ky = h / img->h;
    for (int ty = 0; ty < img->nty; ty++) {
        for (int tx = 0; tx < img->ntx; tx++) {
            float t0 = tx * img->tile, t1 = t0 + img->tile;
            float th = img->tile;
            if (t1 > img->w) t1 = img->w;
            if ((ty + 1) * img->tile > img->h) th = img->h - ty * img->tile;
            float a = t0 > sx ? t0 : sx, b = t1 < sx + sw ? t1 : sx + sw;
            if (b <= a || th <= 0) continue;
            gfx_sprite(img->tiles[ty * img->ntx + tx], a - t0, 0, b - a, th,
                       x + (a - sx) * kx, y + ty * img->tile * ky, (b - a) * kx, th * ky, rgba);
        }
    }
}

/* Whole image stretched to a rect. */
void image_draw(const Image *img, float x, float y, float w, float h, uint32_t rgba) {
    image_draw_part(img, 0, img->w, x, y, w, h, rgba);
}

/* ---------------------------------------------------------------- sprite cache */

/* Decoded sprites are cached by "sprite/anim" key so each file is loaded once.
 * Fixed size array; a miss is remembered (ok=0) so we don't retry every frame. */
#define SPRITE_CACHE 64

typedef struct {
    char key[64];
    SpriteAnim s;
    int ok;
    uint8_t clut0[1024]; /* original palette (colour grading) */
} CacheEntry;

static CacheEntry cache[SPRITE_CACHE];
static int ncache;

/* Look up or load assets/sprites/<sprite>/<anim>.dda. */
SpriteAnim *sprite_get(const char *sprite, const char *anim) {
    char key[64];
    snprintf(key, sizeof(key), "%s/%s", sprite, anim);
    for (int i = 0; i < ncache; i++)
        if (strcmp(cache[i].key, key) == 0) return cache[i].ok ? &cache[i].s : NULL;
    if (ncache == SPRITE_CACHE) return NULL;
    CacheEntry *e = &cache[ncache++];
    memset(e, 0, sizeof(*e));
    strcpy(e->key, key);
    char path[128];
    snprintf(path, sizeof(path), "assets/sprites/%s.dda", key);
    int size;
    void *blob = asset_read(path, &size);
    if (blob && sp_anim_load(&e->s.anim, blob, size) == 0) {
        memcpy(e->clut0, e->s.anim.clut, 1024);
        e->s.tex = gfx_texture_t8_ref(e->s.anim.page_w, e->s.anim.page_h, e->s.anim.pixels, e->s.anim.clut);
        e->ok = e->s.tex != NULL;
        if (e->ok && grade_lut) {
            uint8_t c[1024];
            grade_clut(e->clut0, c, grade_lut);
            gfx_texture_set_clut(e->s.tex, c);
        }
    } else {
        plat_log("sprite fail %s (%s)", path, blob ? "bad data" : "missing");
        free(blob);
    }
    return e->ok ? &e->s : NULL;
}

/* Switch the active LUT and immediately repaint all loaded graded images and
 * cached sprites by rebuilding their palettes from the saved originals. */
void grade_set(const uint8_t *lut) {
    grade_lut = lut;
    for (int i = 0; i < ngraded; i++) grade_image(graded[i]);
    uint8_t c[1024];
    for (int i = 0; i < ncache; i++) {
        if (!cache[i].ok) continue;
        grade_clut(cache[i].clut0, c, lut);
        gfx_texture_set_clut(cache[i].s.tex, c);
    }
}

int grade_active(void) { return grade_lut != NULL; }

/* Choose the LUT file for a dungeon at the current torch level. The torch
 * (0-100) is split into 5 bands. Re-reading only happens when the band changes. */
void grade_scene(const char *dir, int torch) {
    static uint8_t lut[16 * 256 * 3];
    static char cur[48];
    char key[48];
    /* bands as the darkness table: 0 = no light .. 4 = 76-100 (colour_grade_0..4) */
    int band = torch > 75 ? 4 : torch > 50 ? 3 : torch > 25 ? 2 : torch > 0 ? 1 : 0;
    snprintf(key, sizeof(key), "%s/%d", dir ? dir : "", band);
    if (dir && strcmp(key, cur) == 0) return;
    if (!dir) {
        cur[0] = 0;
        grade_set(NULL);
        return;
    }
    snprintf(cur, sizeof(cur), "%s", key);
    char path[96];
    snprintf(path, sizeof(path), "assets/dungeons/%s/grade_%d.lut", dir, band);
    int size = 0;
    uint8_t *d = asset_read(path, &size);
    if (d && size == (int)sizeof(lut)) {
        memcpy(lut, d, sizeof(lut));
        grade_set(lut);
    } else {
        grade_set(NULL);
    }
    free(d);
}

/* Free all cached sprites (each scene does this on enter to save RAM). */
void sprite_cache_release_all(void) {
    for (int i = 0; i < ncache; i++) {
        if (!cache[i].ok) continue;
        gfx_texture_free(cache[i].s.tex);
        sp_anim_free(&cache[i].s.anim);
    }
    ncache = 0;
}

/* Draw one animation frame at time t (seconds). Steps: sp_pose samples the
 * skeleton at t; sp_emit builds triangles for it. Two passes: pass 0 = normal
 * alpha parts, pass 1 = additive parts (glows). x,y = feet position. */
int sprite_draw(SpriteAnim *s, float t, int loop, float x, float y, float scale, int flip,
                const uint8_t tint[4]) {
    static SpPose pose;
    static SpVertex verts[12288];
    if (!s) return 0;
    sp_pose(&pose, &s->anim, t, loop);
    for (int pass = 0; pass < 2; pass++) {
        int n = sp_emit(&pose, x, y, scale, flip, tint, pass, verts, 12288);
        gfx_triangles(s->tex, verts, n, pass ? BLEND_ADD : BLEND_ALPHA);
    }
    return 1;
}
