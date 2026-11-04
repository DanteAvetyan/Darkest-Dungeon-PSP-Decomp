/*
 * scene_title.c - the title screen and new-game menu.
 * Flow: "Press Start" -> menu (Continue / New Campaign / Quit); New Campaign
 * opens the mode picker (Radiant / Darkest / Stygian) which calls
 * campaign_new() and switches to the dungeon (the Old Road tutorial).
 * Title screen: the red sky, the Ancestor's manor and the burning logo, as in the original.
 * "Press Start", then Continue / New Campaign / Quit. */
#include <math.h>
#include <stdio.h>

#include "assets.h"
#include "audio.h"
#include "campaign.h"
#include "dd_types.h"
#include "platform.h"
#include "scenes.h"
#include "text.h"
#include "ui.h"

/* Art, menu cursor state and an animation clock `t` (drives the flicker). */
static Image sky, house, frame;
static int sel, has_save, menu_open; /* menu_open 2: picking the game mode */
static int mode_sel = 1;
/* modes/<m>/mode_settings.json menu_priority order: Radiant, Darkest, Stygian */
static const int mode_order[GM_COUNT] = {GM_RADIANT, GM_DARKEST, GM_STYGIAN};
static Image mode_bg, mode_icon[GM_COUNT]; /* fe_flow modeselect backdrop, modes/<m>/fe_flow icons */
static float t;

/* Load title art once, detect an existing save (enables Continue). */
void title_enter(void) {
    audio_scene_music(0, 0, 0);
    grade_scene(NULL, 0);
    sprite_cache_release_all();
    if (!sky.tiles) image_load(&sky, "assets/ui/title_sky.ddi");
    if (!house.tiles) image_load(&house, "assets/ui/title_house.ddi");
    if (!frame.tiles) image_load(&frame, "assets/ui/frame_button.ddi");
    if (!mode_bg.tiles) image_load(&mode_bg, "assets/ui/mode_backdrop.ddi");
    static const char *const icons[GM_COUNT] = {"base", "radiant", "new_game_plus"}; /* GM_* order */
    for (int i = 0; i < GM_COUNT; i++)
        if (!mode_icon[i].tiles) {
            char p[48];
            snprintf(p, sizeof(p), "assets/ui/mode_%s.ddi", icons[i]);
            image_load(&mode_icon[i], p);
        }
    has_save = campaign_load() == 0;
    sel = has_save ? 0 : 1;
    menu_open = 0;
    t = 0;
}

/* Menu input: Up/Down to move, X to choose, O to go back. */
void title_update(float dt) {
    t += dt;
    if (!menu_open) {
        if (input_pressed(BTN_START) || input_pressed(BTN_CROSS)) menu_open = 1;
        return;
    }
    if (menu_open == 2) {
        mode_sel = ui_menu_input(mode_sel, GM_COUNT);
        if (input_pressed(BTN_CIRCLE)) menu_open = 1;
        if (input_pressed(BTN_CROSS)) {
            campaign_new(g_camp.seed ^ plat_ticks_ms(), mode_order[mode_sel]);
            campaign_save();
            scene_switch(SCENE_DUNGEON); /* a new campaign opens on the Old Road */
        }
        return;
    }
    int prev = sel;
    sel = ui_menu_input(sel, 3);
    if (!has_save && sel == 0) sel = prev == 1 ? 2 : 1; /* skip Continue without a save */
    if (input_pressed(BTN_CIRCLE)) menu_open = 0;
    if (!input_pressed(BTN_CROSS)) return;
    if (sel == 0 && has_save) {
        scene_switch(g_camp.in_dungeon ? SCENE_DUNGEON : SCENE_TOWN);
    } else if (sel == 1) {
        menu_open = 2;
        mode_sel = 1;
    } else if (sel == 2) {
        game_request_quit();
    }
}

