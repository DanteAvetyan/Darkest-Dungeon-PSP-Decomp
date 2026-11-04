#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dd_types.h"
#include "platform.h"
#include "gfx.h"
#include "spine.h"

/* ---------------------------------------------------------------- input */

/* The platform gives us the buttons HELD this frame. A menu needs "pressed
 * this frame" (a rising edge), otherwise holding Down would scroll at 60Hz.
 * We keep last frame's state and compare. */
static uint32_t held_now, held_prev;

void input_update(uint32_t held) {
    held_prev = held_now;
    held_now = held;
}
int input_pressed(uint32_t btn) { return (held_now & btn) && !(held_prev & btn); }
int input_held(uint32_t btn) { return (held_now & btn) != 0; }

/* Up/Down moves a cursor through n items, wrapping at both ends. */
int ui_menu_input(int sel, int n) {
    if (n <= 0) return 0;
    if (input_pressed(BTN_UP)) sel = (sel + n - 1) % n;
    if (input_pressed(BTN_DOWN)) sel = (sel + 1) % n;
    return sel;
}

/* ---------------------------------------------------------------- drawing */

/* Darkness overlay. A GX x GY grid of vertices each gets a black alpha value:
 * 0 near the centre, rising towards the corners (smoothstep for a soft edge).
 * The GPU interpolates alpha across each cell (Gouraud shading), giving a
 * smooth vignette from only ~128 cells. Lower torch => stronger and larger. */
void ui_vignette(float x, float y, float w, float h, int torch) {
    enum { GX = 16, GY = 8 };
    float t = 1.0f - (torch < 0 ? 0 : torch > 100 ? 100 : torch) / 100.0f; /* 0 full light .. 1 none */
    if (t < 0.02f) t = 0.02f; /* a faint vignette even at full light */
    float strength = 0.15f + 0.75f * t;   /* alpha at the corners */
    float inner = 0.80f - 0.55f * t;      /* clear radius (1 = corner) */
    uint8_t a[GY + 1][GX + 1];
    for (int j = 0; j <= GY; j++)
        for (int i = 0; i <= GX; i++) {
            float dx = (float)i / GX * 2 - 1, dy = (float)j / GY * 2 - 1;
            float d = sqrtf((dx * dx + dy * dy) * 0.5f); /* 0 centre .. 1 corner */
            float k = (d - inner) / (1.0f - inner);
            k = k < 0 ? 0 : k > 1 ? 1 : k;
            k = k * k * (3 - 2 * k); /* smoothstep */
            a[j][i] = (uint8_t)(255 * strength * k);
        }
    static SpVertex v[GX * GY * 6];
    int n = 0;
    for (int j = 0; j < GY; j++)
        for (int i = 0; i < GX; i++) {
            if (!a[j][i] && !a[j][i + 1] && !a[j + 1][i] && !a[j + 1][i + 1]) continue; /* clear cell */
            const int ci[6] = {0, 1, 2, 1, 3, 2};
            for (int c = 0; c < 6; c++) {
                int ox = (ci[c] & 1), oy = (ci[c] >> 1);
                SpVertex *p = &v[n++];
                p->u = p->v = 0;
                p->x = x + (i + ox) * w / GX;
                p->y = y + (j + oy) * h / GY;
                p->z = 0;
                p->color = RGBA(0, 0, 0, a[j + oy][i + ox]);
            }
        }
    if (n) gfx_triangles(NULL, v, n, 0);
}

/* Hollow rectangle drawn as four 1px rects. */
void ui_frame(float x, float y, float w, float h, uint32_t col) {
    gfx_rect(x, y, w, 1, col);
    gfx_rect(x, y + h - 1, w, 1, col);
    gfx_rect(x, y, 1, h, col);
    gfx_rect(x + w - 1, y, 1, h, col);
}

/* Dark translucent box with an edge. */
void ui_panel(float x, float y, float w, float h) {
    gfx_rect(x, y, w, h, COL_PANEL);
    ui_frame(x, y, w, h, COL_EDGE);
}

/* Progress bar: back rectangle, then a fill covering `frac` of the width. */
void ui_bar(float x, float y, float w, float h, float frac, uint32_t fill, uint32_t back) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx_rect(x, y, w, h, back);
    gfx_rect(x, y, w * frac, h, fill);
}

