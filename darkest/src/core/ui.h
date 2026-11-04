/*
 * ui.h - small shared UI toolkit used by every scene: colours, input edge
 * detection, panels, bars, menus, floating combat text and per-class art.
 */
#ifndef DD_UI_H
#define DD_UI_H

#include <stdint.h>
#include "assets.h"
#include "gfx.h"
#include "text.h"

/* Palette tuned to the game's dark, muted look. */
#define COL_TEXT RGBA(225, 215, 190, 255)
#define COL_DIM RGBA(140, 130, 115, 255)
#define COL_GOLD RGBA(230, 190, 90, 255)
#define COL_RED RGBA(200, 40, 30, 255)
#define COL_GOOD RGBA(120, 200, 110, 255)
#define COL_PANEL RGBA(18, 14, 12, 225)
#define COL_EDGE RGBA(110, 90, 60, 255)
#define COL_SELECT RGBA(230, 190, 90, 255)

/* Input: edge-triggered presses for this frame. */
void input_update(uint32_t held);
int input_pressed(uint32_t btn);
int input_held(uint32_t btn);

void ui_panel(float x, float y, float w, float h);
void ui_bar(float x, float y, float w, float h, float frac, uint32_t fill, uint32_t back);
void ui_frame(float x, float y, float w, float h, uint32_t col); /* outline only */
/* Torch vignette (our addition, not in the original): edges darken and the dark creeps towards the
 * centre as torch (0..100) falls. Gouraud-shaded grid over the rectangle. */
void ui_vignette(float x, float y, float w, float h, int torch);
/* Vertical menu; returns nothing, draws items with `sel` highlighted. */
void ui_menu(float x, float y, float w, const char *const *items, int n, int sel);
/* Button hints along the bottom edge, e.g. "X Select  O Back". */
void ui_hints(const char *text);
void ui_hints_top(const char *text); /* same, centred in the top bar */
/* Moves a menu cursor with up/down; returns the new selection. */
int ui_menu_input(int sel, int n);

/* Floating combat text. */
void ui_popup(float x, float y, uint32_t col, const char *fmt, ...);
void ui_popups_update(float dt);
void ui_popups_draw(void);
void ui_popups_clear(void);

/* Lazily loaded per-class art (NULL if missing). */
Image *ui_portrait(int hero_cls);
Image *ui_skill_icon(int hero_cls, int skill);
void ui_art_release(void);

#endif
