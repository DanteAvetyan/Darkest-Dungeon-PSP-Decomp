/*
 * scene_town.c - the Hamlet: the between-expeditions hub.
 *
 * What you do here: hire heroes (Stage Coach), buy gear and skills (Blacksmith,
 * Guild), relieve stress (Tavern, Abbey), treat quirks (Sanitarium), buy
 * trinkets (Nomad Wagon), unlock camping skills (Survivalist), then choose a
 * quest, party and provisions ("embark") and set off.
 *
 * SCREEN FLOW (TState):
 *   TS_HAMLET          the town view; Left/Right pick a building
 *   TS_BUILDING        inside one building (its own sub-steps via sel/sub/phase)
 *   TS_ROSTER/TS_HERO  roster list and a hero's character sheet
 *   TS_EMBARK_QUEST -> TS_EMBARK_PARTY -> TS_EMBARK_PROVISION   embark wizard
 *   TS_LOG, TS_EPILOG, TS_GAMEOVER, TS_JOURNAL, TS_EXCHANGE, TS_QUIT   overlays
 * town_update() switches on `st`; town_draw() likewise.
 *
 * The town view is built from the original's layout data: every building has
 * an (x,y,z) position in layout units, scaled by TOWN_SCALE to our 480px screen.
 *
 * The Hamlet: the original's town view built from its building sprites and layout data
 * (campaign/town/town.layout.darkest). Pick a building with Left/Right, X to enter.
 * Triangle roster, Start embark, Select end the week, O save & quit. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "audio.h"
#include "campaign.h"
#include "platform.h"
#include "scenes.h"
#include "stress.h"
#include "hud.h"
#include "quirk.h"
#include "town_event.h"
#include "trinket.h"
#include "text.h"
#include "ui.h"

/* Layout units -> screen pixels, and the screen y of layout y=0. */
#define TOWN_SCALE 0.25f
#define TOWN_Y0 217.0f /* screen y of the layout's y = 0 */

/* Building ids. Order matches the bld[] table below. */
typedef enum {
    B_STAGE, B_GUILD, B_SMITH, B_TAVERN, B_ABBEY, B_SURVIVAL, B_GRAVEYARD, B_SANITARIUM, B_NOMAD, B_STATUE, B_COUNT
} Bld;

/* Static building table: asset id, display name, layout position, draw order
 * (z), and whether it is selectable (open). */
static const struct {
    const char *id, *name;
    float x, y; /* town.layout.darkest .pos */
    int z, open;
} bld[B_COUNT] = {
    {"stage_coach", "Stage Coach", 315, -5, -3, 1},
    {"guild", "Guild", 1282, 100, 0, 1},
    {"blacksmith", "Blacksmith", 1460, -30, -2, 1},
    {"tavern", "Tavern", 550, 10, -1, 1},
    {"abbey", "Abbey", 1070, 220, 3, 1},
    {"camping_trainer", "Survivalist", 185, 315, 0, 1},
    {"graveyard", "Graveyard", 965, 230, 2, 1},
    {"sanitarium", "Sanitarium", 790, 110, 1, 1},
    {"nomad_wagon", "Nomad Wagon", 1130, -40, -5, 1},
    {"statue", "Memoirs", 940, -15, -4, 0},
};

/* Tavern (row 0) and Abbey (row 1) each have three activities. Base cost and
 * stress relief come from here; upgrades change them via the trees named.
 * rules: building.json activities (cost in gold, stress relief) */
static const struct { const char *name, *icon, *tree; int cost, relief; } activities[2][3] = {
    {{"Bar", "tavern_bar", "tavern.bar", 1000, 45}, {"Gambling", "tavern_gambling", "tavern.gambling", 1250, 55},
     {"Brothel", "tavern_brothel", "tavern.brothel", 1500, 65}},
    {{"Meditation", "abbey_meditation", "abbey.meditation", 1000, 45}, {"Prayer", "abbey_prayer", "abbey.prayer", 1250, 55},
     {"Flagellation", "abbey_flagellation", "abbey.flagellation", 1500, 65}},
};

/* Activity values after building upgrades (and the active town event). */
static int act_cost(int abbey, int a) { return town_event_cost(abbey * 3 + a, (int)tree_value(activities[abbey][a].tree, TV_COST, activities[abbey][a].cost)); }
static int act_relief(int abbey, int a) { return (int)tree_value(activities[abbey][a].tree, TV_HEAL, activities[abbey][a].relief); }
static int act_slots(int abbey, int a) { return (int)tree_value(activities[abbey][a].tree, TV_SLOTS, 1); }
/* building.json requirements not_have_quirks: e.g. a Gambler only gambles, the Faithless skip prayer */
static void notify(const char *s);
/* Some heroes refuse certain activities because of a quirk (a Gambler only
 * gambles). Shows a message and returns 1 if refused. */
static int act_refused(const Hero *h, int act) {
    const DDActivity *a = &dd_activities[act];
    for (int k = 0; k < a->nno; k++)
        if (quirk_has(h, a->no_quirk[k])) {
            char b[96];
            snprintf(b, sizeof(b), "%s: %s", h->name, dd_ui_str[US_HAS_QUIRK]);
            char m[128];
            snprintf(m, sizeof(m), b, dd_quirks[a->no_quirk[k]].name);
            notify(m);
            return 1;
        }
    return 0;
}

/* How many heroes currently occupy this activity (slots are limited). */
static int act_used(int abbey, int a) {
    int n = 0;
    for (int i = 0; i < g_camp.nroster; i++) n += g_camp.roster[i].busy == 1 + abbey * 3 + a;
    return n;
}

/* Screen states of this scene (see file comment). */
typedef enum {
    TS_HAMLET, TS_BUILDING, TS_ROSTER, TS_HERO, TS_QUIT, TS_EPILOG, TS_EXCHANGE, TS_JOURNAL, TS_GAMEOVER, TS_LOG,
    TS_EMBARK_QUEST, TS_EMBARK_PARTY, TS_EMBARK_PROVISION
} TState;

/* Scene globals: current state, cursors, timers, cached images, and the
 * embark wizard's choices (emb_*: dungeon, level, length, quest type, and the
 * provisions being bought). */
static TState st, back_to;
static int epi_line;       /* epilogue line shown (after the Heart of Darkness falls) */
static float epi_t;
static int cur;            /* selected building */
static int order[B_COUNT], norder;
static int sel, sub, phase; /* list cursors / building sub-step */
static int hero_sel, activity;
static int log_top; /* first activity log entry shown */
static float t, note_t;
static char note[128];
static Image store_bg, store_char, qmap_art[13];
static uint8_t qmap_tried[13];
static Image sky, bimg_bg, bimg_char, act_icons[3], loading;
static int loaded_bld = -1, loaded_loading = -1;
static Image nav_icon[B_COUNT]; /* building quick navigation (ui/<b>_icon.ddi) */
static uint8_t nav_tried[B_COUNT];
static int emb_dungeon, emb_level = 1, emb_length = 1, emb_quest, emb_choice;
static uint8_t emb_items[ITEM_COUNT] = {4, 4, 0, 0, 0, 0, 0, 0, 0, 0}; /* food, torches */

/* Total gold for the provisions chosen so far (free on the Old Road). */
static int provisions_cost(void) {
    /* the opening expedition is supplied for free (the plot quest's starting items) */
    if (g_camp.nboard && emb_choice < g_camp.nboard && g_camp.board[emb_choice].plot == PLOT_OLD_ROAD) return 0;
    int c = 0;
    for (int i = 0; i < ITEM_COUNT; i++) c += emb_items[i] * town_event_item_price(i, item_prices[i]);
    return c;
}

/* Flash a short message for ~2 seconds. */
static void notify(const char *s) {
    static const struct { const char *key, *snd; } cue[] = {
        {"Not enough", "prop_gen_negative"}, {"Can't do", "prop_gen_negative"}, {"busy", "prop_gen_negative"},
        {"Closed", "prop_gen_negative"}, {"No free", "prop_gen_negative"}, {"Sold out", "prop_gen_negative"},
        {"Only four", "prop_gen_negative"}, {"Locked", "prop_gen_negative"}, {"Already", "prop_gen_negative"},
        {"is full", "prop_gen_negative"}, {"party is full", "prop_gen_negative"},
        {"Upgrade purchased", "town_gen_building_upgrade"}, {"Upgraded", "town_gen_purchase_upgrade_last"},
        {"A new hero", "town_stagecoach_purchase"}, {"Bought", "town_gen_purchase_upgrade_last"},
        {"treated for", "town_sanitarium_treatment"}, {"is locked in", "town_sanitarium_treatment"},
        {"A week passes", "town_gen_district_build"}, {"Exchanged", "gen_raid_heirloom_add"},
    };
    for (unsigned i = 0; i < sizeof cue / sizeof *cue; i++)
        if (strstr(s, cue[i].key)) { audio_sfx(cue[i].snd); break; }
    if (strstr(s, "Dismissed")) audio_narrate_important("town_dismiss", 100);
    else if ((strstr(s, "Upgrade purchased") || strstr(s, "Upgraded")) && cur >= 0 && cur < norder) {
        static const char *const up[B_COUNT] = {"town_upgrade_stage", "town_upgrade_guild", "town_upgrade_blacksmith", "town_upgrade_tavern",
            "town_upgrade_abbey", "town_upgrade_camp", NULL, "town_upgrade_sanit", "town_upgrade_nomad", NULL};
        if (up[order[cur]]) audio_narrate_important(up[order[cur]], 100);
    }
    snprintf(note, sizeof(note), "%s", s);
    note_t = 2.2f;
}

/* Scene enter: free caches, build the left-to-right building order (insertion
 * sort by x), choose the first unlocked building, and open the activity log
 * if an expedition just ended. */
void town_enter(void) {
    audio_scene_music(1, 0, 0);
    audio_narrate_reset();
    grade_scene(NULL, 0); /* the hamlet is not colour graded */
    sprite_cache_release_all();
    hud_release();
    for (int i = 0; i < B_COUNT; i++) { image_free(&nav_icon[i]); nav_tried[i] = 0; }
    if (!sky.tiles) image_load(&sky, "assets/ui/town_sky.ddi");
    /* building selection order: left to right as drawn */
    norder = 0;
    for (int i = 0; i < B_COUNT; i++)
        if (bld[i].open) order[norder++] = i;
    for (int i = 1; i < norder; i++)
        for (int j = i; j > 0 && bld[order[j]].x < bld[order[j - 1]].x; j--) {
            int k = order[j]; order[j] = order[j - 1]; order[j - 1] = k;
        }
    st = g_camp.log_unseen ? TS_LOG : TS_HAMLET; /* the activity log opens first after an expedition */
    if (cur < 0 || cur >= norder || !building_unlocked(bld[order[cur]].id))
        for (cur = 0; cur < norder - 1 && !building_unlocked(bld[order[cur]].id); cur++) {}
}

/* Load a building's backdrop and character art (lazy; one at a time). */
static void load_building(int b) {
    if (loaded_bld == b) return;
    char p[64];
    image_free(&bimg_bg);
    image_free(&bimg_char);
    for (int i = 0; i < 3; i++) image_free(&act_icons[i]);
    snprintf(p, sizeof(p), "assets/ui/%s_bg.ddi", bld[b].id);
    image_load(&bimg_bg, p);
    snprintf(p, sizeof(p), "assets/ui/%s_char.ddi", bld[b].id);
    image_load(&bimg_char, p);
    if (b == B_TAVERN || b == B_ABBEY)
        for (int i = 0; i < 3; i++) {
            snprintf(p, sizeof(p), "assets/ui/%s.ddi", activities[b == B_ABBEY][i].icon);
            image_load(&act_icons[i], p);
        }
    if (b == B_SMITH) {
        image_load(&act_icons[0], "assets/ui/blacksmith_weapon.ddi");
        image_load(&act_icons[1], "assets/ui/blacksmith_armour.ddi");
    }
    loaded_bld = b;
}

/* ---------------------------------------------------------------- hero list widget
 * A scrolling list of roster heroes reused by several buildings. */

static int hero_ok_for_party(int i) { return g_camp.roster[i].alive && !g_camp.roster[i].busy; }
/* quest.restriction.json: heroes above the quest level's resolve cap refuse it (plot quests: no cap).
 * Town event remove_quest_hero_level_restriction lifts it. */
/* Quests have a maximum hero resolve level. */
static int too_experienced(int i) {
    if (g_camp.board[emb_choice].plot || emb_level < 0 || emb_level > 6 || town_event_op(TE_NO_LEVEL_LIMIT, -1)) return 0;
    return g_camp.roster[i].level > g_rules->max_level[emb_level];
}

/* One roster row: portrait, name, class/level, HP/stress, plus `right` text. */
static void draw_hero_row(float x, float y, float w, const Hero *h, int selected, const char *right) {
    if (selected) gfx_rect(x - 2, y - 1, w + 4, 24, RGBA(90, 66, 30, 210));
    Image *p = ui_portrait(h->cls);
    if (p) image_draw(p, x, y, 22, 22, h->busy ? RGBA(110, 110, 110, 255) : RGBA(255, 255, 255, 255));
    text_printf(FONT_BODY, x + 26, y - 1, selected ? COL_SELECT : COL_TEXT, "%s", h->name);
    text_printf(FONT_BODY, x + 26, y + 10, COL_DIM, "%s %s", dd_ui_str[US_RESOLVE_TITLE + (h->level < 7 ? h->level : 6)], dd_heroes[h->cls].name);
    /* resolve level badge, then HP / stress */
    ui_frame(x + w - 14, y + 4, 12, 12, RGBA(150, 140, 120, 255));
    text_printf(FONT_SMALL, x + w - 10, y + 4, COL_TEXT, "%d", h->level);
    ui_bar(x + w - 64, y + 4, 46, 3, (float)h->hp / hero_max_hp(h), COL_RED, RGBA(40, 10, 10, 220));
    ui_bar(x + w - 64, y + 9, 46, 3, h->stress / 200.0f, RGBA(230, 230, 230, 255), RGBA(30, 30, 30, 200));
    int virtue = 0;
    const char *tr = stress_affliction_name(h, &virtue);
    if (right) text_draw(FONT_BODY, x + w - 70, y + 3, right, COL_GOLD, 2);
    else if (tr) text_draw(FONT_BODY, x + w - 70, y + 3, tr, virtue ? COL_GOLD : RGBA(210, 90, 210, 255), 2);
    else if (h->busy == MISSING_BUSY) text_draw(FONT_BODY, x + w - 70, y + 3, "missing", COL_RED, 2);
    else if (h->busy) text_draw(FONT_BODY, x + w - 70, y + 3, "busy", COL_DIM, 2);
}