/* Draw a vertical list; the selected row gets a highlight and gold text. */
void ui_menu(float x, float y, float w, const char *const *items, int n, int sel) {
    int lh = text_line_height(FONT_BODY) + 2;
    for (int i = 0; i < n; i++) {
        if (i == sel) gfx_rect(x, y + i * lh, w, lh, RGBA(80, 60, 30, 200));
        text_draw(FONT_BODY, x + 4, y + i * lh + 1, items[i], i == sel ? COL_SELECT : COL_TEXT, 0);
    }
}

void ui_hints(const char *text) {
    int w = text_width(FONT_BODY, text) + 10;
    gfx_rect(SCREEN_W - w, SCREEN_H - 14, w, 14, RGBA(0, 0, 0, 170));
    text_draw(FONT_BODY, SCREEN_W - 5, SCREEN_H - 13, text, COL_DIM, 2);
}

void ui_hints_top(const char *text) { text_draw(FONT_BODY, SCREEN_W / 2, 1, text, COL_DIM, 1); }

/* ---------------------------------------------------------------- popups */

/* Floating numbers (damage, heals...) that drift up and fade. A fixed pool;
 * t = seconds left, t<=0 means the slot is free. */
#define MAX_POPUPS 24
typedef struct {
    float x, y, t;
    uint32_t col;
    char text[24];
} Popup;
static Popup popups[MAX_POPUPS];

/* Spawn a popup (printf-style text). Popups born at nearly the same x are
 * stacked upward so two hits in one frame stay readable. */
void ui_popup(float x, float y, uint32_t col, const char *fmt, ...) {
    Popup *p = NULL;
    for (int i = 0; i < MAX_POPUPS; i++)
        if (popups[i].t <= 0) { p = &popups[i]; break; }
    if (!p) return;
    /* stack on top of fresh popups at the same spot instead of overlapping them */
    int stacked = 0;
    for (int i = 0; i < MAX_POPUPS; i++)
        if (&popups[i] != p && popups[i].t > 0.9f && popups[i].x > x - 24 && popups[i].x < x + 24) stacked++;
    y -= stacked * 14;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->text, sizeof(p->text), fmt, ap);
    va_end(ap);
    p->x = x;
    p->y = y;
    p->t = 1.8f;
    p->col = col;
}

void ui_popups_update(float dt) {
    for (int i = 0; i < MAX_POPUPS; i++)
        if (popups[i].t > 0) {
            popups[i].t -= dt;
            popups[i].y -= 18 * dt;
        }
}

void ui_popups_draw(void) {
    for (int i = 0; i < MAX_POPUPS; i++) {
        Popup *p = &popups[i];
        if (p->t <= 0) continue;
        uint32_t a = p->t > 0.4f ? 255 : (uint32_t)(p->t / 0.4f * 255);
        text_draw(FONT_TITLE, p->x, p->y, p->text, (p->col & 0x00ffffffu) | (a << 24), 1);
    }
}

void ui_popups_clear(void) { memset(popups, 0, sizeof(popups)); }

/* ---------------------------------------------------------------- per-class art */

/* Hero portraits and skill icons are loaded lazily on first use, once;
 * tried_* remembers failed loads so we don't hit the disk every frame. */
#define MAX_CLASSES 16
static Image portraits[MAX_CLASSES];
static Image icons[MAX_CLASSES][8];
static uint8_t tried_portrait[MAX_CLASSES], tried_icon[MAX_CLASSES][8];

Image *ui_portrait(int cls) {
    if (cls < 0 || cls >= MAX_CLASSES) return NULL;
    if (!tried_portrait[cls]) {
        char path[96];
        snprintf(path, sizeof(path), "assets/heroes/%s/portrait.ddi", dd_heroes[cls].id);
        image_load(&portraits[cls], path);
        tried_portrait[cls] = 1;
    }
    return portraits[cls].tiles ? &portraits[cls] : NULL;
}

Image *ui_skill_icon(int cls, int skill) {
    if (cls < 0 || cls >= MAX_CLASSES || skill < 0 || skill >= 8) return NULL;
    if (!tried_icon[cls][skill]) {
        char path[96];
        snprintf(path, sizeof(path), "assets/heroes/%s/skill%d.ddi", dd_heroes[cls].id, skill);
        image_load(&icons[cls][skill], path);
        tried_icon[cls][skill] = 1;
    }
    return icons[cls][skill].tiles ? &icons[cls][skill] : NULL;
}

void ui_art_release(void) {
    for (int c = 0; c < MAX_CLASSES; c++) {
        image_free(&portraits[c]);
        for (int s = 0; s < 8; s++) image_free(&icons[c][s]);
    }
    memset(tried_portrait, 0, sizeof(tried_portrait));
    memset(tried_icon, 0, sizeof(tried_icon));
}
