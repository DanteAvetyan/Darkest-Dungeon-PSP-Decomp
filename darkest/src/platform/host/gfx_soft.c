/*
 * gfx_soft.c - a tiny SOFTWARE RASTERIZER implementing gfx.h on the PC.
 * It is the CPU doing what the PSP's GPU does, in about 100 lines, and is the
 * best place to learn how triangle drawing works:
 *   for each triangle: find its bounding box; for each pixel in the box compute
 *   BARYCENTRIC weights (w0,w1,w2 = how close the pixel is to each corner);
 *   if all weights are >= 0 the pixel is inside; blend the vertex colours and
 *   texture sample with those weights; then alpha/additive blend into the
 *   framebuffer `fb`.
 * Used for tests and screenshots (gfx_soft_save_ppm writes a .ppm image).
 * Software implementation of gfx.h for testing on a PC: renders into a 480x272 RGBA buffer
 * that host code can save with gfx_soft_save_ppm(). Nearest-neighbour sampling, no filtering. */
#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same fields as the PSP version, minus alignment concerns. */
struct GfxTexture {
    int w, h;
    uint8_t *pix;
    uint8_t clut[256 * 4];
};

/* The framebuffer: 272 rows x 480 columns x RGBA bytes. */
static uint8_t fb[SCREEN_H][SCREEN_W][4];

void gfx_texture_set_clut(GfxTexture *t, const uint8_t *clut) {
    if (t) memcpy(t->clut, clut, sizeof(t->clut));
}
/* Rough display-list size estimate per frame (to check we'd fit the PSP's
 * 1 MB list) and its maximum so far; read by the host main loop. */
long gfx_soft_frame_bytes, gfx_soft_max_bytes;

int gfx_init(void) { return 0; }
void gfx_shutdown(void) {}

/* Copy the pixels and palette into a new texture. */
GfxTexture *gfx_texture_t8(int w, int h, const uint8_t *pixels, const uint8_t *clut) {
    GfxTexture *t = malloc(sizeof(*t));
    t->w = w;
    t->h = h;
    t->pix = malloc((size_t)w * h);
    memcpy(t->pix, pixels, (size_t)w * h);
    memcpy(t->clut, clut, sizeof(t->clut));
    return t;
}

/* On the PC there is no alignment benefit, so "ref" simply copies. */
GfxTexture *gfx_texture_t8_ref(int w, int h, const uint8_t *pixels, const uint8_t *clut) {
    return gfx_texture_t8(w, h, pixels, clut);
}

void gfx_texture_free(GfxTexture *t) {
    if (!t) return;
    free(t->pix);
    free(t);
}

/* Clear the framebuffer to colour c. */
void gfx_begin(uint32_t c) {
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) memcpy(fb[y][x], &c, 4);
}

/* "Present": nothing to show, just update the statistics. */
void gfx_end(void) {
    if (gfx_soft_frame_bytes > gfx_soft_max_bytes) gfx_soft_max_bytes = gfx_soft_frame_bytes;
    gfx_soft_frame_bytes = 0;
}

/* Combine source pixel s with the framebuffer pixel. ADD: dest += src*alpha
 * (clamped). ALPHA: dest = src*a + dest*(1-a), the classic "over" blend. */
static void blend_px(int x, int y, const uint8_t s[4], int mode) {
    uint8_t *d = fb[y][x];
    int a = s[3];
    if (mode == BLEND_ADD) {
        for (int c = 0; c < 3; c++) {
            int v = d[c] + s[c] * a / 255;
            d[c] = v > 255 ? 255 : (uint8_t)v;
        }
        return;
    }
    for (int c = 0; c < 3; c++) d[c] = (uint8_t)((s[c] * a + d[c] * (255 - a)) / 255);
}

/* The edge function: twice the signed area of triangle (a, b, p). Its sign
 * says which side of line a-b the point p is on - the building block of
 * barycentric coordinates. */
static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Rasterize one triangle (see the file comment). Texture lookup is
 * nearest-neighbour: truncate the interpolated UV to a texel and read its
 * palette entry, then multiply by the vertex colour (modulate). */
static void tri(GfxTexture *t, const SpVertex *v0, const SpVertex *v1, const SpVertex *v2, int mode) {
    float area = edge(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (fabsf(area) < 1e-6f) return;
    int minx = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x)));
    int maxx = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int miny = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y)));
    int maxy = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > SCREEN_W - 1) maxx = SCREEN_W - 1;
    if (maxy > SCREEN_H - 1) maxy = SCREEN_H - 1;
    const uint8_t *c0 = (const uint8_t *)&v0->color, *c1 = (const uint8_t *)&v1->color,
                  *c2 = (const uint8_t *)&v2->color;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = edge(v1->x, v1->y, v2->x, v2->y, px, py) / area;
            float w1 = edge(v2->x, v2->y, v0->x, v0->y, px, py) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            uint8_t s[4];
            for (int c = 0; c < 4; c++) s[c] = (uint8_t)(c0[c] * w0 + c1[c] * w1 + c2[c] * w2);
            if (t) {
                int u = (int)(v0->u * w0 + v1->u * w1 + v2->u * w2);
                int vv = (int)(v0->v * w0 + v1->v * w1 + v2->v * w2);
                if (u < 0) u = 0;
                if (vv < 0) vv = 0;
                if (u >= t->w) u = t->w - 1;
                if (vv >= t->h) vv = t->h - 1;
                const uint8_t *tc = &t->clut[t->pix[vv * t->w + u] * 4];
                for (int c = 0; c < 4; c++) s[c] = (uint8_t)(s[c] * tc[c] / 255);
            }
            if (s[3]) blend_px(x, y, s, mode);
        }
    }
}

/* Draw every 3 vertices as a triangle. */
void gfx_triangles(GfxTexture *t, const SpVertex *v, int count, int blend) {
    gfx_soft_frame_bytes += (long)count * sizeof(SpVertex) + 64; /* what the PSP display list would hold */
    for (int i = 0; i + 2 < count; i += 3) tri(t, &v[i], &v[i + 1], &v[i + 2], blend);
}

/* Same quad construction as the PSP file. */
void gfx_sprite(GfxTexture *t, float sx, float sy, float sw, float sh,
                float dx, float dy, float dw, float dh, uint32_t rgba) {
    SpVertex q[6] = {
        {sx, sy, rgba, dx, dy, 0},           {sx + sw, sy, rgba, dx + dw, dy, 0},
        {sx + sw, sy + sh, rgba, dx + dw, dy + dh, 0},
        {sx, sy, rgba, dx, dy, 0},           {sx + sw, sy + sh, rgba, dx + dw, dy + dh, 0},
        {sx, sy + sh, rgba, dx, dy + dh, 0},
    };
    gfx_triangles(t, q, 6, BLEND_ALPHA);
}

void gfx_rect(float x, float y, float w, float h, uint32_t rgba) {
    gfx_sprite(NULL, 0, 0, 0, 0, x, y, w, h, rgba);
}

/* Write the framebuffer as a binary PPM (P6) image: a text header then raw
 * RGB bytes. Convert to PNG to view it. */
int gfx_soft_save_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) fwrite(fb[y][x], 1, 3, f);
    fclose(f);
    return 0;
}