/* Scrolling roster list; returns the y after the list. */
/* The list: `rows` visible; `first` is chosen so the cursor row is never
 * below the last visible row (the list scrolls as the cursor moves down).
 * `right` is a callback giving per-hero right-hand text (cost, status...). */
static void draw_roster(float x, float y, float w, int rows, int cursor, const char *(*right)(int)) {
    int first = cursor - rows + 1 > 0 ? cursor - rows + 1 : 0;
    for (int r = 0; r < rows && first + r < g_camp.nroster; r++) {
        int i = first + r;
        draw_hero_row(x, y + r * 26, w, &g_camp.roster[i], i == cursor, right ? right(i) : NULL);
    }
    if (g_camp.nroster == 0) text_draw(FONT_BODY, x, y, "No heroes.", COL_DIM, 0);
}

/* ---------------------------------------------------------------- buildings */

/* Right-hand text for blacksmith rows: "W<weapon tier> A<armour tier>". */
static const char *gear_text(int i) {
    static char b[24];
    snprintf(b, sizeof(b), "W%d A%d", g_camp.roster[i].weapon_lvl + 1, g_camp.roster[i].armour_lvl + 1);
    return b;
}

/* ---------------------------------------------------------------- sanitarium
 * Treats negative quirks, cures diseases, and locks in positive quirks, for
 * a fee, limited slots per week. */

enum { SAN_TREAT = 7, SAN_DISEASE = 8 }; /* Hero.busy codes (1..6 are tavern/abbey activities) */

/* Slots already used this week for a treatment kind. */
static int san_used(int code) {
    int n = 0;
    for (int i = 0; i < g_camp.nroster; i++) n += g_camp.roster[i].busy == code;
    return n;
}

/* Treatment for quirk slot i: cost (gold), and whether it is allowed; *what names the action. */
/* Price of treating quirk slot i (and what the action is called). -1 means
 * not available: positives can be locked only while fewer than 3 are locked. */
static int san_cost(const Hero *h, int i, const char **what) {
    const DDQuirk *q = &dd_quirks[h->quirks[i] - 1];
    int locked = (h->quirk_locked >> i) & 1;
    if (q->disease) { *what = "Cure"; return town_event_cost(TA_DISEASE, (int)tree_value("sanitarium.disease_quirk_cost", TV_COST, 750)); }
    if (q->positive) {
        *what = locked ? "Locked" : "Lock in";
        if (locked) return -1;
        int nl = 0;
        for (int k = 0; k < HERO_QUIRKS; k++)
            nl += h->quirks[k] && dd_quirks[h->quirks[k] - 1].positive && ((h->quirk_locked >> k) & 1);
        return nl >= 3 ? -1 : town_event_cost(TA_TREATMENT, (int)tree_value("sanitarium.cost", TV_COST, 7500)); /* quirks_max_locked_positive */
    }
    *what = "Treat";
    return town_event_cost(TA_TREATMENT, locked ? (int)tree_value("sanitarium.cost", TV_COST_PERMANENT, 5000) : (int)tree_value("sanitarium.cost", TV_COST_NEGATIVE, 1500));
}

/* Weekly slot capacity (grown by building upgrades). */
static int san_slots(int disease) {
    return (int)tree_value("sanitarium.slots", disease ? TV_SLOTS_DISEASE : TV_SLOTS, 1);
}

/* Input for the sanitarium screen. */
static void sanitarium_update(Hero *h) {
    if (phase == 0) {
        sel = ui_menu_input(sel, g_camp.nroster);
        if (input_pressed(BTN_CROSS) && g_camp.nroster) { hero_sel = sel; phase = 1; sub = 0; }
        if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
        return;
    }
    int slots[HERO_QUIRKS], n = 0;
    for (int i = 0; i < HERO_QUIRKS; i++)
        if (h->quirks[i]) slots[n++] = i;
    if (n) sub = ui_menu_input(sub, n);
    if (input_pressed(BTN_CIRCLE)) { phase = 0; sel = hero_sel; return; }
    if (!input_pressed(BTN_CROSS) || !n) return;
    const char *what;
    int i = slots[sub], cost = san_cost(h, i, &what);
    int disease = dd_quirks[h->quirks[i] - 1].disease, code = disease ? SAN_DISEASE : SAN_TREAT;
    if (cost < 0) notify("Can't do that (already locked, or 3 quirks locked).");
    else if (h->busy) notify("That hero is busy this week.");
    else if (san_used(code) >= san_slots(disease)) notify("No free slot this week. Upgrade the Sanitarium.");
    else if (g_camp.gold < cost) notify("Not enough gold.");
    else {
        g_camp.gold -= cost;
        h->busy = (uint8_t)code;
        char b[96];
        if (disease) {
            /* disease_quirk_cure_all_chance: sometimes every disease goes */
            Rng r;
            rng_seed(&r, plat_ticks_ms());
            int all = rng_chance(&r, tree_value("sanitarium.disease_quirk_cost", TV_CHANCE, 0.33f));
            for (int k = 0; k < HERO_QUIRKS; k++)
                if (h->quirks[k] && dd_quirks[h->quirks[k] - 1].disease && (all || k == i)) quirk_remove(h, k);
            snprintf(b, sizeof(b), all ? "%s is cured of all diseases." : "%s is cured.", h->name);
        } else if (dd_quirks[h->quirks[i] - 1].positive) {
            h->quirk_locked |= (uint16_t)(1u << i);
            snprintf(b, sizeof(b), "%s's %s is locked in.", h->name, dd_quirks[h->quirks[i] - 1].name);
        } else {
            snprintf(b, sizeof(b), "%s is treated for %s.", h->name, dd_quirks[h->quirks[i] - 1].name);
            quirk_remove(h, i);
        }
        h->hp = (int16_t)(h->hp > hero_max_hp(h) ? hero_max_hp(h) : h->hp);
        notify(b);
        sub = 0;
    }
}

/* Draw it: the hero's quirk list with costs. */
static void sanitarium_draw(Hero *h, float px, float py, float pw) {
    if (phase == 0) {
        text_printf(FONT_BODY, px + 8, py + 4, COL_DIM, "Choose a hero.   Treatment %d/%d  Disease %d/%d", san_used(SAN_TREAT),
                    san_slots(0), san_used(SAN_DISEASE), san_slots(1));
        draw_roster(px + 8, py + 22, pw - 16, 8, sel, NULL);
        ui_hints(BG_CROSS " Choose  " BG_CIRCLE " Back");
        return;
    }
    text_printf(FONT_BODY, px + 8, py + 4, COL_TEXT, "%s's quirks (* locked)", h->name);
    int row = 0;
    for (int i = 0; i < HERO_QUIRKS; i++) {
        if (!h->quirks[i]) continue;
        const DDQuirk *q = &dd_quirks[h->quirks[i] - 1];
        const char *what;
        int cost = san_cost(h, i, &what);
        float y = py + 22 + row * 16;
        if (row == sub) gfx_rect(px + 4, y - 1, pw - 8, 15, RGBA(90, 66, 30, 210));
        text_printf(FONT_BODY, px + 8, y, q->disease ? RGBA(170, 200, 60, 255) : q->positive ? COL_GOOD : COL_RED, "%s%s",
                    q->name, (h->quirk_locked >> i) & 1 ? " *" : "");
        if (cost >= 0) text_printf(FONT_BODY, px + pw - 8 - 110, y, COL_GOLD, "%s  %d g", what, cost);
        else text_draw(FONT_BODY, px + pw - 8 - 110, y, what, COL_DIM, 0);
        row++;
    }
    if (!row) text_draw(FONT_BODY, px + 8, py + 22, "No quirks.", COL_DIM, 0);
    ui_hints(BG_CROSS " Treat  " BG_CIRCLE " Back");
}

/* ---------------------------------------------------------------- building upgrades (heirlooms)
 * Pressing Triangle in a building opens its upgrade tree. Purchases cost gold
 * plus heirlooms (busts, portraits, deeds, crests) and change tree_value(). */

static int upg_open, upg_sel;
static void est_put(int id, float x, float y, float w, float h);

/* dd_trees entries of building b ("<building id>.<tree>"). */
/* Which upgrade trees belong to building b (out[]); returns the count. */
static int building_trees(int b, int out[8]) {
    int n = 0;
    size_t len = strlen(bld[b].id);
    for (int i = 0; i < dd_tree_count && n < 8; i++)
        if (strncmp(dd_trees[i].id, bld[b].id, len) == 0 && dd_trees[i].id[len] == '.') out[n++] = i;
    return n;
}

/* Upgrade screen input. */
static void upgrades_update(int b) {
    int t[8], n = building_trees(b, t);
    if (input_pressed(BTN_TRIANGLE) || input_pressed(BTN_CIRCLE) || !n) { upg_open = 0; return; }
    upg_sel = ui_menu_input(upg_sel, n);
    if (input_pressed(BTN_CROSS)) {
        int r = tree_buy(t[upg_sel]);
        notify(r == 0 ? "Upgrade purchased." : r == -1 ? "Fully upgraded." : "Not enough heirlooms.");
        if (r == 0) campaign_save();
    }
}

/* Draw the tree list with levels, costs and affordability. */
static void upgrades_draw(int b) {
    int t[8], n = building_trees(b, t);
    float px = 150, py = 22, pw = 322, ph = 30 + n * 26;
    gfx_rect(px, py, pw, ph, RGBA(12, 9, 7, 250));
    ui_panel(px, py, pw, ph);
    text_draw(FONT_TITLE, px + 8, py + 2, "Upgrades", COL_GOLD, 0);
    for (int i = 0; i < n; i++) {
        const DDTree *tr = &dd_trees[t[i]];
        int lvl = g_camp.upgrades[t[i]];
        float y = py + 24 + i * 26;
        if (i == upg_sel) gfx_rect(px + 4, y - 1, pw - 8, 25, RGBA(90, 66, 30, 200));
        static const char *const nice[][2] = {{"numrecruits", "Recruits per week"}, {"rostersize", "Roster size"},
            {"skill_levels", "Skill levels"}, {"cost", "Discount"}, {"weapon", "Weapons"}, {"armour", "Armour"}};
        char name[32];
        snprintf(name, sizeof(name), "%s", strchr(tr->id, '.') + 1);
        for (size_t q = 0; q < sizeof(nice) / sizeof(nice[0]); q++)
            if (strcmp(name, nice[q][0]) == 0) snprintf(name, sizeof(name), "%s", nice[q][1]);
        if (name[0] >= 'a' && name[0] <= 'z') name[0] = (char)(name[0] - 32);
        text_draw(FONT_BODY, px + 8, y, name, i == upg_sel ? COL_SELECT : COL_TEXT, 0);
        for (int k = 0; k < tr->levels; k++) /* level pips */
            gfx_rect(px + 8 + k * 9, y + 15, 7, 5, k < lvl ? COL_GOLD : RGBA(50, 45, 40, 255));
        if (lvl >= tr->levels) {
            text_draw(FONT_SMALL, px + pw - 8, y + 6, "Complete", COL_DIM, 2);
            continue;
        }
        float x = px + 150;
        for (int k = 0; k < 4; k++) {
            int c = tr->cost[lvl][k];
            if (!c) continue;
            est_put(5 + k, x, y + 3, 0, 0); /* E_BUST + k */
            x = text_printf(FONT_SMALL, x + 18, y + 7, g_camp.heirlooms[k] >= c ? COL_TEXT : COL_RED, "%d", c) + 10;
        }
        if (tr->cost[lvl][4]) text_printf(FONT_SMALL, x, y + 7, g_camp.gold >= tr->cost[lvl][4] ? COL_GOLD : COL_RED, "%dg", tr->cost[lvl][4]);
    }
    gfx_rect(0, SCREEN_H - 14, SCREEN_W, 14, RGBA(0, 0, 0, 255));
    ui_hints(BG_CROSS " Buy  " BG_TRIANGLE " Close");
}

/* ---------------------------------------------------------------- trinket widgets
 * Shared grid UI for the Nomad Wagon and the hero-sheet trinket slots. */

/* Cursor over a grid of n cells, cols wide (d-pad). */
/* Move a cursor in a grid with `cols` columns using the d-pad. */
static int grid_input(int s, int n, int cols) {
    if (n <= 0) return 0;
    if (input_pressed(BTN_LEFT) && s > 0) s--;
    if (input_pressed(BTN_RIGHT) && s < n - 1) s++;
    if (input_pressed(BTN_UP) && s >= cols) s -= cols;
    if (input_pressed(BTN_DOWN) && s + cols < n) s += cols;
    return s < n ? s : n - 1;
}

