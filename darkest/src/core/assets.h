/*
 * assets.h - loading of converted game art.
 *
 * The original game's art can't be used directly, so Python tools in
 * darkest/tools convert it into our own small formats (see export_*.py):
 *   .ddi  a still image, cut into tiles, with one 256-colour palette
 *   .dda  a Spine skeletal animation plus its texture page
 * This header is the C side that reads them back.
 */
#ifndef DD_ASSETS_H
#define DD_ASSETS_H

#include "gfx.h"
#include "spine.h"

/* A still image (.ddi).  Big images are split into square tiles because PSP
 * textures max out at 512x512 and must be power-of-two sized.
 * w,h = full size; tile = tile edge; ntx,nty = tiles across/down. */
typedef struct {
    int w, h, tile, ntx, nty;
    GfxTexture **tiles;
    uint8_t *clut0; /* original palette, kept for dungeon art so it can be colour graded */
} Image;

int image_load(Image *img, const char *path);
void image_free(Image *img);
/* Draw the whole image stretched into (x, y, w, h). */
void image_draw(const Image *img, float x, float y, float w, float h, uint32_t rgba);
/* Draw a horizontal window of the image: source x range [sx, sx+sw) of a full-height image. */
void image_draw_part(const Image *img, float sx, float sw, float x, float y, float w, float h, uint32_t rgba);

/* One animation of one sprite: the Spine data plus the GPU texture it uses. */
typedef struct {
    SpAnim anim;
    GfxTexture *tex;
} SpriteAnim;

/* Cached sprite animation from assets/sprites/<sprite>/<anim>.dda; NULL if missing.
 * Stays loaded until sprite_cache_release_all(). */
SpriteAnim *sprite_get(const char *sprite, const char *anim);
void sprite_cache_release_all(void);

/* Draw an animated actor; returns 1 if drawn. */
int sprite_draw(SpriteAnim *s, float t, int loop, float x, float y, float scale, int flip,
                const uint8_t tint[4]);

/* Colour grading (dungeons/<d>/colour_grade_N.png: 16x256 RGB LUT, x = red, y % 16 = green,
 * y / 16 = blue). Dungeon art (assets/dungeons/, assets/sprites/) is re-paletted through the active
 * LUT; NULL = no grading. */
void grade_set(const uint8_t *lut);
/* Pick and apply the LUT for dungeon art dir `dir` (dungeons/<dir>/grade_<band>.lut) at this torch. */
void grade_scene(const char *dir, int torch);
/* 1 while a LUT grades the scene (then the darkness overlay is not needed). */
int grade_active(void);
#endif
