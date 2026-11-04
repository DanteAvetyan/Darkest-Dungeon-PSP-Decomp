#ifndef DD_GAME_H
#define DD_GAME_H

#include <stdint.h>

/* game.h - entry points the platform main loops call each frame:
 *   game_init once, then (game_update + game_draw) per frame, game_shutdown at exit. */

/* Start up: load fonts, seed the campaign RNG, open the title scene. */
int game_init(uint32_t seed);
/* dt in seconds; buttons = BTN_* held this frame */
void game_update(float dt, uint32_t buttons);
/* Render the current scene into the back buffer (gfx_begin ... gfx_end). */
void game_draw(void);
/* Non-zero once a scene called game_request_quit() (e.g. title menu "Quit"). */
int game_quit_requested(void);
void game_shutdown(void);

#endif