/* Trinket icon card (t = dd_trinkets index, -1 empty). */
/* One trinket icon card. */
static void trinket_card(float x, float y, int t, int selected) {
    gfx_rect(x, y, 25, 47, RGBA(20, 16, 12, 230));
    ui_frame(x, y, 25, 47, selected ? COL_SELECT : RGBA(70, 56, 38, 255));
    Image *ic = t >= 0 ? hud_item_icon(LK_TRINKET, t, 1) : NULL;
    if (ic) image_draw(ic, x + 12.5f - ic->w / 2.0f, y + 2, ic->w, ic->h, RGBA(255, 255, 255, 255));
}

/* Rows of trinket cards (index list; -1 = empty cell), scrolled so sel stays visible. */
/* A page of trinket cards. */
static void trinket_grid(float x, float y, const int *list, int n, int cols, int rows, int sel) {
    int first = sel / cols >= rows ? (sel / cols - rows + 1) * cols : 0;
    for (int i = first; i < n && i < first + cols * rows; i++)
        trinket_card(x + (i % cols) * 27, y + ((i - first) / cols) * 49, list[i], i == sel);
}

/* Name, rarity, class restriction and buffs of trinket t; returns the height used. */
/* Name, rarity and buff text for the selected trinket. Returns used height. */
static float trinket_info(float x, float y, float w, int t, const char *extra) {
    if (t < 0) return 0;
    const DDTrinket *d = &dd_trinkets[t];
    char b[256];
    text_draw(FONT_BODY, x, y, d->name, COL_GOLD, 0);
    snprintf(b, sizeof(b), "%s%s%s%s", trinket_rarity_name(d->rarity), d->cls >= 0 ? "  -  " : "",
             d->cls >= 0 ? dd_heroes[d->cls].name : "", d->cls >= 0 ? " only" : "");
    text_draw(FONT_SMALL, x, y + 14, b, COL_DIM, 0);
    if (extra) text_draw(FONT_SMALL, x + w, y + 14, extra, COL_GOLD, 2);
    int lines = text_draw_wrapped(FONT_SMALL, x, y + 25, w, trinket_describe(t, b, sizeof(b)), COL_TEXT);
    return 25 + lines * text_line_height(FONT_SMALL);
}

/* Fill out[] with the stash indices. */
static int stash_list(int *out) {
    for (int i = 0; i < g_camp.nstash; i++) out[i] = g_camp.stash[i];
    return g_camp.nstash;
}

/* Nomad Wagon: buy from this week's stock, sell from the stash. */
static void nomad_update(void) {
    if (input_pressed(BTN_SQUARE)) { phase = !phase; sel = 0; }
    int n = phase ? g_camp.nstash : wagon_count();
    sel = grid_input(sel, n, 10);
    if (input_pressed(BTN_CIRCLE)) { st = TS_HAMLET; return; }
    if (!input_pressed(BTN_CROSS) || !n) return;
    char b[96];
    if (phase) {
        int t = g_camp.stash[sel], v = trinket_sell_value(t);
        g_camp.gold += v;
        stash_remove(sel);
        if (sel >= g_camp.nstash && sel > 0) sel--;
        snprintf(b, sizeof(b), "Sold %s for %d gold.", dd_trinkets[t].name, v);
        notify(b);
        return;
    }
    int t = g_camp.wagon[sel] - 1;
    if (t < 0) notify("Sold out.");
    else if (g_camp.gold < wagon_price(t)) notify("Not enough gold.");
    else if (stash_add(t) != 0) notify("The trinket chest is full.");
    else {
        g_camp.gold -= wagon_price(t);
        g_camp.wagon[sel] = 0;
        snprintf(b, sizeof(b), "Bought %s.", dd_trinkets[t].name);
        notify(b);
    }
}

/* Draw the wagon's two grids and info. */
static void nomad_draw(float px, float py, float pw) {
    int list[STASH_MAX] = {0}, n;
    if (phase) n = stash_list(list);
    else {
        n = wagon_count();
        for (int i = 0; i < n; i++) list[i] = g_camp.wagon[i] - 1;
    }
    text_draw(FONT_BODY, px + 8, py + 4, phase ? "Sell your trinkets" : "Trinkets for sale this week", COL_TEXT, 0);
    text_draw(FONT_SMALL, px + pw - 8, py + 6, phase ? BG_SQUARE " Buy" : BG_SQUARE " Sell", COL_DIM, 2);
    trinket_grid(px + 8, py + 22, list, n, 10, 2, sel);
    if (!n) text_draw(FONT_BODY, px + 8, py + 40, phase ? "Your trinket chest is empty." : "Sold out until next week.", COL_DIM, 0);
    else if (sel < n && list[sel] >= 0) {
        char pr[32];
        int t = list[sel];
        snprintf(pr, sizeof(pr), phase ? "Sells for %d" : "%d gold", phase ? trinket_sell_value(t) : wagon_price(t));
        trinket_info(px + 8, py + 126, pw - 16, t, pr);
    } else if (sel < n) text_draw(FONT_BODY, px + 8, py + 126, "Sold.", COL_DIM, 0);
    text_printf(FONT_SMALL, px + 8, py + 218, COL_DIM, "Gold %d   Chest %d/%d", g_camp.gold, g_camp.nstash, STASH_MAX);
    ui_hints(phase ? BG_CROSS " Sell  " BG_SQUARE " Buy  " BG_CIRCLE " Back" : BG_CROSS " Buy  " BG_SQUARE " Sell  " BG_CIRCLE " Back");
}

static void building_switch(int dir);
/* Input inside a building. One `case` per building type implements what
 * that building does: Stage Coach hires recruits; Guild upgrades skills;
 * Blacksmith upgrades weapon/armour; Tavern/Abbey assign heroes to
 * activities; Survivalist unlocks camping skills; Graveyard lists the dead;
 * Sanitarium and Nomad Wagon delegate to their own functions. L/R switch
 * building, Triangle opens upgrades, O leaves. */
static void building_update(void) {
    Hero *h = g_camp.nroster ? &g_camp.roster[hero_sel] : NULL;
    if (upg_open) { upgrades_update(order[cur]); return; }
    if (input_pressed(BTN_L)) { building_switch(-1); return; }
    if (input_pressed(BTN_R)) { building_switch(1); return; }
    { int tt[8]; if (input_pressed(BTN_TRIANGLE) && building_trees(order[cur], tt)) { upg_open = 1; upg_sel = 0; return; } }
    switch (cur >= 0 ? order[cur] : 0) {
    case B_STAGE:
        sel = ui_menu_input(sel, (int)tree_value("stage_coach.numrecruits", TV_AMOUNT, 2));
        if (input_pressed(BTN_CROSS)) {
            if (!g_camp.recruits[sel].alive) notify("Already recruited.");
            else if (campaign_hire(sel) != 0) notify(dd_ui_str[US_COACH_FULL]);
            else {
                static const struct { const char *cls, *vo; } rv[] = {{"crusader", "cru"}, {"arbalest", "arb"}, {"abomination", "abm"},
                    {"antiquarian", "antq"}, {"bounty_hunter", "bh"}, {"grave_robber", "grv"}, {"hellion", "hll"}, {"houndmaster", "hnd"},
                    {"highwayman", "hwy"}, {"jester", "jest"}, {"leper", "lep"}, {"man_at_arms", "maa"}, {"occultist", "occ"},
                    {"plague_doctor", "plg"}, {"vestal", "vst"}};
                const Hero *nh = &g_camp.roster[g_camp.nroster - 1];
                for (unsigned i = 0; i < sizeof rv / sizeof *rv; i++)
                    if (!strcmp(dd_heroes[nh->cls].id, rv[i].cls)) { char nm[40]; snprintf(nm, sizeof nm, "town_rec_%s", rv[i].vo); audio_narrate_important(nm, 100); }
                notify("A new hero joins the hamlet.");
            }
        }
        if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
        break;
    case B_GUILD:
    case B_SMITH: {
        if (phase == 0) {
            sel = ui_menu_input(sel, g_camp.nroster);
            if (input_pressed(BTN_CROSS) && g_camp.nroster) { hero_sel = sel; phase = 1; sub = 0; }
            if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
            break;
        }
        int guild = order[cur] == B_GUILD;
        int n = guild ? dd_heroes[h->cls].skill_count : 2;
        sub = ui_menu_input(sub, n);
        if (input_pressed(BTN_CIRCLE)) { phase = 0; sel = hero_sel; break; }
        if (guild && input_pressed(BTN_SQUARE)) {
            int slot = -1, free = -1;
            for (int i = 0; i < HERO_SKILL_SLOTS; i++) {
                if (h->equipped[i] == sub) slot = i;
                if (h->equipped[i] < 0 && free < 0) free = i;
            }
            if (slot >= 0) h->equipped[slot] = -1;
            else if (free >= 0) h->equipped[free] = (int8_t)sub;
            else notify("Only four skills can be equipped.");
        }
        if (input_pressed(BTN_CROSS)) {
            int cost = guild ? upgrade_skill_cost(h, sub) : sub == 0 ? upgrade_weapon_cost(h) : upgrade_armour_cost(h);
            if (cost < 0) notify("Locked: upgrade the building (/\\) or raise resolve.");
            else if (g_camp.gold < cost) notify("Not enough gold.");
            else {
                g_camp.gold -= cost;
                if (!guild) town_event_paid(sub == 0 ? 1 : 0);
                if (guild) h->skill_lvl[sub]++;
                else if (sub == 0) h->weapon_lvl++;
                else { h->armour_lvl++; h->hp = (int16_t)hero_max_hp(h); }
                notify("Upgraded.");
            }
        }
        break;
    }
    case B_SANITARIUM: sanitarium_update(h); break;
    case B_NOMAD: nomad_update(); break;
    case B_TAVERN:
    case B_ABBEY: {
        int abbey = order[cur] == B_ABBEY;
        if (phase == 0) {
            sel = ui_menu_input(sel, 3);
            if (input_pressed(BTN_CROSS)) { activity = sel; phase = 1; sub = 0; }
            if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
            break;
        }
        sub = ui_menu_input(sub, g_camp.nroster);
        if (input_pressed(BTN_CIRCLE)) { phase = 0; sel = activity; break; }
        if (input_pressed(BTN_CROSS) && g_camp.nroster) {
            Hero *x = &g_camp.roster[sub];
            /* estate hero_activity_cost_multiplier_table: higher resolve levels pay more */
            int cost = (int)(act_cost(abbey, activity) * g_rules->act_mult[x->level < 7 ? x->level : 6] + 0.5f);
            if (town_event_locked(abbey * 3 + activity)) notify("Closed this week.");
            else if (x->busy) notify("That hero is already busy this week.");
            else if (act_refused(x, abbey * 3 + activity)) { /* not_have_quirks */ }
            else if (act_used(abbey, activity) >= act_slots(abbey, activity)) notify("No free slot. Upgrade the building.");
            else if (g_camp.gold < cost) notify("Not enough gold.");
            else {
                g_camp.gold -= cost;
                int before = x->stress, aff = x->affliction;
                stress_on_town_week(x, town_event_relief(abbey * 3 + activity, sub, act_relief(abbey, activity)));
                x->act_relief = (uint8_t)(before - x->stress);
                x->act_cured = (uint8_t)(aff && !x->affliction ? aff : 0);
                x->busy = (uint8_t)(1 + abbey * 3 + activity); /* which activity, for slot counts */
                { static const char *const as[2][3] = {{"town_tavern_bar", "town_tavern_gambling", "town_tavern_brothel_v1"},
                                                      {"town_abbey_meditation", "town_abbey_prayer", "town_abbey_flagellation"}};
                  audio_sfx(as[abbey ? 1 : 0][activity % 3]); }
                char b[96];
                snprintf(b, sizeof(b), "%s spends the week at the %s.", x->name, activities[abbey][activity].name);
                notify(b);
            }
        }
        break;
    }
    default:
        if (input_pressed(BTN_CIRCLE) || input_pressed(BTN_CROSS)) st = TS_HAMLET;
        break;
    }
}

/* building_navigation.layout: one icon per building down the left edge (button_spacing 68 at 1080p),
 * the open one hopped up (selected_hop_offset), unbuilt ones dimmed. L/R switch buildings. */
/* Vertical strip of building icons at the left edge for quick navigation:
 * the current one is highlighted, locked ones dimmed. Icons load lazily. */
static void nav_draw(void) {
    gfx_rect(0, 46, 25, norder * 19 + 4, RGBA(0, 0, 0, 170));
    for (int i = 0; i < norder; i++) {
        int b = order[i];
        if (!nav_tried[b]) {
            char p[64];
            snprintf(p, sizeof(p), "assets/ui/%s_icon.ddi", bld[b].id);
            image_load(&nav_icon[b], p);
            nav_tried[b] = 1;
        }
        float y = 48 + i * 19, x = i == cur ? 2 : 4;
        int open = building_unlocked(bld[b].id);
        if (nav_icon[b].tiles)
            image_draw(&nav_icon[b], x, y, 18, 18, i == cur ? RGBA(255, 255, 255, 255) : open ? RGBA(170, 170, 170, 255) : RGBA(55, 55, 55, 255));
        if (i == cur) ui_frame(x - 1, y - 1, 20, 20, COL_SELECT);
    }
}

/* Move to the next/previous unlocked building without leaving the screen. */
static void building_switch(int dir) {
    int c = cur;
    for (int k = 0; k < norder; k++) {
        c = (c + dir + norder) % norder;
        if (building_unlocked(bld[order[c]].id)) break;
    }
    if (c == cur) return;
    cur = c;
    sel = sub = phase = 0;
    hero_sel = 0;
    upg_open = 0;
}

/* Draw the inside of the current building (backdrop, character, then the
 * per-building panel). */
