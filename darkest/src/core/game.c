/*
 * game.c - top-level loop glue and the scene state machine.
 *
 * The game is a set of "scenes" (title, town, dungeon, battle).  Exactly one
 * is active.  Every scene exposes four functions: enter / update / draw and
 * (for battle) leave.  This file just dispatches to the active scene.
 *
 * Switching scenes is DEFERRED: scene_switch() only records the request; the
 * real switch happens at the start of the next game_update().  That avoids a
 * scene destroying its own data while it is still running its update code.
 */
#include "game.h"
#include "audio.h"
#include "dd_load.h"
#include "pack.h"
#include <stdlib.h>

#include "assets.h"
#include "campaign.h"
#include "gfx.h"
#include "platform.h"
#include "scenes.h"
#include "text.h"
#include "hud.h"
#include "ui.h"

/* current = scene running now; pending = scene requested by scene_switch(). */
static SceneId current = SCENE_TITLE, pending = SCENE_TITLE;
static int switch_pending, quit;

/* Ask to change scene; takes effect next frame (see file comment). */
void scene_switch(SceneId id) {
    pending = id;
    switch_pending = 1;
}

void game_request_quit(void) { quit = 1; }
int game_quit_requested(void) { return quit; }

/* Perform the switch: tear down battle if we are leaving it, then call the
 * new scene's enter() which loads its art and resets its state. */
static void enter(SceneId id) {
    if (current == SCENE_BATTLE && id != SCENE_BATTLE) battle_leave();
    current = id;
    plat_log("scene %d", (int)id);
    switch (id) {
    case SCENE_TITLE: title_enter(); break;
    case SCENE_TOWN: town_enter(); break;
    case SCENE_DUNGEON: dungeon_enter(); break;
    case SCENE_BATTLE: battle_enter(); break;
    }
}

/* text_init loads the bitmap fonts; the seed becomes the campaign RNG seed so
 * every run can differ (tests pass a fixed seed to be repeatable). */
int game_init(uint32_t seed) {
    {   /* game data tables come from the pak (made by the extractor from the user's own game copy) */
        int sz = 0;
        void *blob = asset_read("assets/gen/dd_data.bin", &sz);
        int rc = blob ? dd_data_load(blob, sz) : -100;
        free(blob);
        if (rc == 0) g_rules = &dd_rules[GM_DARKEST];
        if (rc != 0) { plat_log("game data: load failed (%d)", rc); return rc == -3 ? -3 : -2; }
    }
    if (text_init() != 0) return -1;
    audio_init();
    g_camp.seed = seed;
    enter(SCENE_TITLE);
    return 0;
}

/* One logical frame. input_update turns raw held buttons into per-frame
 * "pressed" edges (see ui.c). dt is real seconds since last frame, so
 * animation speed is independent of frame rate. */
void game_update(float dt, uint32_t buttons) {
    input_update(buttons);
    if (input_pressed(BTN_CROSS)) audio_sfx("ui_town_button_click");
    else if (input_pressed(BTN_UP | BTN_DOWN)) audio_sfx("ui_button_scroll_down");
    if (switch_pending) {
        switch_pending = 0;
        enter(pending);
    }
    switch (current) {
    case SCENE_TITLE: title_update(dt); break;
    case SCENE_TOWN: town_update(dt); break;
    case SCENE_DUNGEON: dungeon_update(dt); break;
    case SCENE_BATTLE: battle_update(dt); break;
    }
}

/* Clear to a near-black brown, let the scene draw, then present. */
void game_draw(void) {
    gfx_begin(RGBA(8, 6, 6, 255));
    switch (current) {
    case SCENE_TITLE: title_draw(); break;
    case SCENE_TOWN: town_draw(); break;
    case SCENE_DUNGEON: dungeon_draw(); break;
    case SCENE_BATTLE: battle_draw(); break;
    }
    gfx_end();
}

/* Free caches in reverse of how they were built. */
void game_shutdown(void) {
    if (current == SCENE_BATTLE) battle_leave();
    sprite_cache_release_all();
    ui_art_release();
    hud_release();
    text_shutdown();
}
