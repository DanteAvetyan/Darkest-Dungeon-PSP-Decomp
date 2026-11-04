/*
 * text.c - draws text from a bitmap font atlas.
 *
 * .ddf layout: "DDF1" | line_h, base, atlas w, atlas h, glyph count (u16s) |
 * pad to 16 | N glyph records of 16 bytes (char code, x,y,w,h in the atlas,
 * xoff,yoff = draw offset, xadv = cursor advance) | atlas pixels (8-bit alpha
 * coverage).  Only ASCII (<128) glyphs are kept.
 *
 * Drawing a string = one textured quad (2 triangles) per glyph, batched into
 * one gfx_triangles call.  Every string is drawn twice: first a 1px-offset
 * black copy (a cheap drop shadow so text reads on any background), then the
 * coloured copy.
 */
#include "text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "gfx.h"
#include "pack.h"
#include "platform.h"

/* One glyph: its rectangle in the atlas and how to place it. */
typedef struct {
    uint16_t x, y, w, h;
    int16_t xoff, yoff, xadv;
} Glyph;

/* A loaded font: metrics, atlas texture, and glyph table indexed by ASCII code. */
typedef struct {
    int line_h, base;
    GfxTexture *tex;
    Glyph g[128];
    int loaded;
} Font;

static Font fonts[FONT_COUNT];

/* Parse a .ddf. Returns 0 ok, <0 error. */
static int font_load(Font *f, const char *path) {
    int size;
    uint8_t *d = asset_read(path, &size);
    if (!d) return -1;
    if (memcmp(d, "DDF1", 4) != 0) {
        free(d);
        return -2;
    }
    uint16_t hdr[6];
    memcpy(hdr, d + 4, sizeof(hdr));
    f->line_h = hdr[0];
    f->base = hdr[1];
    int pw = hdr[2], ph = hdr[3], n = hdr[4];
    const uint8_t *p = d + 16;
    for (int i = 0; i < n; i++, p += 16) {
        uint16_t v[5];
        int16_t s[3];
        memcpy(v, p, 10);
        memcpy(s, p + 10, 6);
        if (v[0] >= 128) continue;
        Glyph *g = &f->g[v[0]];
        g->x = v[1]; g->y = v[2]; g->w = v[3]; g->h = v[4];
        g->xoff = s[0]; g->yoff = s[1]; g->xadv = s[2];
    }
    /* palette: white with alpha = index, so vertex colour tints the glyphs */
    uint8_t clut[1024];
    for (int i = 0; i < 256; i++) {
        clut[i * 4] = clut[i * 4 + 1] = clut[i * 4 + 2] = 255;
        clut[i * 4 + 3] = (uint8_t)i;
    }
    f->tex = gfx_texture_t8(pw, ph, p, clut);
    free(d);
    f->loaded = f->tex != NULL;
    return f->loaded ? 0 : -3;
}

/* The small font is optional: if its file is missing, reuse the body font. */
int text_init(void) {
    int a = font_load(&fonts[FONT_BODY], "assets/fonts/body.ddf");
    int b = font_load(&fonts[FONT_TITLE], "assets/fonts/title.ddf");
    if (font_load(&fonts[FONT_SMALL], "assets/fonts/small.ddf")) font_load(&fonts[FONT_SMALL], "assets/fonts/body.ddf");
    return a || b ? -1 : 0;
}

void text_shutdown(void) {
    for (int i = 0; i < FONT_COUNT; i++) gfx_texture_free(fonts[i].tex);
    memset(fonts, 0, sizeof(fonts));
}

int text_line_height(int font) { return fonts[font].line_h; }

/* PSP button glyphs inside strings (text.h BG_*), drawn from assets/ui/buttons.ddi */
static Image btn_icons;
/* Map a control character to an icon index in buttons.ddi (-1 = ordinary char).
 * Index = position in the strip; unused codes are -1. */
static int btn_icon(unsigned c) {
    static const signed char map[32] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, -1, -1, 8, 9, -1, 10, 11, 12, 13};
    return c < 32 ? map[c] : -1;
}
/* Icons are drawn square, at most 14px, matching the font height. */
static float btn_size(const Font *f) { return f->line_h > 14 ? 14 : f->line_h; }