static void building_draw(void) {
    int b = order[cur];
    load_building(b);
    if (bimg_bg.tiles) image_draw(&bimg_bg, 0, 0, SCREEN_W, SCREEN_H, RGBA(150, 140, 130, 255));
    if (bimg_char.tiles) image_draw(&bimg_char, -10, SCREEN_H - bimg_char.h - 4, bimg_char.w, bimg_char.h, RGBA(255, 255, 255, 255));
    nav_draw();
    text_draw(FONT_TITLE, 12, 18, bld[b].name, COL_GOLD, 0);
    float px = 186, py = 22, pw = 286, ph = 232;
    ui_panel(px, py, pw, ph);
    Hero *h = g_camp.nroster ? &g_camp.roster[hero_sel] : NULL;
    switch (b) {
    case B_STAGE:
        text_draw(FONT_BODY, px + 8, py + 4, "Recruits arrive each week.", COL_DIM, 0);
        for (int i = 0; i < (int)tree_value("stage_coach.numrecruits", TV_AMOUNT, 2) && i < RECRUITS; i++) {
            if (g_camp.recruits[i].alive) draw_hero_row(px + 8, py + 22 + i * 28, pw - 16, &g_camp.recruits[i], i == sel, "Free");
            else text_draw(FONT_BODY, px + 8, py + 26 + i * 28, "(recruited)", i == sel ? COL_SELECT : COL_DIM, 0);
        }
        text_printf(FONT_BODY, px + 8, py + ph - 16, COL_DIM, "Roster %d/%d", g_camp.nroster, campaign_roster_size());
        if (dd_coach_idle_count && sel < RECRUITS && g_camp.recruits[sel].alive) { /* the selected recruit's bark */
            const Hero *r = &g_camp.recruits[sel];
            char bark[160];
            snprintf(bark, sizeof(bark), "%s: \"%s\"", r->name, dd_coach_idle[(g_camp.week * 5 + sel * 3 + r->cls) % dd_coach_idle_count]);
            ui_panel(30, 208, 152, 40);
            text_draw_wrapped(FONT_SMALL, 36, 212, 140, bark, COL_TEXT);
        }
        ui_hints(BG_CROSS " Recruit  " BG_CIRCLE " Back");
        break;
    case B_SANITARIUM: sanitarium_draw(h, px, py, pw); break;
    case B_NOMAD: nomad_draw(px, py, pw); break;
    case B_GUILD:
    case B_SMITH:
        if (phase == 0) {
            text_draw(FONT_BODY, px + 8, py + 4, "Choose a hero.", COL_DIM, 0);
            draw_roster(px + 8, py + 22, pw - 16, 8, sel, b == B_SMITH ? gear_text : NULL);
            ui_hints(BG_CROSS " Choose  " BG_CIRCLE " Back");
        } else if (b == B_GUILD) {
            const DDHeroClass *c = &dd_heroes[h->cls];
            text_printf(FONT_BODY, px + 8, py + 4, COL_TEXT, "%s - skills (* equipped)", h->name);
            for (int s = 0; s < c->skill_count; s++) {
                float y = py + 22 + s * 28;
                int eq = 0;
                for (int i = 0; i < HERO_SKILL_SLOTS; i++) eq |= h->equipped[i] == s;
                if (s == sub) gfx_rect(px + 4, y - 2, pw - 8, 26, RGBA(90, 66, 30, 210));
                Image *ic = ui_skill_icon(h->cls, s);
                if (ic) image_draw(ic, px + 8, y, 22, 22, eq ? RGBA(255, 255, 255, 255) : RGBA(110, 110, 110, 255));
                const DDSkill *sk = &dd_skills[c->skill_first + s * 5 + h->skill_lvl[s]];
                text_printf(FONT_BODY, px + 36, y + 4, eq ? COL_TEXT : COL_DIM, "%s%s  Rank %d", eq ? "* " : "", sk->name, h->skill_lvl[s] + 1);
                int cost = upgrade_skill_cost(h, s);
                if (cost >= 0) text_printf(FONT_BODY, px + pw - 12 - text_width(FONT_BODY, "0000 g"), y + 4, COL_GOLD, "%d g", cost);
            }
            ui_hints(BG_CROSS " Upgrade  " BG_SQUARE " Equip  " BG_CIRCLE " Back");
        } else {
            const DDHeroClass *c = &dd_heroes[h->cls];
            text_printf(FONT_BODY, px + 8, py + 4, COL_TEXT, "%s - equipment", h->name);
            for (int i = 0; i < 2; i++) {
                float y = py + 26 + i * 70;
                if (i == sub) gfx_rect(px + 4, y - 4, pw - 8, 62, RGBA(90, 66, 30, 210));
                if (act_icons[i].tiles) image_draw(&act_icons[i], px + 8, y, 36, 36, RGBA(255, 255, 255, 255));
                int lvl = i ? h->armour_lvl : h->weapon_lvl, cost = i ? upgrade_armour_cost(h) : upgrade_weapon_cost(h);
                text_printf(FONT_BODY, px + 52, y, COL_TEXT, "%s  tier %d", i ? "Armour" : "Weapon", lvl + 1);
                if (i == 0) {
                    const DDWeapon *w = &c->weapon[lvl];
                    text_printf(FONT_BODY, px + 52, y + 13, COL_DIM, "DMG %d-%d  CRIT %d%%  SPD %d", w->dmg_lo, w->dmg_hi, (int)(w->crit * 100 + 0.5f), w->spd);
                } else {
                    const DDArmour *a = &c->armour[lvl];
                    text_printf(FONT_BODY, px + 52, y + 13, COL_DIM, "HP %d  DODGE %d", a->hp, (int)(a->def * 100 + 0.5f));
                }
                if (cost >= 0) text_printf(FONT_BODY, px + 52, y + 28, COL_GOLD, "Upgrade: %d g", cost);
                else text_draw(FONT_BODY, px + 52, y + 28, lvl >= 4 ? "Fully upgraded" : "Needs more resolve", COL_DIM, 0);
            }
            ui_hints(BG_CROSS " Upgrade  " BG_CIRCLE " Back");
        }
        break;
    case B_TAVERN:
    case B_ABBEY: {
        int abbey = b == B_ABBEY;
        if (phase == 0) {
            text_draw(FONT_BODY, px + 8, py + 4, "Stress relief for a week; ends afflictions.", COL_DIM, 0);
            for (int i = 0; i < 3; i++) {
                float y = py + 24 + i * 46;
                if (i == sel) gfx_rect(px + 4, y - 3, pw - 8, 42, RGBA(90, 66, 30, 210));
                if (act_icons[i].tiles) image_draw(&act_icons[i], px + 8, y, 36, 36, RGBA(255, 255, 255, 255));
                text_draw(FONT_TITLE, px + 52, y, activities[abbey][i].name, i == sel ? COL_SELECT : COL_TEXT, 0);
                text_printf(FONT_BODY, px + 52, y + 20, COL_DIM, "%d g   -%d stress   %d/%d slots", act_cost(abbey, i), act_relief(abbey, i), act_used(abbey, i), act_slots(abbey, i));
            }
            ui_hints(BG_CROSS " Choose  " BG_CIRCLE " Back");
        } else {
            text_printf(FONT_BODY, px + 8, py + 4, COL_TEXT, "%s: who will go?", activities[abbey][activity].name);
            draw_roster(px + 8, py + 22, pw - 16, 8, sub, NULL);
            ui_hints(BG_CROSS " Send  " BG_CIRCLE " Back");
        }
        break;
    }
    case B_GRAVEYARD:
        text_draw(FONT_BODY, px + 8, py + 4, "Here lie the fallen.", COL_DIM, 0);
        for (int i = 0; i < g_camp.nfallen && i < 13; i++) {
            const Fallen *f = &g_camp.graveyard[g_camp.nfallen - 1 - i];
            text_printf(FONT_BODY, px + 8, py + 22 + i * 15, COL_TEXT, "%s the %s, level %d - week %d", f->name, dd_heroes[f->cls].name, f->level, f->week);
        }
        if (!g_camp.nfallen) text_draw(FONT_BODY, px + 8, py + 22, "None yet.", COL_DIM, 0);
        ui_hints(BG_CIRCLE " Back");
        break;
    case B_SURVIVAL:
        text_draw(FONT_BODY, px + 8, py + 4, "Camping skills are taught here.", COL_DIM, 0);
        ui_hints(BG_CIRCLE " Back");
        break;
    default:
        ui_hints(BG_CIRCLE " Back");
        break;
    }
    if (upg_open) upgrades_draw(b);
    else {
        int tt[8];
        if (building_trees(b, tt)) text_draw(FONT_SMALL, 12, 40, BG_TRIANGLE " Upgrades", COL_DIM, 0);
    }
}

/* ---------------------------------------------------------------- roster & hero
 * The character sheet: art pieces are loaded by id via sheet(). */

/* Character sheet over the game's panel art (shared/character/character.layout positions at 480/1395). */
static Image sheet_art[9], sheet_camp[16];
static int tfocus, tpick, tpick_sel; /* hero sheet: trinket slot focused (1/2, 0 = skills), picker open */
static uint8_t sheet_tried[9];
static int sheet_camp_cls = -1;

/* Lazy-load character-sheet art piece id. */
static Image *sheet(int id) {
    static const char *const name[9] = {"panel", "res_stun", "res_poison", "res_bleed", "res_disease", "res_move",
                                        "res_debuff", "res_death_blow", "res_trap"};
    if (!sheet_tried[id]) {
        char p[48];
        snprintf(p, sizeof(p), "assets/hud/sheet/%s.ddi", name[id]);
        image_load(&sheet_art[id], p);
        sheet_tried[id] = 1;
    }
    return sheet_art[id].tiles ? &sheet_art[id] : NULL;
}

/* Free the sheet art. */
static void sheet_release(void) {
    for (int i = 0; i < 9; i++) image_free(&sheet_art[i]);
    for (int i = 0; i < 16; i++) image_free(&sheet_camp[i]);
    memset(sheet_tried, 0, sizeof(sheet_tried));
    sheet_camp_cls = -1;
}

/* Fills one of the panel's rank circles. */
/* A stress pip: a small plus-shaped mark made of two overlapping rectangles. */
static void pip(float x, float y, int on, uint32_t col) {
    if (!on) return;
    gfx_rect(x - 2.5f, y - 3.5f, 5, 7, col);
    gfx_rect(x - 3.5f, y - 2.5f, 7, 5, col);
}

/* Draw a hero's full sheet: portrait, resolve, stats, resistances, skills,
 * quirks, equipment and trinket slots. */