/* Layers: sky, glowing logo (additive blend), the manor, then menu text. */
void title_draw(void) {
    if (sky.tiles) {
        /* the glow slowly breathes */
        uint8_t k = (uint8_t)(200 + 55 * sinf(t * 0.8f));
        image_draw(&sky, 0, 0, SCREEN_W, SCREEN_H, RGBA(k, k, k, 255));
    }
    if (house.tiles) image_draw(&house, 0, SCREEN_H - house.h, house.w, house.h, RGBA(255, 255, 255, 255));

    const uint8_t white[4] = {255, 255, 255, 255};
    /* fx/titles: "show" burns the letters in once (1.33 s), then "torch_loop" (12 s) repeats at the
     * original speed */
    SpriteAnim *show = sprite_get("titles", "show"), *loop = sprite_get("titles", "torch_loop");
    float ly = menu_open ? 150 : 170, sd = show ? show->anim.duration : 0;
    if (show && t < sd) sprite_draw(show, t, 0, SCREEN_W / 2, ly, 0.85f, 0, white);
    else if (loop) sprite_draw(loop, t - sd, 1, SCREEN_W / 2, ly, 0.85f, 0, white);
    else if (show) sprite_draw(show, t, 0, SCREEN_W / 2, ly, 0.85f, 0, white);
    else text_draw(FONT_TITLE, SCREEN_W / 2, 40, "Darkest Dungeon", COL_GOLD, 1);

    if (!menu_open) {
        if (fmodf(t, 1.4f) < 1.0f) text_draw(FONT_TITLE, SCREEN_W / 2, 196, "Press " BG_START, COL_TEXT, 1);
        return;
    }
    if (menu_open == 2) { /* "Select Campaign": skull of the highlighted mode, the three modes, its tooltip */
        float x0 = SCREEN_W / 2 - 120, y0 = 30;
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 150));
        if (mode_bg.tiles) image_draw(&mode_bg, x0, y0, 240, 200, RGBA(255, 255, 255, 255));
        else ui_panel(x0, y0, 240, 200);
        int m = mode_order[mode_sel];
        if (mode_icon[m].tiles) image_draw(&mode_icon[m], SCREEN_W / 2 - 24, y0 - 22, 48, 48, RGBA(255, 255, 255, 255));
        text_draw(FONT_TITLE, SCREEN_W / 2, y0 + 30, dd_ui_str[US_SELECT_CAMPAIGN], COL_GOLD, 1);
        for (int i = 0; i < GM_COUNT; i++) {
            float y = y0 + 62 + i * 20;
            if (i == mode_sel) {
                gfx_rect(SCREEN_W / 2 - 80, y - 3, 160, 1, COL_GOLD);
                gfx_rect(SCREEN_W / 2 - 80, y + 14, 160, 1, COL_GOLD);
            }
            text_draw(FONT_BODY, SCREEN_W / 2, y, dd_ui_str[US_MODE_RADIANT + i], i == mode_sel ? COL_SELECT : COL_TEXT, 1);
        }
        text_draw_wrapped(FONT_BODY, x0 + 14, y0 + 130, 212, dd_ui_str[US_MODE_RADIANT_TIP + mode_sel], COL_TEXT);
        ui_hints(BG_CROSS " Start  " BG_CIRCLE " Back");
        return;
    }
    static const char *const items[] = {"Continue", "New Campaign", "Quit"};
    for (int i = 0; i < 3; i++) {
        float y = 150 + i * 34;
        if (frame.tiles) image_draw(&frame, SCREEN_W / 2 - 80, y, 160, 30, i == sel ? RGBA(255, 255, 255, 255) : RGBA(150, 140, 130, 255));
        uint32_t c = i == sel ? COL_SELECT : (i == 0 && !has_save ? RGBA(90, 80, 70, 255) : COL_TEXT);
        text_draw(FONT_TITLE, SCREEN_W / 2, y + 6, items[i], c, 1);
    }
    ui_hints(BG_CROSS " Select  " BG_CIRCLE " Back");
}
