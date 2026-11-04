/*
 * gfx.h - the drawing interface the game uses.  Two implementations:
 *   PSP:  platform/psp/gfx_psp.c  (sceGu, the PSP's hardware GPU API)
 *   PC:   platform/host/gfx_soft.c (a tiny software rasterizer that can dump
 *         frames to .ppm files, which is how we take test screenshots)
 * Game code only sees this header, so it never knows which one is running.
 *
 * Everything is 2D: textured triangles and rectangles in screen pixels,
 * (0,0) top-left, 480x272 (the PSP's screen).
 *
 * Textures are 8-bit "paletted" (T8): each pixel is an index into a 256-colour
 * palette called a CLUT.  This is 4x smaller than RGBA, which matters on a
 * PSP with ~32 MB RAM, and lets us recolour a texture by swapping only its
 * palette (used for dungeon colour grading).
 */
#ifndef DD_GFX_H
#define DD_GFX_H

#include <stdint.h>
#include "spine.h"

#define SCREEN_W 480
#define SCREEN_H 272

/* Opaque handle; the layout differs per platform. */
typedef struct GfxTexture GfxTexture;

/* Blend modes: normal transparency, or additive (adds light; glows/fire). */
enum { BLEND_ALPHA, BLEND_ADD };

int gfx_init(void);
void gfx_shutdown(void);

/* 8-bit paletted texture; w/h must be powers of two (<= 512). Pixels and CLUT are copied. */
GfxTexture *gfx_texture_t8(int w, int h, const uint8_t *pixels, const uint8_t *clut_rgba);
/* Same, but references the caller's memory (must stay alive; PSP: 16-byte aligned) when possible. */
GfxTexture *gfx_texture_t8_ref(int w, int h, const uint8_t *pixels, const uint8_t *clut_rgba);
void gfx_texture_free(GfxTexture *t);
/* Replace a texture's palette (colour grading). */
void gfx_texture_set_clut(GfxTexture *t, const uint8_t *clut_rgba);

/* Frame bracket: gfx_begin clears the screen, gfx_end shows it. */
void gfx_begin(uint32_t clear_rgba);
void gfx_end(void); /* present */

/* Textured triangle list (tex may be NULL for flat colour). UVs are in texture pixels. */
void gfx_triangles(GfxTexture *tex, const SpVertex *v, int count, int blend);

/* Convenience: axis-aligned rectangles. */
void gfx_rect(float x, float y, float w, float h, uint32_t rgba);
void gfx_sprite(GfxTexture *tex, float sx, float sy, float sw, float sh,
                float dx, float dy, float dw, float dh, uint32_t rgba);

/* Pack a colour into one uint32: byte order R,G,B,A from low to high bits
 * (this is what the PSP GPU's ABGR8888 format expects). */
#define RGBA(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))

#endif