static void hero_draw(const Hero *h) {
    const DDHeroClass *c = &dd_heroes[h->cls];
    const uint32_t white = RGBA(255, 255, 255, 255);
    const float k = 480.0f / 1395, y0 = 2;
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 255));
    Image *bg = sheet(0);
    if (bg) image_draw(bg, 0, y0, bg->w, bg->h, white);

    /* left column: name, class, the hero */
    text_draw(FONT_TITLE, 76 * k, 26 * k + y0, h->name, COL_GOLD, 0);
    text_draw(FONT_SMALL, 76 * k, 80 * k + y0, c->name, COL_DIM, 0);
    const uint8_t tint[4] = {255, 255, 255, 255};
    sprite_draw(sprite_get(c->sprite, "idle"), t, 1, 98 * k, 700 * k + y0, 0.85f, 0, tint);

    /* resolve emblem with positive quirks left of it, negative quirks and diseases right */
    char b[48];
    int need = h->level < 7 ? g_rules->level_xp[h->level + 1] : 0;
    if (need) snprintf(b, sizeof(b), "Quirks          Resolve %d  (XP %d/%d)", h->level, h->xp, need);
    else snprintf(b, sizeof(b), "Quirks          Resolve %d", h->level);
    text_draw(FONT_SMALL, 141 * k + 6, 128 * k + y0 + 6, b, COL_TEXT, 0);
    snprintf(b, sizeof(b), "%d", h->level);
    text_draw(FONT_TITLE, 160, 70, b, COL_GOLD, 1);
    int np = 0, nn = 0;
    for (int i = 0; i < HERO_QUIRKS; i++) {
        if (!h->quirks[i]) continue;
        const DDQuirk *q = &dd_quirks[h->quirks[i] - 1];
        int lock = (h->quirk_locked >> i) & 1;
        snprintf(b, sizeof(b), "%s%s", q->name, lock ? " *" : "");
        if (q->positive) text_draw(FONT_SMALL, 56, 60 + 10 * np++, b, COL_GOOD, 0);
        else text_draw(FONT_SMALL, 186, 60 + 10 * nn++, b, q->disease ? RGBA(170, 200, 60, 255) : COL_RED, 0);
    }
    int virtue = 0;
    const char *tr = stress_affliction_name(h, &virtue);
    if (tr) text_draw(FONT_SMALL, 76 * k, 92 * k + y0 + 10, tr, virtue ? COL_GOLD : RGBA(210, 90, 210, 255), 0);
    text_printf(FONT_SMALL, 76 * k, 92 * k + y0 + 20, COL_DIM, "Stress %d", h->stress);

    /* base stats */
    HudHero v;
    hud_hero(&v, h, NULL);
    text_draw(FONT_SMALL, 141 * k + 6, 358 * k + y0 + 6, "Base Stats", COL_TEXT, 0);
    char col1[4][24], col2[4][24];
    snprintf(col1[0], 24, "Max HP  %d", v.max_hp);
    snprintf(col1[1], 24, "Dodge  %d", (int)floorf(v.dodge * 100 + 0.5f));
    snprintf(col1[2], 24, "Prot  %d", (int)floorf(v.prot * 100 + 0.5f));
    snprintf(col1[3], 24, "Speed  %d", v.spd);
    snprintf(col2[0], 24, "Acc Mod  %d", (int)floorf(v.acc * 100 + 0.5f));
    snprintf(col2[1], 24, "Crit  %d%%", (int)floorf(v.crit * 100 + 0.5f));
    snprintf(col2[2], 24, "Dmg  %d-%d", v.dmg_lo, v.dmg_hi);
    snprintf(col2[3], 24, "HP  %d/%d", h->hp, v.max_hp);
    for (int r = 0; r < 4; r++) {
        text_draw(FONT_SMALL, 56, 137 + r * 9, col1[r], COL_TEXT, 0);
        text_draw(FONT_SMALL, 150, 137 + r * 9, col2[r], COL_TEXT, 0);
    }

    /* equipment */
    text_draw(FONT_SMALL, 141 * k + 6, 516 * k + y0 + 6, "Equipment", COL_TEXT, 0);
    Image *w = hud_hero_equip(h->cls, 0, h->weapon_lvl), *ar = hud_hero_equip(h->cls, 1, h->armour_lvl);
    if (w) image_draw(w, 97, 210, 18, 36, white);
    if (ar) image_draw(ar, 128, 210, 18, 36, white);
    text_printf(FONT_SMALL, 96, 248, COL_DIM, "W%d", h->weapon_lvl + 1);
    text_printf(FONT_SMALL, 127, 248, COL_DIM, "A%d", h->armour_lvl + 1);
    for (int s2 = 0; s2 < HERO_TRINKETS; s2++) { /* the panel's two trinket slots */
        float x = 168 + s2 * 31;
        Image *ic = h->trinkets[s2] ? hud_item_icon(LK_TRINKET, h->trinkets[s2] - 1, 1) : NULL;
        if (ic) image_draw(ic, x + 14 - ic->w / 2.0f, 207, ic->w, ic->h, white);
        if (tfocus == s2 + 1) ui_frame(x, 204, 28, 51, COL_SELECT);
    }

    /* combat skills: icons, selected one's launch ranks (4..1) and targets (1..4) */
    int ns = c->skill_count < 8 ? c->skill_count : 8;
    if (sub >= ns) sub = 0;
    const DDSkill *sk = &dd_skills[c->skill_first + sub * 5 + h->skill_lvl[sub]];
    snprintf(b, sizeof(b), "%s (rank %d)", sk->name, h->skill_lvl[sub] + 1);
    text_draw(FONT_SMALL, 280, 16, b, COL_GOLD, 0);
    for (int r = 1; r <= 4; r++) pip(836 * k + (4 - r) * 47.5f * k, 134 * k + y0, sk->launch.mask & (1 << (r - 1)), COL_GOLD);
    uint32_t tc = sk->target.flags & RK_FRIENDLY ? COL_GOOD : COL_RED;
    for (int r = 1; r <= 4; r++)
        pip(1146 * k + (r - 1) * 47.5f * k, 134 * k + y0, !(sk->target.flags & RK_SELF) && (sk->target.mask & (1 << (r - 1))), tc);
    for (int s2 = 0; s2 < ns; s2++) {
        int eq = 0;
        for (int i = 0; i < HERO_SKILL_SLOTS; i++) eq |= h->equipped[i] == s2;
        float x = 272 + s2 * 27, y = 62;
        Image *ic = ui_skill_icon(h->cls, s2);
        if (ic) image_draw(ic, x, y, 22, 22, eq ? white : RGBA(90, 90, 90, 255));
        if (s2 == sub) ui_frame(x - 1, y - 1, 24, 24, COL_SELECT);
    }

    /* camping skills */
    text_draw(FONT_SMALL, 780 * k + 6, 288 * k + y0, "Camping Skills", COL_TEXT, 0);
    if (sheet_camp_cls != h->cls) {
        for (int i = 0; i < 16; i++) image_free(&sheet_camp[i]);
        sheet_camp_cls = h->cls;
    }
    int nc = 0;
    for (int i = 0; i < dd_camp_skill_count && nc < 7; i++) {
        if (!(dd_camp_skills[i].classes & (1 << h->cls))) continue;
        if (!sheet_camp[nc].tiles) {
            char p[80];
            snprintf(p, sizeof(p), "assets/camp/%s.ddi", dd_camp_skills[i].id);
            image_load(&sheet_camp[nc], p);
        }
        if (sheet_camp[nc].tiles) image_draw(&sheet_camp[nc], 272 + nc * 27, 112, 22, 22, white);
        nc++;
    }

    /* resistances */
    text_draw(FONT_SMALL, 780 * k + 6, 448 * k + y0, "Resistances", COL_TEXT, 0);
    for (int r = 0; r < 8; r++) {
        float x = 274 + (r % 4) * 50, y = 168 + (r / 4) * 16;
        Image *ic = sheet(1 + r);
        if (ic) image_draw(ic, x, y, ic->w, ic->h, white);
        text_printf(FONT_SMALL, x + 15, y + 1, COL_TEXT, "%d%%", (int)floorf((c->res[r] + quirk_stat(h, STAT_RES, r) + trinket_stat(h, STAT_RES, r)) * 100 + 0.5f));
    }
    text_draw(FONT_SMALL, 780 * k + 6, 618 * k + y0, "Diseases", COL_TEXT, 0);
    int nd = 0;
    for (int i = 0; i < HERO_QUIRKS; i++)
        if (h->quirks[i] && dd_quirks[h->quirks[i] - 1].disease)
            text_draw(FONT_SMALL, 280, 232 + 10 * (nd++ % 2), dd_quirks[h->quirks[i] - 1].name, RGBA(170, 200, 60, 255), 0);
    if (!nd) text_draw(FONT_SMALL, 280, 232, "None", COL_DIM, 0);

    /* focused trinket slot: its details over the right column; the picker lists the estate's chest */
    if (tpick) {
        int list[STASH_MAX] = {0}, n = stash_list(list);
        ui_panel(250, 20, 226, 236);
        text_printf(FONT_BODY, 258, 24, COL_TEXT, "Trinket chest (%d)", n);
        trinket_grid(258, 42, list, n, 8, 2, tpick_sel);
        if (n) trinket_info(258, 146, 210, list[tpick_sel], NULL);
        ui_hints(BG_CROSS " Equip  " BG_CIRCLE " Cancel");
        return;
    }
    if (tfocus && h->trinkets[tfocus - 1]) {
        ui_panel(250, 186, 226, 70);
        trinket_info(258, 190, 210, h->trinkets[tfocus - 1] - 1, NULL);
    }
    ui_hints(tfocus ? BG_LEFTRIGHT " Slot  " BG_CROSS " Change  " BG_SQUARE " Remove  " BG_UP " Skills  " BG_CIRCLE " Back" : BG_LEFTRIGHT " Skill  " BG_DOWN " Trinkets  " BG_SQUARE " Dismiss  " BG_CIRCLE " Back");
}

/* ---------------------------------------------------------------- embark
 * Three steps: pick a quest on the map (TS_EMBARK_QUEST), pick up to 4 heroes
 * (TS_EMBARK_PARTY), buy provisions (TS_EMBARK_PROVISION); then Start builds
 * the Dungeon and switches to SCENE_DUNGEON. */

/* Is roster hero idx already in the chosen party? */
static int in_party(int idx) {
    for (int r = 0; r < 4; r++)
        if (g_camp.party[r] == idx) return r;
    return -1;
}

/* Input for all three embark steps. The final confirmation pays for the
 * provisions, generates the map (dungeon_generate or a plot map), applies
 * town-event buffs, saves, and switches scene. */
static void embark_update(void) {
    if (st == TS_EMBARK_QUEST) {
        sel = ui_menu_input(sel, g_camp.nboard);
        if (g_camp.nboard && input_pressed(BTN_RIGHT)) sel = (sel + 1) % g_camp.nboard;
        if (g_camp.nboard && input_pressed(BTN_LEFT)) sel = (sel + g_camp.nboard - 1) % g_camp.nboard;
        if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
        if ((input_pressed(BTN_CROSS) || input_pressed(BTN_START)) && g_camp.nboard) {
            const QuestOffer *q = &g_camp.board[sel];
            emb_dungeon = q->dungeon;
            emb_level = q->level;
            emb_length = q->length;
            emb_quest = q->quest;
            emb_choice = sel;
            st = TS_EMBARK_PARTY;
            sub = 0;
            for (int r = 0; r < 4; r++) /* drop busy or dead heroes from last party */
                if (g_camp.party[r] >= 0 && (g_camp.party[r] >= g_camp.nroster || !hero_ok_for_party(g_camp.party[r]) ||
                                             too_experienced(g_camp.party[r]) ||
                                             (emb_dungeon == DG_DARKEST && g_camp.roster[g_camp.party[r]].dd_veteran)))
                    g_camp.party[r] = -1;
        }
        return;
    }
    if (st == TS_EMBARK_PARTY) {
        sub = ui_menu_input(sub, g_camp.nroster);
        if (input_pressed(BTN_CIRCLE)) { st = TS_EMBARK_QUEST; sel = emb_choice; }
        if (input_pressed(BTN_CROSS) && g_camp.nroster) {
            int r = in_party(sub);
            if (r >= 0) g_camp.party[r] = -1;
            else if (!hero_ok_for_party(sub)) notify("That hero is busy this week.");
            else if (too_experienced(sub)) notify("Too experienced for a quest this easy.");
            else if (emb_dungeon == DG_DARKEST && g_camp.roster[sub].dd_veteran) notify("\"Never again.\" They won't go back.");
            else {
                for (r = 0; r < 4 && g_camp.party[r] >= 0; r++) {}
                if (r < 4) g_camp.party[r] = sub;
                else notify("The party is full.");
            }
            int packed[4] = {-1, -1, -1, -1}, k = 0;
            for (r = 0; r < 4; r++)
                if (g_camp.party[r] >= 0) packed[k++] = g_camp.party[r];
            memcpy(g_camp.party, packed, sizeof(packed));
        }
        if (input_pressed(BTN_START) || input_pressed(BTN_TRIANGLE)) {
            int count = 0;
            for (int r = 0; r < 4; r++) count += g_camp.party[r] >= 0;
            if (!count) notify("Choose at least one hero.");
            else { st = TS_EMBARK_PROVISION; sel = 0; }
        }
        return;
    }
    /* provisions: one row per store item (firewood is free and given per quest length), then Embark */
    /* store cards: Left/Right pick, X buy one, Square return one, Start embark */
    const int nstore = ITEM_COUNT - 1; /* firewood is not sold */
    if (sel >= nstore) sel = 0;
    if (input_pressed(BTN_LEFT)) sel = (sel + nstore - 1) % nstore;
    if (input_pressed(BTN_RIGHT)) sel = (sel + 1) % nstore;
    int d = input_pressed(BTN_CROSS) - input_pressed(BTN_SQUARE);
    if (d) {
        int v = emb_items[sel] + d;
        int plot = g_camp.nboard && emb_choice < g_camp.nboard && g_camp.board[emb_choice].plot == PLOT_OLD_ROAD;
        if (d > 0 && !plot && provisions_cost() + town_event_item_price(sel, item_prices[sel]) > g_camp.gold) notify("Not enough gold.");
        else emb_items[sel] = (uint8_t)(v < 0 ? 0 : v > 16 ? 16 : v);
    }
    if (input_pressed(BTN_CIRCLE)) st = TS_EMBARK_PARTY;
    if (input_pressed(BTN_START)) {
        int cost = provisions_cost();
        if (g_camp.gold < cost) { notify("Not enough gold."); return; }
        g_camp.gold -= cost;
        Rng r;
        rng_seed(&r, g_camp.seed ^ (uint32_t)(g_camp.week * 7919) ^ plat_ticks_ms());
        int plot = g_camp.board[emb_choice].plot;
        if (plot >= PLOT_DD) dungeon_plot(&g_camp.dungeon, &r, plot - PLOT_DD);
        else dungeon_generate(&g_camp.dungeon, &r, emb_dungeon, emb_level, emb_length, emb_quest);
        g_camp.dungeon.heir = g_camp.board[emb_choice].heir;
        town_event_embark(&g_camp.dungeon);
        memcpy(g_camp.dungeon.items, emb_items, sizeof(emb_items));
        /* medium 1, long 2; the Darkest Dungeon's "exhausting" third quest (length 4) allows 4 camps (wiki) */
        g_camp.dungeon.items[ITEM_FIREWOOD] = (uint8_t)(emb_length >= 4 ? 4 : emb_length - 1);
        /* the quest leaves the board */
        memmove(&g_camp.board[emb_choice], &g_camp.board[emb_choice + 1], (g_camp.nboard - emb_choice - 1) * sizeof(QuestOffer));
        g_camp.nboard--;
        g_camp.in_dungeon = 1;
        image_free(&store_bg);
        image_free(&store_char);
        for (int i = 0; i < 13; i++) image_free(&qmap_art[i]);
        memset(qmap_tried, 0, sizeof(qmap_tried));
        campaign_save();
        scene_switch(SCENE_DUNGEON);
    }
}

/* Provision store: the merchant, a row of item cards, and the pack being filled. */
/* A provision card with count and price. */
static void item_card(float x, float y, int item, int count, int price, int selected) {
    gfx_rect(x, y, 25, 64, RGBA(20, 16, 12, 230));
    ui_frame(x, y, 25, 64, selected ? COL_SELECT : RGBA(70, 56, 38, 255));
    Image *ic = hud_item_icon(LK_ITEM, item, item == ITEM_FOOD ? (count ? count : 1) : 1);
    if (ic) image_draw(ic, x + 12.5f - ic->w / 2.0f, y + 2, ic->w, ic->h, count || price ? RGBA(255, 255, 255, 255) : RGBA(90, 90, 90, 255));
    char b[12];
    if (count) {
        snprintf(b, sizeof(b), "%d", count);
        text_draw(FONT_SMALL, x + 23, y + 36, b, COL_TEXT, 2);
    }
    if (price >= 0) {
        snprintf(b, sizeof(b), "%d", price);
        text_draw(FONT_SMALL, x + 12.5f, y + 52, b, COL_GOLD, 1);
    }
}

