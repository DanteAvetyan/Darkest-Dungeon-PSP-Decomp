/*
 * gfx_psp.c - gfx.h on real PSP hardware using sceGu (the PSP's GPU, called
 * the "GE", graphics engine).
 *
 * HOW THE GE WORKS (the short version):
 *   - You don't draw directly. You fill a DISPLAY LIST (a command buffer in
 *     RAM: `list` below) with state changes and draw calls, then tell the GE to
 *     run it. sceGuStart opens a list, sceGuFinish closes it, sceGuSync waits
 *     for the GPU to finish.
 *   - Two screen buffers live in VRAM (double buffering): one is displayed
 *     while the other is drawn into; sceGuSwapBuffers flips them.
 *   - The GE reads textures from RAM through its own cache. After CPU writes
 *     we must "write back" the data cache (sceKernelDcacheWritebackRange) so
 *     the GE sees the new bytes; textures and palettes must be 16-byte aligned.
 *
 * sceGu implementation of gfx.h. 32-bit double-buffered framebuffer in VRAM, textures live in
 * main RAM (8-bit CLUT, 16-byte aligned) and are written back from the data cache on creation. */
#include "gfx.h"

#include <malloc.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdint.h>
#include <string.h>

/* Buffer row width is 512 pixels (a GE requirement) though the screen is
 * 480 wide; FRAME_BYTES is one 32-bit buffer so the second starts after it. */
#define BUF_W 512
#define FRAME_BYTES (BUF_W * SCREEN_H * 4)

/* Our texture: size, 8-bit indices, and a 256-colour palette (CLUT).
 * `owned` says whether we malloc'd the memory (and must free it). */
struct GfxTexture {
    int w, h;
    uint8_t *pix;   /* 16-byte aligned */
    uint32_t *clut; /* 16-byte aligned, 256 entries */
    int owned;      /* pix/clut were allocated here */
};

/* The GE display list: 1 MB of command space, 16-byte aligned. */
static unsigned int __attribute__((aligned(16))) list[256 * 1024];
/* The texture currently bound to the GE, so repeated draws with the same
 * texture skip the (slow) re-binding. */
static GfxTexture *bound;

/* One-time GPU setup: draw buffer at VRAM 0, display buffer after it, screen
 * offsets/viewport (the GE's coordinate space is 4096x4096 with the screen
 * in the middle, hence the 2048 offsets), scissor, no depth test, smooth
 * shading, standard alpha blending, modulate texture colour by vertex colour,
 * bilinear filtering, clamped edges, CLUT in RGBA8888. */
int gfx_init(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCREEN_W, SCREEN_H, (void *)FRAME_BYTES, BUF_W);
    sceGuOffset(2048 - SCREEN_W / 2, 2048 - SCREEN_H / 2);
    sceGuViewport(2048, 2048, SCREEN_W, SCREEN_H);
    sceGuScissor(0, 0, SCREEN_W, SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    return 0;
}

void gfx_shutdown(void) { sceGuTerm(); }

/* Create a texture by COPYING pixels and palette into aligned memory
 * (memalign(16, ...)), then flush the data cache so the GE can read them. */
GfxTexture *gfx_texture_t8(int w, int h, const uint8_t *pixels, const uint8_t *clut) {
    GfxTexture *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    t->pix = memalign(16, (size_t)w * h);
    t->clut = memalign(16, 256 * 4);
    if (!t->pix || !t->clut) {
        free(t->pix);
        free(t->clut);
        free(t);
        return NULL;
    }
    memcpy(t->pix, pixels, (size_t)w * h);
    memcpy(t->clut, clut, 256 * 4);
    sceKernelDcacheWritebackRange(t->pix, w * h);
    sceKernelDcacheWritebackRange(t->clut, 256 * 4);
    t->owned = 1;
    return t;
}

/* Create a texture that POINTS at the caller's memory instead of copying -
 * saves RAM for big sprite pages. Only possible when both pointers are
 * already 16-byte aligned; otherwise falls back to copying. */
GfxTexture *gfx_texture_t8_ref(int w, int h, const uint8_t *pixels, const uint8_t *clut) {
    if (((uintptr_t)pixels & 15) || ((uintptr_t)clut & 15)) return gfx_texture_t8(w, h, pixels, clut);
    GfxTexture *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    t->pix = (uint8_t *)pixels;
    t->clut = (uint32_t *)clut;
    t->owned = 0;
    sceKernelDcacheWritebackRange(t->pix, w * h);
    sceKernelDcacheWritebackRange(t->clut, 256 * 4);
    return t;
}

/* Free; only releases memory we own (a _ref texture belongs to the caller). */
void gfx_texture_free(GfxTexture *t) {
    if (!t) return;
    if (bound == t) bound = NULL;
    if (t->owned) {
        free(t->pix);
        free(t->clut);
    }
    free(t);
}

/* Start a frame: open the display list and clear the screen. */
void gfx_begin(uint32_t clear) {
    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    bound = NULL;
}

/* End a frame: close and run the list, wait for the GPU, wait for the vertical
 * blank (so we never tear and the game runs at 60 fps), then flip buffers. */
void gfx_end(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

/* Replace the palette (colour grading). Clearing `bound` forces the GE to
 * reload the CLUT next time this texture is used. */
void gfx_texture_set_clut(GfxTexture *t, const uint8_t *clut) {
    if (!t) return;
    memcpy(t->clut, clut, 256 * 4);
    sceKernelDcacheWritebackRange(t->clut, 256 * 4);
    if (bound == t) bound = NULL; /* reload the CLUT on the next bind */
}

/* Make `t` the active texture (or disable texturing for flat colour). Loads
 * its palette and describes the image to the GE. */
static void bind(GfxTexture *t) {
    if (!t) {
        sceGuDisable(GU_TEXTURE_2D);
        bound = NULL;
        return;
    }
    sceGuEnable(GU_TEXTURE_2D);
    if (bound == t) return;
    sceGuClutLoad(256 / 8, t->clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, t->w, t->h, t->w, t->pix);
    sceGuTexFlush();
    bound = t;
}

/* Draw a triangle list. Vertices must be copied INTO the display list memory
 * (sceGuGetMemory) because the GE reads them later, after this call returns -
 * the caller's array may be gone by then. GU_TRANSFORM_2D means coordinates
 * are already screen pixels (no 3D projection). Additive blending adds the
 * source colour to the screen instead of mixing (for glows). */
void gfx_triangles(GfxTexture *t, const SpVertex *v, int count, int blend) {
    if (count <= 0) return;
    bind(t);
    if (blend == BLEND_ADD) sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xffffffff);
    /* Vertices must live in memory the GE can read after this call returns: copy into the list. */
    SpVertex *dl = sceGuGetMemory(count * sizeof(SpVertex));
    memcpy(dl, v, count * sizeof(SpVertex));
    sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   count, NULL, dl);
    if (blend == BLEND_ADD) sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

/* A textured rectangle = 2 triangles (6 vertices). sx,sy,sw,sh select the
 * source region in texture pixels; dx,dy,dw,dh the destination on screen;
 * rgba tints (and fades) it. */
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

/* A flat-colour rectangle: a sprite with no texture. */
void gfx_rect(float x, float y, float w, float h, uint32_t rgba) {
    gfx_sprite(NULL, 0, 0, 0, 0, x, y, w, h, rgba);
}