/* Width = sum of glyph advances (icons count as size+1). */
int text_width(int font, const char *s) {
    const Font *f = &fonts[font];
    int w = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (btn_icon(c) >= 0) w += (int)btn_size(f) + 1;
        else if (c < 128) w += f->g[c].xadv;
    }
    return w;
}

/* Draw one line. Two passes: pass 0 = shadow (offset +1,+1, black with the same
 * alpha), pass 1 = real colour.  Each glyph becomes 6 vertices (two
 * triangles) with UVs in atlas pixels.  Button icons are only recorded while
 * laying out and drawn afterwards from a separate image.  Returns the x
 * where the text ended so callers can chain strings. */
float text_draw(int font, float x, float y, const char *s, uint32_t rgba, int align) {
    const Font *f = &fonts[font];
    if (!f->loaded) return x;
    if (align == 1) x -= text_width(font, s) / 2.0f;
    else if (align == 2) x -= text_width(font, s);
    SpVertex v[6 * 96];
    int n = 0;
    float icon_x[16];
    int icon_i[16], nicon = 0;
    uint32_t shadow = rgba & 0xff000000u;
    for (int pass = 0; pass < 2; pass++) {
        float cx = x + (pass ? 0 : 1), cy = y + (pass ? 0 : 1);
        uint32_t col = pass ? rgba : shadow;
        for (const char *p = s; *p; p++) {
            unsigned c = (unsigned char)*p;
            if (c >= 128) continue;
            if (btn_icon(c) >= 0) { /* leave room; the glyph is drawn after the text batch */
                if (pass && nicon < 16) {
                    icon_x[nicon] = cx;
                    icon_i[nicon++] = btn_icon(c);
                }
                cx += btn_size(f) + 1;
                continue;
            }
            const Glyph *g = &f->g[c];
            if (c != 32 && g->w > 0 && n + 6 <= 6 * 96) {
                float x0 = cx + g->xoff, y0 = cy + g->yoff, x1 = x0 + g->w, y1 = y0 + g->h;
                SpVertex q[6] = {
                    {g->x, g->y, col, x0, y0, 0}, {g->x + g->w, g->y, col, x1, y0, 0},
                    {g->x + g->w, g->y + g->h, col, x1, y1, 0}, {g->x, g->y, col, x0, y0, 0},
                    {g->x + g->w, g->y + g->h, col, x1, y1, 0}, {g->x, g->y + g->h, col, x0, y1, 0},
                };
                memcpy(&v[n], q, sizeof(q));
                n += 6;
            }
            cx += g->xadv;
        }
        if (n) gfx_triangles(f->tex, v, n, BLEND_ALPHA); /* a string of only button glyphs has no text */
        n = 0;
        if (pass) x = cx;
        if (pass && nicon) {
            if (!btn_icons.tiles) image_load(&btn_icons, "assets/ui/buttons.ddi");
            float sz = btn_size(f), iy = y + (f->line_h - sz) / 2;
            for (int k = 0; k < nicon && btn_icons.tiles; k++)
                image_draw_part(&btn_icons, icon_i[k] * 16.0f, 16, icon_x[k], iy, sz, sz, 0xffffffffu);
        }
    }
    return x;
}

/* Greedy word-wrap: grow a line char by char; once it is wider than w, cut at
 * the last space. Honours '\n'. Returns total height so callers can stack
 * following content below it. */
int text_draw_wrapped(int font, float x, float y, float w, const char *s, uint32_t rgba) {
    char line[160];
    int lh = fonts[font].line_h, used = 0;
    while (*s) {
        int len = 0, last_space = -1;
        while (s[len] && s[len] != '\n' && len < (int)sizeof(line) - 1) {
            line[len] = s[len];
            line[len + 1] = 0;
            if (s[len] == ' ') last_space = len;
            if (text_width(font, line) > w && last_space > 0) {
                len = last_space;
                break;
            }
            len++;
        }
        line[len] = 0;
        text_draw(font, x, y + used, line, rgba, 0);
        used += lh;
        s += len;
        while (*s == ' ' || *s == '\n') s++;
    }
    return used;
}

float text_printf(int font, float x, float y, uint32_t rgba, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return text_draw(font, x, y, buf, rgba, 0);
}