/* The provision store screen. */
static void store_draw(void) {
    if (!store_bg.tiles) image_load(&store_bg, "assets/hud/town/store_bg.ddi");
    if (!store_char.tiles) image_load(&store_char, "assets/hud/town/store_char.ddi");
    if (store_bg.tiles) image_draw(&store_bg, 0, 0, SCREEN_W, SCREEN_H, RGBA(255, 255, 255, 255));
    if (store_char.tiles) image_draw(&store_char, -10, SCREEN_H - store_char.h, store_char.w, store_char.h, RGBA(255, 255, 255, 255));
    static const char *const qn[3] = {"Explore", "Cleanse", "Slay the boss"}, *const ln[4] = {"", "Short", "Medium", "Long"};
    text_draw(FONT_TITLE, 8, 4, "Provisions", COL_GOLD, 0);
    text_printf(FONT_SMALL, 8, 26, COL_DIM, "%s  Lv%d  %s %s", dd_dungeon_names[emb_dungeon], emb_level, ln[emb_length <= 3 ? emb_length : 0], qn[emb_quest <= 2 ? emb_quest : 0]);
    int free_plot = provisions_cost() == 0 && g_camp.nboard && g_camp.board[emb_choice].plot == PLOT_OLD_ROAD;
    const float x0 = 228;
    gfx_rect(x0 - 4, 22, SCREEN_W - x0 + 2, 128, RGBA(0, 0, 0, 150));
    text_draw(FONT_SMALL, x0, 26, "Store", COL_TEXT, 0);
    for (int i = 0; i < ITEM_COUNT - 1; i++) item_card(x0 + i * 27, 38, i, 0, free_plot ? 0 : town_event_item_price(i, item_prices[i]), i == sel);
    text_draw(FONT_BODY, x0, 106, item_names[sel], COL_GOLD, 0);
    if (free_plot) text_printf(FONT_SMALL, x0, 120, COL_TEXT, "Free for this expedition   In pack: %d", emb_items[sel]);
    else text_printf(FONT_SMALL, x0, 120, COL_TEXT, "%d gold each   In pack: %d", town_event_item_price(sel, item_prices[sel]), emb_items[sel]);
    text_printf(FONT_SMALL, x0, 131, COL_DIM, BG_CROSS " Buy   " BG_SQUARE " Return");

    gfx_rect(x0 - 4, 154, SCREEN_W - x0 + 2, 90, RGBA(0, 0, 0, 150));
    text_draw(FONT_SMALL, x0, 158, "Your pack", COL_TEXT, 0);
    int k = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        int c = i == ITEM_FIREWOOD ? emb_length - 1 : emb_items[i];
        if (c > 0) item_card(x0 + (k++) * 27, 170, i, c, -1, 0);
    }
    if (!k) text_draw(FONT_SMALL, x0, 176, "Empty. Bring food and torches!", COL_DIM, 0);

    int cost = provisions_cost();
    text_printf(FONT_BODY, x0, 248, cost > g_camp.gold ? COL_RED : COL_GOLD, "Cost %d / Gold %d", cost, g_camp.gold);
    ui_hints(BG_START " Embark  " BG_CIRCLE " Back");
}

/* Quest selection on the estate map: quest icons beside each dungeon (quest_select.layout
 * quest_map_pos, rows of 4), details in the verbose panel on the left. */
/* Quest-map art piece (lazy). */
static Image *qmap(int id) {
    static const char *const name[13] = {"map", "verbose", "selected", "locked", "explore_1", "explore_3", "explore_5",
        "cleanse_1", "cleanse_3", "cleanse_5", "kill_boss_1", "kill_boss_3", "kill_boss_5"};
    if (!qmap_tried[id]) {
        char p[48];
        snprintf(p, sizeof(p), "assets/hud/quest/%s.ddi", name[id]);
        image_load(&qmap_art[id], p);
        qmap_tried[id] = 1;
    }
    return qmap_art[id].tiles ? &qmap_art[id] : NULL;
}

/* Where quest i's marker sits on the map: a per-dungeon anchor plus a small
 * offset so quests of the same dungeon don't overlap. */
static void quest_pos(int i, float *x, float *y) {
    static const float base[DG_COUNT][2] = {{940, 220}, {1000, 525}, {815, 400}, {1260, 400}, {1130, 110}}; /* ruins(crypts), weald, warrens, cove, darkest */
    const QuestOffer *q = &g_camp.board[i];
    int k = 0;
    for (int j = 0; j < i; j++) k += g_camp.board[j].dungeon == q->dungeon;
    *x = base[q->dungeon][0] * 0.25f + (k % 4) * 26;
    *y = base[q->dungeon][1] * 0.25f + (k / 4) * 26;
}

/* The quest map: dungeon regions with quest markers and a detail panel for
 * the selected one. */
static void quest_map_draw(void) {
    static const char *const qnames[3] = {"Explore", "Cleanse", "Slay the Boss"};
    static const char *const lnames[4] = {"", "Short", "Medium", "Long"};
    static const char *const goals[3] = {"Explore 90% of the rooms.", "Clear every room battle.", "Find and slay the boss."};
    const uint32_t white = RGBA(255, 255, 255, 255);
    Image *bg = qmap(0);
    if (bg) image_draw(bg, 0, 0, SCREEN_W, SCREEN_H, white);
    static const float base[DG_COUNT][2] = {{940, 220}, {1000, 525}, {815, 400}, {1260, 400}};
    for (int d = 0; d < DG_REGULAR; d++) {
        float x = base[d][0] * 0.25f, y = base[d][1] * 0.25f;
        if (!dungeon_unlocked(d)) {
            Image *lk = qmap(3);
            if (lk) image_draw(lk, x, y, lk->w, lk->h, white);
        }
    }
    for (int i = 0; i < g_camp.nboard; i++) {
        const QuestOffer *q = &g_camp.board[i];
        float x, y;
        quest_pos(i, &x, &y);
        int lv = q->level >= 5 ? 2 : q->level >= 3 ? 1 : 0;
        Image *ic = qmap(4 + (q->quest <= 2 ? q->quest : 2) * 3 + lv);
        if (ic) image_draw(ic, x, y, ic->w, ic->h, white);
        else gfx_rect(x, y, 24, 24, COL_GOLD);
        if (i == sel) {
            Image *ring = qmap(2);
            if (ring) image_draw(ring, x + 12 - ring->w / 2.0f, y + 12 - ring->h / 2.0f, ring->w, ring->h, white);
            else ui_frame(x - 2, y - 2, 28, 28, COL_SELECT);
        }
    }
    /* details */
    Image *vb = qmap(1);
    float px = 6, py = 30, pw = vb ? vb->w : 100;
    if (vb) image_draw(vb, px, py, vb->w, vb->h, white);
    else ui_panel(px, py, 100, 211);
    if (!g_camp.nboard) {
        text_draw_wrapped(FONT_SMALL, px + 8, py + 12, pw - 16, "No quests this week.", COL_DIM);
    } else {
        const QuestOffer *q = &g_camp.board[sel < g_camp.nboard ? sel : 0];
        Dungeon tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.level = q->level;
        tmp.length = q->length;
        tmp.heir = q->heir;
        float y = py + 10;
        if (q->plot >= PLOT_DD) { /* the Darkest Dungeon: its own names and goals */
            int qn = q->plot - PLOT_DD;
            const DDPlotQuest *pq = &dd_plot_quests[qn];
            text_draw(FONT_BODY, px + pw / 2, y, dd_dungeon_names[q->dungeon], COL_GOLD, 1);
            y += 15;
            if (qn < DD_PLOT_QUESTS) text_printf(FONT_SMALL, px + 8, y, COL_TEXT, "%d. %s", qn + 1, pq->name);
            else text_draw(FONT_SMALL, px + 8, y, pq->name, COL_TEXT, 0);
            y += 11;
            if (qn < DD_PLOT_QUESTS) text_draw(FONT_SMALL, px + 8, y, "Darkest", COL_DIM, 0);
            else text_printf(FONT_SMALL, px + 8, y, COL_DIM, "Level %d  %s", pq->level, pq->quest == QT_EXPLORE ? "Explore every room" : "Kill the boss");
            y += 14;
            y += text_draw_wrapped(FONT_SMALL, px + 8, y, pw - 16, pq->desc, COL_DIM) + 4;
            const char *rule = !pq->can_retreat ? "No retreat." : qn == PQ_TUTORIAL ? "" : "Retreat costs a hero's life.";
            y += text_draw_wrapped(FONT_SMALL, px + 8, y, pw - 16, rule, COL_DIM) + 4;
            if (qn < DD_PLOT_QUESTS) y += text_draw_wrapped(FONT_SMALL, px + 8, y, pw - 16, "Those who return never go back.", COL_DIM) + 8;
            else if (pq->ignore_upgrades)
                y += text_draw_wrapped(FONT_SMALL, px + 8, y, pw - 16, "Ignore it and the brigands wreck the estate.", COL_DIM) + 8;
            if (qn < DD_PLOT_QUESTS || pq->gold)
                text_printf(FONT_SMALL, px + 8, y, COL_TEXT, "%d gold, %d crests", (int)pq->gold, pq->crests);
            else if (pq->trinket >= 0) text_printf(FONT_SMALL, px + 8, y, COL_TEXT, "Reward: %s", dd_trinkets[pq->trinket].name);
            else text_draw(FONT_SMALL, px + 8, y, "Reward: a crow trinket", COL_TEXT, 0);
            ui_hints(BG_DPAD " Quest  " BG_CROSS " Choose  " BG_CIRCLE " Back");
            return;
        }
        text_draw(FONT_BODY, px + pw / 2, y, dd_dungeon_names[q->dungeon], COL_GOLD, 1);
        y += 15;
        text_printf(FONT_SMALL, px + 8, y, COL_TEXT, "%s %s", lnames[q->length <= 3 ? q->length : 0], qnames[q->quest <= 2 ? q->quest : 0]);
        y += 11;
        text_printf(FONT_SMALL, px + 8, y, COL_DIM, "Level %d%s", q->level, q->plot ? "  (story)" : "");
        y += 14;
        y += text_draw_wrapped(FONT_SMALL, px + 8, y, pw - 16, goals[q->quest <= 2 ? q->quest : 0], COL_DIM) + 8;
        text_draw(FONT_SMALL, px + 8, y, "Rewards", COL_GOLD, 0);
        y += 12;
        Image *g = hud_item_icon(LK_GOLD, 0, dungeon_reward(&tmp));
        if (g) image_draw(g, px + 8, y, 15, 30, white);
        Image *h = hud_item_icon(LK_HEIRLOOM, q->heir, 1);
        if (h) image_draw(h, px + 50, y, 15, 30, white);
        text_printf(FONT_SMALL, px + 25, y + 20, COL_TEXT, "%d", dungeon_reward(&tmp));
        text_printf(FONT_SMALL, px + 67, y + 20, COL_TEXT, "%d", dungeon_heirloom_reward(&tmp));
        y += 36;
        if (q->length >= 2) text_printf(FONT_SMALL, px + 8, y, COL_DIM, "Supplied: %d firewood", q->length - 1);
    }
    ui_hints(BG_DPAD " Quest  " BG_CROSS " Choose  " BG_CIRCLE " Back");
}

/* Draw whichever embark step is active. */
static void embark_draw(void) {
    static const char *const loadings[DG_COUNT] = {"crypts", "weald", "warrens", "cove", "darkestdungeon"};
    int want_loading = st == TS_EMBARK_QUEST && g_camp.nboard ? g_camp.board[sel < g_camp.nboard ? sel : 0].dungeon : emb_dungeon;
    if (loaded_loading != want_loading) {
        char p[64];
        image_free(&loading);
        snprintf(p, sizeof(p), "assets/loading/%s_0.ddi", loadings[st == TS_EMBARK_QUEST && g_camp.nboard ? g_camp.board[sel < g_camp.nboard ? sel : 0].dungeon : emb_dungeon]);
        image_load(&loading, p);
        loaded_loading = want_loading;
    }
    if (loading.tiles && st != TS_EMBARK_QUEST) {
        float w = loading.w * SCREEN_H / (float)loading.h;
        image_draw(&loading, (SCREEN_W - w) / 2, 0, w, SCREEN_H, RGBA(170, 160, 150, 255));
    }
    if (st == TS_EMBARK_QUEST) {
        quest_map_draw();
    } else if (st == TS_EMBARK_PARTY) {
        ui_panel(16, 18, 448, 236);
        text_draw(FONT_TITLE, 24, 22, "Assemble the Party", COL_GOLD, 0);
        for (int r = 0; r < 4; r++) { /* rank 1 on the right, like the battle line */
            float x = 380 - r * 60;
            gfx_rect(x, 42, 52, 40, RGBA(30, 24, 20, 230));
            int i = g_camp.party[r];
            if (i >= 0) {
                Image *p = ui_portrait(g_camp.roster[i].cls);
                if (p) image_draw(p, x + 8, 44, 36, 36, RGBA(255, 255, 255, 255));
            }
            text_printf(FONT_BODY, x + 18, 84, COL_DIM, "%d", r + 1);
        }
        draw_roster(24, 100, 432, 5, sub, NULL);
        ui_hints(BG_CROSS " Add/Remove  " BG_START " Provisions  " BG_CIRCLE " Back");
    } else {
        store_draw();
    }
}

