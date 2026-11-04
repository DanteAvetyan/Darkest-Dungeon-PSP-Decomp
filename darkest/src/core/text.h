/*
 * text.h - bitmap-font text drawing.
 * Three fonts are pre-rendered to .ddf files (see tools): body, title, small.
 * Strings are plain ASCII; control bytes 0x01..0x11 are special PSP button
 * icons (see BG_* below) that draw a picture instead of a letter.
 */
#ifndef DD_TEXT_H
#define DD_TEXT_H

#include <stdint.h>

/* Font ids to pass as the `font` argument everywhere. */
enum { FONT_BODY, FONT_TITLE, FONT_SMALL, FONT_COUNT };

/* Load all fonts from assets (call once at startup). */
int text_init(void);
void text_shutdown(void);
/* Pixel height of one line, and pixel width of a string (ignores wrapping). */
int text_line_height(int font);
int text_width(int font, const char *s);
/* Draws one line; returns the x after the last glyph. align: 0 left, 1 centre, 2 right of x. */
float text_draw(int font, float x, float y, const char *s, uint32_t rgba, int align);
/* Word-wrapped text inside width w; returns the height used. */
int text_draw_wrapped(int font, float x, float y, float w, const char *s, uint32_t rgba);
/* printf-style convenience (left aligned). */
float text_printf(int font, float x, float y, uint32_t rgba, const char *fmt, ...);

/* PSP button glyphs for prompts: put these in strings, e.g. BG_CROSS " Use" */
#define BG_CROSS "\x01"
#define BG_CIRCLE "\x02"
#define BG_SQUARE "\x03"
#define BG_TRIANGLE "\x04"
#define BG_L "\x05"
#define BG_R "\x06"
#define BG_START "\x07"
#define BG_SELECT "\x08"
#define BG_DPAD "\x0b"
#define BG_LEFTRIGHT "\x0c"
#define BG_UPDOWN "\x0e"
#define BG_RIGHT "\x0f"
#define BG_UP "\x10"
#define BG_DOWN "\x11"
#endif
