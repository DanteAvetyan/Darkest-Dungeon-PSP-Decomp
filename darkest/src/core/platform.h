/*
 * platform.h - the ONLY door between portable game code (src/core) and the
 * operating system.  Core code never calls fopen/sceIo/etc. directly; it calls
 * these functions, and each platform provides them:
 *   - PSP:  src/platform/psp/main_psp.c
 *   - PC:   src/platform/host/main_host.c
 * That separation is why the same game logic can run on a PSP and in PC tests.
 * Graphics has its own interface in gfx.h.
 */
#ifndef DD_PLATFORM_H
#define DD_PLATFORM_H

#include <stdint.h>

/* Button bitmask.  Each button is one bit, so a whole controller state fits in a
 * uint32_t and several buttons can be tested at once: (buttons & BTN_CROSS).
 * The PC build maps keyboard / scripted tokens onto these same bits. */
enum {
    BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_CROSS = 1 << 4, BTN_CIRCLE = 1 << 5, BTN_SQUARE = 1 << 6, BTN_TRIANGLE = 1 << 7,
    BTN_L = 1 << 8, BTN_R = 1 << 9, BTN_START = 1 << 10, BTN_SELECT = 1 << 11,
};

/* Whole file into a malloc'd buffer (caller frees). Paths are relative to the game folder. */
void *plat_read_file(const char *path, int *size);
/* Buffer that can hold texture data (16-byte aligned on PSP); free() it. */
void *plat_alloc(int n);
/* Save data (small). Returns 0 on success. */
int plat_write_file(const char *path, const void *data, int size);
/* Milliseconds since start; used for timing and seeding. */
uint32_t plat_ticks_ms(void);
/* Appends a line to log.txt next to the game (debugging on hardware). */
void plat_log(const char *fmt, ...);

#endif