/* ---------------------------------------------------------------- hamlet */

/* ---------------------------------------------------------------- estate frame
 * The common frame around town screens: gold/heirloom plate and the roster
 * panel. */

#define ROSTER_X (SCREEN_W - 96)
#define BAR_Y (SCREEN_H - 22)

static Image est_art[9];
static uint8_t est_tried[9];
enum { E_PLATE, E_ROSTER_BG, E_ROSTER_EL, E_ROSTER_TOP, E_GOLD, E_BUST, E_PORTRAIT, E_DEED, E_CREST };

/* Lazy estate art. */
static Image *est(int id) {
    static const char *const name[9] = {"nameplate", "roster_bg", "roster_el", "roster_top", "cur_gold",
                                        "cur_bust", "cur_portrait", "cur_deed", "cur_crest"};
    if (!est_tried[id]) {
        char p[64];
        snprintf(p, sizeof(p), "assets/hud/town/%s.ddi", name[id]);
        image_load(&est_art[id], p);
        est_tried[id] = 1;
    }
    return est_art[id].tiles ? &est_art[id] : NULL;
}

/* Draw estate art stretched to a rect. */
static void est_put(int id, float x, float y, float w, float h) {
    Image *im = est(id);
    if (im) image_draw(im, x, y, w > 0 ? w : im->w, h > 0 ? h : im->h, RGBA(255, 255, 255, 255));
}

/* Hint line inside the estate frame. */
static void estate_hints(const char *text) {
    text_draw(FONT_SMALL, ROSTER_X - 4, BAR_Y - 11, text, COL_DIM, 2);
}

/* Nameplate, roster sidebar and the gold / heirloom bar around the hamlet (estate screen). */
/* The estate frame: title plate, week number, active town event title, and
 * optionally the roster strip on the right. */
static void estate_ui(int roster) {
    est_put(E_PLATE, 0, 0, 0, 0);
    text_draw(FONT_TITLE, 80, 12, "The Hamlet", COL_GOLD, 1);
    text_printf(FONT_SMALL, 52, 31, COL_DIM, "Week %d", g_camp.week);
    if (town_event()) text_printf(FONT_SMALL, 90, 31, COL_GOLD, "%s", town_event()->title);

    if (roster) {
        gfx_rect(ROSTER_X, 0, 96, SCREEN_H, RGBA(0, 0, 0, 200));
        est_put(E_ROSTER_BG, ROSTER_X, 0, 96, SCREEN_H);
        est_put(E_ROSTER_TOP, ROSTER_X, 0, 96, 15);
        text_printf(FONT_SMALL, ROSTER_X + 48 - 20, 3, COL_TEXT, "%d / %d", g_camp.nroster, campaign_roster_size());
        int rows = (SCREEN_H - 18) / 26, first = st == TS_ROSTER && sel >= rows ? sel - rows + 1 : 0;
        for (int r = 0; r < rows && first + r < g_camp.nroster; r++) {
            int i = first + r;
            const Hero *h = &g_camp.roster[i];
            float y = 16 + r * 26;
            est_put(E_ROSTER_EL, ROSTER_X, y, 96, 26);
            if (st == TS_ROSTER && i == sel) ui_frame(ROSTER_X + 1, y, 94, 26, COL_SELECT);
            Image *p = ui_portrait(h->cls);
            if (p) image_draw(p, ROSTER_X + 4, y + 3, 20, 20, h->busy ? RGBA(110, 110, 110, 255) : RGBA(255, 255, 255, 255));
            text_draw(FONT_SMALL, ROSTER_X + 27, y + 3, h->name, COL_GOLD, 0);
            int virtue = 0;
            const char *tr = stress_affliction_name(h, &virtue);
            text_draw(FONT_SMALL, ROSTER_X + 27, y + 13, tr ? tr : dd_heroes[h->cls].name,
                      tr ? (virtue ? COL_GOLD : RGBA(210, 90, 210, 255)) : COL_DIM, 0);
            text_printf(FONT_SMALL, ROSTER_X + 84, y + 3, COL_TEXT, "%d", h->level);
            for (int k = 0; k < 10; k++) /* stress pips */
                gfx_rect(ROSTER_X + 27 + k * 4, y + 23, 3, 2, k < h->stress / 10 ? RGBA(235, 235, 235, 255) : RGBA(50, 50, 50, 220));
        }
    }

    gfx_rect(0, BAR_Y, ROSTER_X, 22, RGBA(8, 6, 5, 235));
    gfx_rect(0, BAR_Y, ROSTER_X, 1, COL_EDGE);
    float x = 8;
    est_put(E_GOLD, x, BAR_Y + 6, 0, 0);
    x = text_printf(FONT_BODY, x + 13, BAR_Y + 4, COL_GOLD, "%d", g_camp.gold) + 12;
    for (int k = 0; k < HEIR_COUNT; k++) {
        est_put(E_BUST + k, x, BAR_Y + 3, 0, 0);
        x = text_printf(FONT_BODY, x + 18, BAR_Y + 4, COL_TEXT, "%d", g_camp.heirlooms[k]) + 10;
    }
    float bw = 86, bx = ROSTER_X - bw - 4;
    ui_panel(bx, BAR_Y + 2, bw, 18);
    text_draw(FONT_TITLE, bx + bw / 2, BAR_Y + 2, "Embark", st == TS_HAMLET ? COL_GOLD : COL_DIM, 1);
    text_draw(FONT_SMALL, bx - 4, BAR_Y + 6, BG_START, COL_DIM, 2);
}

/* The town panorama: slowly drifting sky, the ground sprite, then buildings
 * drawn from z=5 down to z=-5 (painter's order: higher z first = behind),
 * with the selected one highlighted and the others slightly dimmed. */
static void hamlet_draw(void) {
    if (sky.tiles) {
        /* the sky drifts slowly (town.anim.darkest sky_anim) */
        float off = t * 3.0f;
        float sw = sky.w * 0.7f, sx = (sky.w - sw) / 2 + (off - (int)(off / 40) * 40) - 20;
        image_draw_part(&sky, sx, sw, 0, 0, SCREEN_W, SCREEN_H, RGBA(255, 255, 255, 255));
    }
    const uint8_t white[4] = {255, 255, 255, 255}, dim[4] = {150, 150, 160, 255};
    SpriteAnim *ground = sprite_get("town_ground", "idle");
    sprite_draw(ground, t, 1, 960 * TOWN_SCALE, TOWN_Y0 + 220 * TOWN_SCALE, 1.0f, 0, white);
    int sel_b = st == TS_HAMLET ? order[cur] : -1;
    for (int z = 5; z >= -5; z--)
        for (int i = 0; i < B_COUNT; i++) {
            if (bld[i].z != z) continue;
            char spr[32];
            snprintf(spr, sizeof(spr), "town_%s", bld[i].id);
            int lit = (sel_b < 0 || i == sel_b) && building_unlocked(bld[i].id);
            sprite_draw(sprite_get(spr, "idle"), t, 1, bld[i].x * TOWN_SCALE, TOWN_Y0 - bld[i].y * TOWN_SCALE, 1.0f, 0, lit ? white : dim);
        }
    estate_ui(1);
    if (st == TS_HAMLET) {
        int b = order[cur];
        float x = bld[b].x * TOWN_SCALE, y = TOWN_Y0 - bld[b].y * TOWN_SCALE - 64;
        if (y < 58) y = 58;
        float w = text_width(FONT_TITLE, bld[b].name) + 16;
        if (x - w / 2 < 2) x = w / 2 + 2;
        if (x + w / 2 > ROSTER_X - 2) x = ROSTER_X - 2 - w / 2;
        ui_panel(x - w / 2, y, w, 22);
        text_draw(FONT_TITLE, x, y + 2, bld[b].name, building_unlocked(bld[b].id) ? COL_GOLD : COL_DIM, 1);
        estate_hints(BG_CROSS " Enter  " BG_TRIANGLE " Roster  " BG_SQUARE " Exchange  " BG_L " Journal  " BG_R " Log  " BG_SELECT " End week  " BG_CIRCLE " Quit");
    }
}

/* Per-frame update: game-over/epilogue checks, then the `st` switch. The
 * hamlet state itself handles Left/Right (select building), X (enter), Start
 * (embark), Select (end the week), Triangle (roster). */
void town_update(float dt) {
    t += dt;
    if (note_t > 0) note_t -= dt;
    if (st == TS_HAMLET && g_camp.game_won == 1) { st = TS_EPILOG; epi_line = 0; epi_t = 0; }
    if (st != TS_GAMEOVER && campaign_check_game_over()) { st = TS_GAMEOVER; campaign_save(); }
    switch (st) {
    case TS_EPILOG: /* the narrator's epilogue (str_vo_epilog_N), one line per X */
        epi_t += dt;
        if (input_pressed(BTN_CROSS) || input_pressed(BTN_START)) {
            epi_t = 0;
            if (++epi_line >= dd_epilog_count) {
                g_camp.game_won = 2;
                campaign_save();
                st = TS_HAMLET;
            }
        }
        break;
    case TS_HAMLET:
        if (town_event() && !g_camp.event_seen) { /* the town event card, once per week */
            static const DDTownEvent *spoken;
            if (spoken != town_event()) {
                spoken = town_event();
                static const struct { const char *id, *vo; } ev[] = {
                    {"idle_resolve_level_plague_doctor", "idleresolveplague"}, {"idle_resolve_level_jester", "idleresolvejester"},
                    {"idle_resolve_level_highwayman", "idleresolvehighway"}, {"idle_resolve_level_grave_robber", "idleresolvegrave"},
                    {"idle_resolve_level_man_at_arms", "idleresolvemanatarms"}, {"idle_resolve_level_arbalest", "idleresolvearbalest"},
                    {"idle_resolve_level_houndmaster", "idleresolvehoundmaster"}, {"idle_resolve_level_abomination", "idleresolveabom"},
                    {"bonus_recruit_crusader", "recruitcrusader"}, {"bonus_recruit_bounty_hunter", "recruitbounty"},
                    {"bonus_recruit_vestal", "recruitvestal"}, {"bonus_recruit_hellion", "recruithellion"},
                    {"bonus_recruit_occultist", "recruitoccultist"}, {"bonus_recruit_leper", "recruitleper"},
                    {"bonus_recruit_antiquarian", "recruitanti"}, {"in_activity_buff_stress_heal_buff", "inactivitystressbuff"},
                    {"in_activity_buff_stress_heal_debuff", "inactivitystressdebuff"}, {"lock_bar_discount_tavern", "lockbar"},
                    {"lock_gambling_discount_tavern", "lockgambling"}, {"lock_brothel_discount_tavern", "lockbrothel"},
                    {"lock_meditation_discount_abbey", "lockmed"}, {"lock_prayer_discount_abbey", "lockprayer"},
                    {"lock_flagellation_discount_abbey", "lockflagg"}, {"provision_supply_discount", "provisiondiscount"},
                    {"provision_supply_free", "provisionfree"}, {"nomad_wagon_trinket_discount", "nomadwagondiscount"},
                    {"free_abbey", "freeabbey"}, {"free_tavern", "freetavern"}, {"free_sanitarium", "freesanitarium"},
                    {"free_all_activities", "townfair"}, {"dead_recruit", "deadrecruit"},
                    {"remove_quest_hero_level_restriction", "removequestherolevel"}, {"upgrade_tag_free_armour", "upgradearmour"},
                    {"upgrade_tag_free_weapon", "upgradeweapon"}, {"upgrade_tag_free_building_upgrade", "upgradebuilding"},
                    {"idle_stress_heal_buff", "idlestresshealbuff"}, {"embark_party_buff_crypts_buff", "buffcrypts"},
                    {"embark_party_buff_weald_buff", "buffweald"}, {"embark_party_buff_warrens_buff", "buffwarrens"},
                    {"embark_party_buff_cove_buff", "buffcove"},
                };
                for (unsigned i = 0; i < sizeof ev / sizeof *ev; i++)
                    if (!strcmp(town_event()->id, ev[i].id)) { char nm[64]; snprintf(nm, sizeof nm, "neut_townevent_%s", ev[i].vo); audio_narrate_important(nm, 100); }
            }
            if (input_pressed(BTN_CROSS) || input_pressed(BTN_CIRCLE)) g_camp.event_seen = 1;
            break;
        }
        if (input_pressed(BTN_LEFT)) cur = (cur + norder - 1) % norder;
        if (input_pressed(BTN_RIGHT)) cur = (cur + 1) % norder;
        if (input_pressed(BTN_CROSS)) {
            if (!building_unlocked(bld[order[cur]].id)) notify("Not yet rebuilt. Complete more quests.");
            else {
                static const char *const enter_snd[B_COUNT] = {"town_enter_coach", "town_enter_guild", "town_enter_blacksmith",
                    "town_enter_tavern", "town_enter_abbey", NULL, "town_enter_graveyard", "town_enter_sanitarium", NULL, NULL};
                if (enter_snd[order[cur]]) audio_sfx(enter_snd[order[cur]]);
                st = TS_BUILDING; sel = sub = phase = 0; hero_sel = 0; upg_open = 0;
            }
        }
        if (input_pressed(BTN_TRIANGLE)) { st = TS_ROSTER; sel = 0; }
        if (input_pressed(BTN_SQUARE)) { st = TS_EXCHANGE; sel = 0; }
        if (input_pressed(BTN_L)) { st = TS_JOURNAL; sel = 0; phase = 0; }
        if (input_pressed(BTN_R)) st = TS_LOG;
        if (input_pressed(BTN_START)) { st = TS_EMBARK_QUEST; sel = 0; audio_sfx("ui_town_button_setoff"); }
        if (input_pressed(BTN_SELECT)) {
            campaign_next_week();
            campaign_save();
            notify("A week passes in the hamlet.");
        }
        if (input_pressed(BTN_CIRCLE)) { st = TS_QUIT; sel = 0; }
        break;
    case TS_BUILDING: building_update(); break;
    case TS_ROSTER:
        sel = ui_menu_input(sel, g_camp.nroster);
        if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
        if (input_pressed(BTN_CROSS) && g_camp.nroster) { hero_sel = sel; st = TS_HERO; back_to = TS_ROSTER; sub = 0; }
        break;
    case TS_HERO: {
        Hero *h = &g_camp.roster[hero_sel];
        if (tpick) {
            tpick_sel = grid_input(tpick_sel, g_camp.nstash, 8);
            if (input_pressed(BTN_CIRCLE)) tpick = 0;
            if (input_pressed(BTN_CROSS) && g_camp.nstash) {
                const char *why = trinket_equip(h, tfocus - 1, tpick_sel);
                if (why) notify(why);
                else tpick = 0;
            }
            break;
        }
        if (input_pressed(BTN_DOWN)) tfocus = tfocus ? tfocus : 1;
        if (input_pressed(BTN_UP)) tfocus = 0;
        if (input_pressed(BTN_CIRCLE)) { st = back_to; sheet_release(); tfocus = 0; break; }
        if (tfocus) {
            if (input_pressed(BTN_LEFT)) tfocus = 1;
            if (input_pressed(BTN_RIGHT)) tfocus = 2;
            if (input_pressed(BTN_CROSS)) {
                if (!g_camp.nstash) notify("No trinkets in the chest. Find some, or visit the Nomad Wagon.");
                else { tpick = 1; tpick_sel = 0; }
            }
            if (input_pressed(BTN_SQUARE) && h->trinkets[tfocus - 1] && trinket_unequip(h, tfocus - 1) != 0)
                notify("The trinket chest is full.");
            break;
        }
        if (input_pressed(BTN_LEFT)) sub = sub > 0 ? sub - 1 : 0;
        if (input_pressed(BTN_RIGHT)) sub++; /* clamped by hero_draw */
        if (input_pressed(BTN_SQUARE)) {
            if (g_camp.nroster <= 1) notify("You can't dismiss your last hero.");
            else { campaign_dismiss(hero_sel); notify("Dismissed."); st = TS_ROSTER; sel = 0; }
        }
        break;
    }
    case TS_LOG:
        if (input_pressed(BTN_DOWN) && log_top < g_camp.nlog - 1) log_top++; /* scroll the entries */
        if (input_pressed(BTN_UP) && log_top > 0) log_top--;
        if (input_pressed(BTN_CROSS) || input_pressed(BTN_CIRCLE) || input_pressed(BTN_R)) {
            g_camp.log_unseen = 0;
            g_camp.nlog = 0; /* the log covers what happened since the hamlet was last seen */
            log_top = 0;
            st = TS_HAMLET;
        }
        break;
    case TS_GAMEOVER: /* Stygian: the campaign is lost for good */
        if (input_pressed(BTN_CROSS) || input_pressed(BTN_START)) scene_switch(SCENE_TITLE);
        break;
    case TS_JOURNAL: /* pages found (loot journal_page); X reads one */
        if (phase) { if (input_pressed(BTN_CIRCLE) || input_pressed(BTN_CROSS)) phase = 0; break; }
        sel = ui_menu_input(sel, JOURNAL_PAGES);
        if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
        if (input_pressed(BTN_CROSS) && (g_camp.journal >> sel & 1)) phase = 1;
        break;
    case TS_EXCHANGE: { /* heirloom exchange: trade one heirloom type for another at fixed rates */
        sel = ui_menu_input(sel, dd_heir_exchange_count);
        if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
        const DDHeirExchange *x = &dd_heir_exchange[sel];
        if (input_pressed(BTN_CROSS) && dd_heir_exchange_count) {
            if (g_camp.heirlooms[x->from] < x->from_n) notify("Not enough heirlooms.");
            else {
                g_camp.heirlooms[x->from] -= x->from_n;
                g_camp.heirlooms[x->to] += x->to_n;
                notify("Exchanged.");
            }
        }
        break;
    }
    case TS_QUIT:
        sel = ui_menu_input(sel, 2);
        if (input_pressed(BTN_CIRCLE)) st = TS_HAMLET;
        if (input_pressed(BTN_CROSS)) {
            if (sel == 0) st = TS_HAMLET;
            else { campaign_save(); scene_switch(SCENE_TITLE); }
        }
        break;
    default: embark_update(); break;
    }
}

/* Draw by state (hamlet, building, roster, hero sheet, embark wizard, overlays). */
void town_draw(void) {
    if (st == TS_GAMEOVER) { /* game_over.layout: estate name, weeks, dead heroes, reason */
        static Image bg;
        if (!bg.tiles) image_load(&bg, "assets/ui/game_over.ddi");
        if (bg.tiles) image_draw(&bg, 0, 0, SCREEN_W, SCREEN_H, RGBA(255, 255, 255, 255));
        else gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 255));
        /* estate_info_pos 400 70 + offsets, 1920x1080 scaled by 1/4 */
        char b[32];
        text_draw(FONT_TITLE, 242, 33, "Game Over", COL_GOLD, 1);
        snprintf(b, sizeof(b), "Weeks: %d", g_camp.week);
        text_draw(FONT_BODY, 295, 101, b, COL_TEXT, 1);
        snprintf(b, sizeof(b), "Dead Heroes: %d", g_camp.deaths);
        text_draw(FONT_BODY, 295, 155, b, COL_TEXT, 1);
        text_draw_wrapped(FONT_BODY, 150, 192, 190, dd_game_over_reason[g_camp.game_over == 1 ? 0 : 1], COL_TEXT);
        ui_hints(BG_CROSS " Title");
        return;
    }
    if (st == TS_EPILOG) {
        gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 255));
        text_draw(FONT_TITLE, SCREEN_W / 2, 40, "The Darkest Dungeon", COL_GOLD, 1);
        for (int k = 3; k >= 0; k--) { /* the current line, with the three before it fading upward */
            int i = epi_line - k;
            if (i < 0 || i >= dd_epilog_count) continue;
            float a = k ? 0.45f - k * 0.1f : (epi_t < 0.6f ? epi_t / 0.6f : 1);
            text_draw(FONT_BODY, SCREEN_W / 2, 150 - k * 22, dd_epilog[i], RGBA(220, 210, 190, (int)(255 * a)), 1);
        }
        ui_hints(BG_CROSS " Continue");
        return;
    }
    if (st == TS_BUILDING) building_draw();
    else if (st >= TS_EMBARK_QUEST) embark_draw();
    else hamlet_draw();
    if (st == TS_ROSTER) {
        estate_hints(BG_UPDOWN " Hero  " BG_CROSS " Details  " BG_CIRCLE " Back");
    } else if (st == TS_HERO) {
        hero_draw(&g_camp.roster[hero_sel]);
    } else if (st == TS_LOG) { /* Activity Log | Caretaker Goals */
        ui_panel(16, 36, SCREEN_W - 32, 196);
        text_draw(FONT_TITLE, 24, 40, dd_ui_str[US_ACTIVITY_LOG], COL_GOLD, 0);
        text_printf(FONT_BODY, 24, 62, COL_TEXT, "Week %d", g_camp.week);
        float y = 80;
        if (!g_camp.nlog) text_draw(FONT_SMALL, 24, y, "A quiet week in the hamlet.", COL_DIM, 0);
        for (int i = log_top; i < g_camp.nlog && y < 214; i++)
            y += text_draw_wrapped(FONT_SMALL, 24, y, 196, g_camp.log[i], COL_TEXT) + 4;
        if (log_top > 0) text_draw(FONT_SMALL, 216, 66, BG_UP, COL_DIM, 0);
        if (y >= 214 && g_camp.nlog > 1) text_draw(FONT_SMALL, 216, 222, BG_DOWN, COL_DIM, 0);
        gfx_rect(SCREEN_W / 2, 44, 1, 180, RGBA(90, 80, 70, 255));
        float gx = SCREEN_W / 2 + 8;
        text_draw(FONT_TITLE, gx, 40, dd_ui_str[US_CARETAKER_GOALS], COL_GOLD, 0);
        text_draw(FONT_BODY, gx, 62, dd_ui_str[US_QUEST_GOALS], COL_GOLD, 0);
        y = 80;
        int lines = 0;
#define GOAL(done, str) do { if (lines < 9) { \
        gfx_rect(gx, y + 2, 7, 7, RGBA(150, 140, 120, 255)); gfx_rect(gx + 1, y + 3, 5, 5, RGBA(20, 18, 16, 255)); \
        if (done) gfx_rect(gx + 2, y + 4, 3, 3, COL_GOLD); \
        y += text_draw_wrapped(FONT_SMALL, gx + 12, y, 196, str, done ? COL_DIM : COL_TEXT) + 3; lines++; } } while (0)
        if (g_camp.quests_done >= 1) GOAL(g_camp.plot_done & 2, dd_ui_str[US_GOAL_TUTORIAL]);
        for (int dg = 0; dg < DG_DARKEST; dg++) { /* the next boss level of each unlocked region's two bosses */
            if (!dungeon_unlocked(dg)) continue;
            for (int b = dg * 2; b < dg * 2 + 2; b++) {
                int lv = 0;
                while (lv < 2 && (g_camp.boss_done >> (b * 3 + lv) & 1)) lv++;
                GOAL(g_camp.boss_done >> (b * 3 + lv) & 1, dd_ui_str[US_GOAL_BOSS + b * 3 + lv]);
            }
        }
        if (g_camp.dd_done < DD_PLOT_QUESTS) GOAL(0, dd_ui_str[US_GOAL_DD1 + g_camp.dd_done]);
        for (int i = 0; i < g_camp.nboard; i++)
            if (g_camp.board[i].plot == PLOT_DD + PQ_INVASION) GOAL(0, dd_ui_str[US_GOAL_INVASION]);
#undef GOAL
        estate_hints(BG_CROSS " Close");
    } else if (st == TS_JOURNAL) {
        ui_panel(30, 20, SCREEN_W - 60, 210);
        if (phase) {
            text_draw(FONT_BODY, SCREEN_W / 2, 26, dd_journal_title[sel], COL_GOLD, 1);
            text_draw_wrapped(FONT_BODY, 42, 46, SCREEN_W - 84, dd_journal_text[sel], COL_TEXT);
            estate_hints(BG_CIRCLE " Back");
        } else {
            int found = 0;
            for (int i = 0; i < JOURNAL_PAGES; i++) found += g_camp.journal >> i & 1;
            text_printf(FONT_BODY, SCREEN_W / 2 - 50, 24, COL_GOLD, "Journal  %d/%d", found, JOURNAL_PAGES);
            int top = sel > 13 ? sel - 13 : 0;
            for (int i = top; i < JOURNAL_PAGES && i < top + 14; i++) {
                int have = g_camp.journal >> i & 1;
                text_printf(FONT_SMALL, 42, 42 + (i - top) * 13, i == sel ? COL_SELECT : have ? COL_TEXT : COL_DIM,
                            "%2d  %s", i, have ? dd_journal_title[i] : "???");
            }
            estate_hints(BG_CROSS " Read  " BG_CIRCLE " Back");
        }
    } else if (st == TS_EXCHANGE) {
        float px = SCREEN_W / 2 - 110, py = 28, ph = 24 + dd_heir_exchange_count * 13;
        ui_panel(px, py, 220, ph);
        text_draw(FONT_BODY, SCREEN_W / 2, py + 4, "Heirloom Exchange", COL_GOLD, 1);
        for (int i = 0; i < dd_heir_exchange_count; i++) {
            const DDHeirExchange *x = &dd_heir_exchange[i];
            int ok = g_camp.heirlooms[x->from] >= x->from_n;
            text_printf(FONT_SMALL, px + 12, py + 20 + i * 13, i == sel ? COL_SELECT : ok ? COL_TEXT : COL_DIM,
                        "%d %s  ->  %d %s", x->from_n, heir_names[x->from], x->to_n, heir_names[x->to]);
        }
        estate_hints(BG_CROSS " Exchange  " BG_CIRCLE " Back");
    } else if (st == TS_QUIT) {
        static const char *const items[] = {"Stay in the hamlet", "Save and quit to title"};
        ui_panel(SCREEN_W / 2 - 90, 110, 180, 44);
        ui_menu(SCREEN_W / 2 - 86, 114, 172, items, 2, sel);
    }
    if (st == TS_HAMLET && town_event() && !g_camp.event_seen) {
        const DDTownEvent *e = town_event();
        ui_panel(SCREEN_W / 2 - 150, 60, 300, 110);
        text_draw(FONT_SMALL, SCREEN_W / 2, 66, "Town Event", COL_DIM, 1);
        text_draw(FONT_TITLE, SCREEN_W / 2, 78, e->title, COL_GOLD, 1);
        text_draw_wrapped(FONT_BODY, SCREEN_W / 2 - 140, 104, 280, e->desc, COL_TEXT);
        text_draw(FONT_SMALL, SCREEN_W / 2, 158, BG_CROSS " Continue", COL_DIM, 1);
    }
    if (note_t > 0) {
        float w = text_width(FONT_BODY, note) + 16;
        ui_panel(SCREEN_W / 2 - w / 2, 236, w, 18);
        text_draw(FONT_BODY, SCREEN_W / 2, 239, note, COL_TEXT, 1);
    }
}
