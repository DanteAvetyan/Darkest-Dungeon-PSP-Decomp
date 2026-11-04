/*
 * hud.c - drawing the raid HUD from pre-rendered art (assets/hud/*.ddi).
 * Layout numbers copy the original game's layout files scaled to 480 wide.
 * Images are loaded lazily through get() and cached; hud_release() frees
 * them when a scene changes (RAM is tight on PSP).
 * Raid HUD (see hud.h). Art comes from tools/export_hud.py at 1/4 of the original 1080p UI. */
#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "campaign.h"
#include "formulas.h"
#include "quirk.h"
#include "trinket.h"
#include "stress.h"
#include "ui.h"
#include "platform.h"

/* the side decor is cropped to its middle so the banner and the map panel get wider than the
 * original's 1/4-scale 180 px: long hero names fit and the map shows more */
#define SIDE_W 36
#define SIDE_ART_W 60
#define PANEL_X SIDE_W              /* banner + hero panel */
#define PANEL_W 204
#define PANEL_EXTRA (PANEL_W - 180)
#define MAP_X (SIDE_W + PANEL_W)
#define MAP_W (SCREEN_W - 2 * SIDE_W - PANEL_W)
#define BANNER_H 34
#define CELL_PX 40.0f               /* map grid cell: room icon + 3 hall squares */
#define ROOM_PX 16.0f
#define HALL_PX 8.0f
#define WHITE RGBA(255, 255, 255, 255)

enum {
    A_BANNER, A_HERO, A_MAP, A_SIDE_L, A_SIDE_R, A_TORCH, A_SELECTED, A_TARGET, A_TARGET_H,
    A_MOVE, A_PASS, A_SKILL_SEL, A_FLAME,
    A_ROOM_UNKNOWN, A_ROOM_EMPTY, A_ROOM_BATTLE, A_ROOM_CURIO, A_ROOM_BOSS, A_ROOM_ENTRANCE,
    A_ROOM_TREASURE, A_HALL_CLEAR, A_HALL_DARK, A_HALL_DIM, A_INDICATOR,
    A_MK_BATTLE, A_MK_CURIO, A_MK_TRAP, A_MK_OBSTACLE, A_MK_HUNGER, A_INVENTORY, A_PARTY_ORDER, A_TRAY, A_COUNT = A_TRAY + TRAY_COUNT
};
static const char *const art_path[A_COUNT] = {
    "banner", "hero", "map", "side_l", "side_r", "torch", "selected", "target", "target_h",
    "move", "pass", "skill_sel", "flame",
    "map/room_unknown", "map/room_empty", "map/room_battle", "map/room_curio", "map/room_boss",
    "map/room_entrance", "map/room_treasure", "map/hall_clear", "map/hall_dark", "map/hall_dim",
    "map/indicator", "map/marker_battle", "map/marker_curio", "map/marker_trap",
    "map/marker_obstacle", "map/marker_hunger", "inventory", "party_order",
    "tray/stun", "tray/tag", "tray/bleed", "tray/poison", "tray/dot_stress", "tray/guard", "tray/riposte",
    "tray/stealth", "tray/buff", "tray/debuff", "tray/deathsdoor", "tray/deathsdoor_effects", "tray/disease",
    "tray/town_event",
};
static Image art[A_COUNT];
static uint8_t tried[A_COUNT];

/* Lazy-load HUD art piece `id` (an A_* value) from art_path[]. */
static Image *get(int id) {
    if (!tried[id]) {
        char path[64];
        snprintf(path, sizeof(path), "assets/hud/%s.ddi", art_path[id]);
        image_load(&art[id], path);
        tried[id] = 1;
    }
    return art[id].tiles ? &art[id] : NULL;
}

Image *hud_tray(int t) { return t >= 0 && t < TRAY_COUNT ? get(A_TRAY + t) : NULL; }

/* Draw art piece id with its top-left at (x,y). Returns 1 if it was drawn. */
static int put(int id, float x, float y, uint32_t col) {
    Image *im = get(id);
    if (im) image_draw(im, x, y, im->w, im->h, col);
    return im != NULL;
}

/* Same, centred on (x, y). */
static int put_c(int id, float x, float y, uint32_t col) {
    Image *im = get(id);
    if (im) image_draw(im, x - im->w / 2.0f, y - im->h / 2.0f, im->w, im->h, col);
    return im != NULL;
}

/* Equipment icons for the hero panel. Only one class's set is kept loaded.
 * weapon/armour icons, one class at a time (the party rarely changes mid-raid) */
static Image equip[2][5];
static int equip_cls = -1;

static Image *equip_icon(int cls, int kind, int lvl) {
    if (cls != equip_cls) {
        for (int k = 0; k < 2; k++)
            for (int l = 0; l < 5; l++) image_free(&equip[k][l]);
        equip_cls = cls;
    }
    Image *im = &equip[kind][lvl];
    if (!im->tiles) {
        char path[96];
        snprintf(path, sizeof(path), "assets/hud/equip/%s_%s_%d.ddi", dd_heroes[cls].id, kind ? "armour" : "weapon", lvl);
        image_load(im, path);
    }
    return im->tiles ? im : NULL;
}

/* item icons: a small cache keyed by name */
#define ICON_CACHE 24
static struct { char name[40]; Image img; int ok; } icons[ICON_CACHE];
static int icon_next;

Image *hud_item_icon(int kind, int ref, int amount) {
    static const char *const item_icon[ITEM_COUNT] = {"provision_", "supply_torch", "supply_shovel", "supply_bandage",
        "supply_antivenom", "supply_medicinal_herbs", "supply_holy_water", "supply_skeleton_key", "supply_laudanum",
        "supply_firewood"};
    static const char *const heir_icon[HEIR_COUNT] = {"bust", "portrait", "deed", "crest"};
    char name[40];
    switch (kind) {
    case LK_GOLD: snprintf(name, sizeof(name), "gold_%d", amount < 250 ? 0 : amount < 750 ? 1 : amount < 1500 ? 2 : 3); break;
    case LK_GEM: snprintf(name, sizeof(name), "gem_%s", dd_gems[ref].id); break;
    case LK_HEIRLOOM: snprintf(name, sizeof(name), "heirloom_%s", heir_icon[ref % HEIR_COUNT]); break;
    case LK_ITEM:
        if (ref < 0 || ref >= ITEM_COUNT) return NULL;
        if (ref == ITEM_FOOD) snprintf(name, sizeof(name), "provision_%d", amount < 2 ? 0 : amount < 4 ? 1 : amount < 8 ? 2 : 3);
        else snprintf(name, sizeof(name), "%s", item_icon[ref]);
        break;
    case LK_TRINKET: snprintf(name, sizeof(name), "trinket_%s", dd_trinkets[ref].id); break;
    default: return NULL;
    }
    for (int i = 0; i < ICON_CACHE; i++)
        if (strcmp(icons[i].name, name) == 0) return icons[i].ok ? &icons[i].img : NULL;
    int k = icon_next;
    icon_next = (icon_next + 1) % ICON_CACHE;
    image_free(&icons[k].img);
    snprintf(icons[k].name, sizeof(icons[k].name), "%s", name);
    char path[72];
    snprintf(path, sizeof(path), "assets/hud/items/%s.ddi", name);
    icons[k].ok = image_load(&icons[k].img, path) == 0 && icons[k].img.tiles;
    return icons[k].ok ? &icons[k].img : NULL;
}

/* The loot window: a row of item cards with amounts; the selected one is
 * named in the title bar. */
void hud_loot(const char *title, const Loot *l, int n, int sel) {
    float cw = 28, w = n * cw + 24;
    if (w < 200) w = 200;
    float x = SCREEN_W / 2 - w / 2, y = 44, h = 104;
    gfx_rect(x, y, w, h, RGBA(10, 8, 6, 235));
    ui_frame(x, y, w, h, COL_EDGE);
    ui_frame(x + 2, y + 2, w - 4, h - 4, RGBA(60, 48, 32, 255));
    text_draw(FONT_TITLE, SCREEN_W / 2, y + 3, title, COL_GOLD, 1);
    float ix = SCREEN_W / 2 - n * cw / 2;
    for (int i = 0; i < n; i++) {
        float cx = ix + i * cw;
        gfx_rect(cx + 1, y + 26, cw - 2, 48, RGBA(28, 22, 16, 255));
        Image *ic = hud_item_icon(l[i].kind, l[i].ref, l[i].amount);
        if (ic) image_draw(ic, cx + (cw - ic->w) / 2, y + 28, ic->w, ic->h, WHITE);
        if (l[i].kind != LK_GOLD && l[i].amount > 1) {
            char b[8];
            snprintf(b, sizeof(b), "%d", l[i].amount);
            text_draw(FONT_SMALL, cx + cw - 3, y + 63, b, COL_TEXT, 2);
        }
        if (i == sel) ui_frame(cx, y + 25, cw, 50, COL_SELECT);
    }
    if (n == 0) text_draw(FONT_BODY, SCREEN_W / 2, y + 44, "Nothing of value.", COL_DIM, 1);
    else if (sel >= 0 && sel < n) {
        char b[48];
        text_draw(FONT_BODY, SCREEN_W / 2, y + 80, loot_label(&l[sel], b, sizeof(b)), COL_TEXT, 1);
    }
}

Image *hud_hero_equip(int cls, int kind, int lvl) { return equip_icon(cls, kind, lvl); }

/* Free all HUD images. */
void hud_release(void) {
    for (int i = 0; i < ICON_CACHE; i++) {
        image_free(&icons[i].img);
        icons[i].name[0] = 0;
    }
    for (int i = 0; i < A_COUNT; i++) image_free(&art[i]);
    memset(tried, 0, sizeof(tried));
    for (int k = 0; k < 2; k++)
        for (int l = 0; l < 5; l++) image_free(&equip[k][l]);
    equip_cls = -1;
}

/* ---------------------------------------------------------------- hero panel */

/* Gather the numbers for the hero panel. In battle, stats include live buffs
 * (read through actor_stat); outside battle we use base stats. */
void hud_hero(HudHero *v, const Hero *h, const Actor *a) {
    Actor tmp;
    if (!a) {
        memset(&tmp, 0, sizeof(tmp));
        tmp.is_hero = 1;
        tmp.cls = h->cls;
        tmp.hero = (Hero *)h;
        tmp.hp = h->hp;
        tmp.max_hp = (int16_t)hero_max_hp(h);
        quirks_to_actor(&tmp);
        trinkets_to_actor(&tmp);
        formulas_context(NULL, NULL, NULL); /* out of battle: situational quirks don't count */
        a = &tmp;
    }
    memset(v, 0, sizeof(*v));
    v->hero = h;
    v->hp = a->hp;
    v->max_hp = a->max_hp;
    v->acc = actor_stat(a, STAT_ATK);
    v->crit = actor_stat(a, STAT_CRIT);
    v->dodge = actor_stat(a, STAT_DEF);
    v->prot = actor_stat(a, STAT_PROT);
    v->dmg_lo = (int)(actor_stat(a, STAT_DMG_LO) + 0.5f);
    v->dmg_hi = (int)(actor_stat(a, STAT_DMG_HI) + 0.5f);
    v->spd = (int)actor_stat(a, STAT_SPD);
    v->usable = 0x3f;
    v->sel = -1;
}

/* Draw the hero panel: portrait, name, HP/stress bars, skill icons (greyed
 * if unusable), stats and equipment. */
static void draw_hero(const HudHero *v) {
    const Hero *h = v->hero;
    float bx = PANEL_X, by = HUD_Y, py = HUD_Y + BANNER_H;
    Image *ban = get(A_BANNER), *hp = get(A_HERO);
    if (ban) { /* portrait + name part, a stretched strip of the name plate, then the skill slots */
        image_draw_part(ban, 0, 66, bx, by, 66, ban->h, WHITE);
        image_draw_part(ban, 40, 26, bx + 66, by, PANEL_EXTRA, ban->h, WHITE);
        image_draw_part(ban, 66, ban->w - 66, bx + 66 + PANEL_EXTRA, by, ban->w - 66, ban->h, WHITE);
    }
    if (hp) {
        image_draw(hp, bx, py, hp->w, hp->h, WHITE);
        image_draw_part(hp, hp->w - 12, 10, bx + hp->w - 2, py, PANEL_EXTRA + 2, hp->h, WHITE);
    }
    if (!h) return;

    /* banner: portrait, name / class, skill slots 1-4 + move, pass beside it */
    Image *p = ui_portrait(h->cls);
    if (p) image_draw(p, bx + 4, by + 5, 24, 24, WHITE);
    text_draw(FONT_SMALL, bx + 30, by + 6, h->name, COL_GOLD, 0);
    text_draw(FONT_SMALL, bx + 30, by + 16, dd_heroes[h->cls].name, COL_DIM, 0);
    for (int s = 0; s < 5; s++) {
        float x = bx + 67 + PANEL_EXTRA + s * 19, y = by + 7;
        uint32_t tint = (v->usable >> s) & 1 ? WHITE : RGBA(70, 70, 70, 255);
        if (s < HERO_SKILL_SLOTS) {
            Image *ic = h->equipped[s] >= 0 ? ui_skill_icon(h->cls, h->equipped[s]) : NULL;
            if (ic) image_draw(ic, x, y, 18, 18, tint);
        } else {
            Image *m = get(A_MOVE);
            if (m) image_draw(m, x, y, 18, 18, tint);
        }
        if (s == v->sel) put_c(A_SKILL_SEL, x + 9, y + 9, WHITE);
    }
    {
        float x = bx + 67 + PANEL_EXTRA + 5 * 19 + 1, y = by + 7;
        Image *pass = get(A_PASS);
        if (pass) image_draw(pass, x, y, pass->w, 18, (v->usable >> 5) & 1 ? WHITE : RGBA(70, 70, 70, 255));
        if (v->sel == 5) ui_frame(x - 1, y - 1, (pass ? pass->w : 5) + 2, 20, COL_SELECT);
    }

    /* hero panel: HP / stress rows, stats, weapon + armour */
    text_printf(FONT_SMALL, bx + 18, py, v->hp <= 0 ? COL_RED : COL_TEXT, "%d/%d", v->hp, v->max_hp);
    text_printf(FONT_SMALL, bx + 18, py + 9, COL_TEXT, "%d/200", h->stress);
    if (h->affliction) {
        int virtue = 0;
        const char *nm = stress_affliction_name(h, &virtue);
        text_draw(FONT_SMALL, bx + 64, py + 9, nm, virtue ? COL_GOLD : RGBA(210, 90, 210, 255), 2);
    }
    /* label left, value right-aligned, two columns that end before the equipment slots */
    static const char *const lab[3][2] = {{"ACC", "DODGE"}, {"CRIT", "PROT"}, {"DMG", "SPD"}};
    char val[3][2][12];
    snprintf(val[0][0], 12, "%d", (int)floorf(v->acc * 100 + 0.5f));
    snprintf(val[0][1], 12, "%d", (int)floorf(v->dodge * 100 + 0.5f));
    snprintf(val[1][0], 12, "%d%%", (int)floorf(v->crit * 100 + 0.5f));
    snprintf(val[1][1], 12, "%d", (int)(v->prot * 100 + 0.5f));
    snprintf(val[2][0], 12, "%d-%d", v->dmg_lo, v->dmg_hi);
    snprintf(val[2][1], 12, "%d", v->spd);
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 2; c++) {
            float x = bx + 2 + c * 33, y = py + 21 + r * 10;
            text_draw(FONT_SMALL, x, y, lab[r][c], COL_DIM, 0);
            text_draw(FONT_SMALL, x + 31, y, val[r][c], COL_TEXT, 2);
        }
    Image *w = equip_icon(h->cls, 0, h->weapon_lvl), *ar = equip_icon(h->cls, 1, h->armour_lvl);
    if (w) image_draw(w, bx + 67, py + 13, 18, 36, WHITE);
    if (ar) image_draw(ar, bx + 90, py + 13, 18, 36, WHITE);
}

/* ---------------------------------------------------------------- map */

/* A room shows on the map once visited, scouted, or adjacent to a visited room. */
static int room_known(const Dungeon *d, int i) {
    const Room *r = &d->rooms[i];
    if (r->visited || r->scouted || i == d->room) return 1;
    for (int k = 0; k < d->ncorr; k++) {
        const Corridor *c = &d->corr[k];
        if ((c->a == i && d->rooms[c->b].visited) || (c->b == i && d->rooms[c->a].visited)) return 1;
    }
    return 0;
}

/* Convert corridor progress (0..1) into a fractional grid position so the
 * party marker slides smoothly between two rooms. */
void hud_party_pos(const Dungeon *d, float frac, float *gx, float *gy) {
    if (d->corridor >= 0) {
        const Corridor *c = &d->corr[d->corridor];
        const Room *a = &d->rooms[c->a], *b = &d->rooms[c->b];
        *gx = a->gx + (b->gx - a->gx) * frac;
        *gy = a->gy + (b->gy - a->gy) * frac;
    } else if (d->room >= 0) {
        *gx = d->rooms[d->room].gx;
        *gy = d->rooms[d->room].gy;
    } else {
        *gx = *gy = 0;
    }
}

/* Is a circle (centre, radius r) fully inside a rect? (map clipping) */
static int inside(float x, float y, float r, float x0, float y0, float w, float h) {
    return x - r >= x0 && y - r >= y0 && x + r <= x0 + w && y + r <= y0 + h;
}

/* The mini-map: draws known halls as lines and rooms as icons, scrolled so
 * that grid point (cx,cy) is the centre; marks the party and a cursor. */
void hud_map(const Dungeon *d, float x0, float y0, float w, float h, float cx, float cy, int cursor, float frac) {
    const float cs = CELL_PX / (d->cell ? d->cell : 1); /* fixed plot maps are in tile units */
    float ox = x0 + w / 2 - cx * cs, oy = y0 + h / 2 - cy * cs;
    /* halls: squares spread evenly between the two rooms */
    for (int i = 0; i < d->ncorr; i++) {
        const Corridor *c = &d->corr[i];
        const Room *a = &d->rooms[c->a], *b = &d->rooms[c->b];
        int here = d->corridor == i;
        if (!a->visited && !b->visited && !a->scouted && !b->scouted && !here) continue;
        int seen = c->explored || a->scouted || b->scouted || here;
        for (int s = 0; s < c->nseg; s++) {
            float f = (s + 1.0f) / (c->nseg + 1);
            float x = ox + (a->gx + (b->gx - a->gx) * f) * cs, y = oy + (a->gy + (b->gy - a->gy) * f) * cs;
            if (!inside(x, y, HALL_PX / 2, x0, y0, w, h)) continue;
            int walked = c->seg_done[s] & 2;
            put_c(walked ? A_HALL_CLEAR : seen ? A_HALL_DIM : A_HALL_DARK, x, y, WHITE);
            if (!seen || (c->seg_done[s] & 1)) continue;
            int mk = c->seg[s] == SPOT_BATTLE ? A_MK_BATTLE : c->seg[s] == SPOT_CURIO ? A_MK_CURIO
                   : c->seg[s] == SPOT_TRAP && c->scouted ? A_MK_TRAP : c->seg[s] == SPOT_OBSTACLE ? A_MK_OBSTACLE
                   : c->seg[s] == SPOT_HUNGER && walked ? A_MK_HUNGER : c->seg[s] == SPOT_GHOST ? A_MK_OBSTACLE : -1;
            if (mk >= 0) put_c(mk, x, y, WHITE);
            if (mk == A_MK_TRAP) { /* a pulsing purple ring: the dark marker is hard to see on the PSP */
                int a = 140 + (int)(115 * sinf(plat_ticks_ms() * 0.006f));
                ui_frame(x - 5, y - 5, 10, 10, RGBA(200, 110, 255, a));
            }
        }
    }
    for (int i = 0; i < d->nrooms; i++) {
        const Room *r = &d->rooms[i];
        float x = ox + r->gx * cs, y = oy + r->gy * cs;
        if (!inside(x, y, ROOM_PX / 2, x0, y0, w, h)) continue;
        int boss = r->content == SPOT_BOSS && !r->done;
        if (!room_known(d, i) && !boss) continue;
        int seen = r->visited || r->scouted;
        int id = boss ? A_ROOM_BOSS
               : !seen ? A_ROOM_UNKNOWN
               : r->content == SPOT_ENTRANCE ? A_ROOM_ENTRANCE
               : r->encounter >= 0 && !r->done ? A_ROOM_BATTLE
               : r->curio != -1 && !r->curio_done ? A_ROOM_CURIO : A_ROOM_EMPTY;
        put_c(id, x, y, WHITE);
        if (i == cursor) ui_frame(x - 10, y - 10, 20, 20, COL_SELECT);
    }
    float px, py;
    hud_party_pos(d, frac, &px, &py);
    px = ox + px * cs;
    py = oy + py * cs;
    if (inside(px, py, 4, x0, y0, w, h) && !put_c(A_INDICATOR, px, py, WHITE)) gfx_rect(px - 3, py - 3, 6, 6, COL_GOLD);
}

/* ---------------------------------------------------------------- frame, torch, bars */

static int inv_panel = -1; /* inventory tab: selected slot, -1 = map tab */
void hud_show_inventory(int sel) { inv_panel = sel; }

/* The alternative right panel: the 16-slot backpack.
 * panel_inventory: 8 x 2 slots of the 16-slot raid inventory, the selected one framed */
static void draw_inventory_panel(const Dungeon *d) {
    Image *bg = get(A_INVENTORY);
    if (bg) image_draw(bg, MAP_X, HUD_Y, MAP_W, bg->h, WHITE);
    if (!d) return;
    Loot slots[16];
    int n = dungeon_inventory_slots(d, slots, 16);
    float cw = (MAP_W - 18) / 8.0f, ch = 41;
    for (int i = 0; i < 16; i++) {
        float x = MAP_X + 3 + (i % 8) * cw, y = HUD_Y + 4 + (i / 8) * ch;
        if (i < n) {
            Image *ic = hud_item_icon(slots[i].kind, slots[i].ref, slots[i].amount);
            if (ic) image_draw(ic, x + 1, y + 1, cw - 2, ch - 2, WHITE);
            if (slots[i].kind != LK_TRINKET) text_printf(FONT_SMALL, x + 2, y, COL_TEXT, "%d", slots[i].amount);
        }
        if (i == inv_panel) ui_frame(x, y, cw, ch, COL_SELECT);
    }
}

/* Compose the whole bottom HUD (decor + hero panel + map or inventory + torch). */
void hud_draw(const HudHero *v, const Dungeon *d, float frac) {
    gfx_rect(0, HUD_Y, SCREEN_W, HUD_H, RGBA(0, 0, 0, 255));
    Image *sl = get(A_SIDE_L), *sr = get(A_SIDE_R);
    float crop = (SIDE_ART_W - SIDE_W) / 2.0f; /* the middle of the decor, skeleton included */
    if (sl) image_draw_part(sl, crop, SIDE_W, 0, HUD_Y, SIDE_W, sl->h, WHITE);
    if (sr) image_draw_part(sr, crop, SIDE_W, SCREEN_W - SIDE_W, HUD_Y, SIDE_W, sr->h, WHITE);
    draw_hero(v);
    if (inv_panel >= 0) draw_inventory_panel(d);
    else {
        Image *m = get(A_MAP);
        if (m) image_draw(m, MAP_X, HUD_Y, MAP_W, m->h, WHITE);
        if (d) {
            float gx, gy;
            hud_party_pos(d, frac, &gx, &gy);
            hud_map(d, MAP_X + 5, HUD_Y + 5, MAP_W - 18, 80, gx, gy, -1, frac);
        }
    }
    put(A_PARTY_ORDER, MAP_X + MAP_W - 16, HUD_Y + 3, WHITE); /* party_order_button: Up/Down swaps heroes */
}

/* Torch meter: flame icon plus a bar filled to `torch`%. */
void hud_torch(int torch) {
    Image *t = get(A_TORCH);
    float w = t ? t->w : 214, x = SCREEN_W / 2 - w / 2, bar_y = 21;
    /* light spreads out from the torch along the bar as it burns brighter */
    float lit = (w / 2 - 6) * torch / 100.0f;
    gfx_rect(SCREEN_W / 2 - lit, bar_y - 1, lit * 2, 3, RGBA(255, 200, 110, 230));
    if (t) image_draw(t, x, 0, t->w, t->h, WHITE);
    Image *f = get(A_FLAME);
    if (torch > 0 && f) {
        float k = 0.5f + torch / 200.0f, fw = f->w * k, fh = f->h * k;
        image_draw(f, SCREEN_W / 2 - fw / 2, 16 - fh, fw, fh, RGBA(255, 255, 255, 140 + torch));
    }
}

/* Small HP bar with stress pips beneath it, centred under an actor. A
 * death's-door hero shows a different bar. */
void hud_bars(float cx, float y, int hp, int max_hp, int stress, int deaths_door) {
    float frac = max_hp > 0 ? (float)(hp < 0 ? 0 : hp) / max_hp : 0;
    ui_bar(cx - 19, y, 38, 3, frac, deaths_door ? RGBA(110, 0, 0, 255) : RGBA(190, 20, 20, 255), RGBA(30, 8, 8, 230));
    if (stress < 0) return;
    /* ten pips of 10 stress; past 100 they fill again in red */
    int over = stress > 100, n = (over ? stress - 100 : stress) / 10;
    for (int k = 0; k < 10; k++) {
        uint32_t col = k < n ? (over ? RGBA(230, 60, 40, 255) : RGBA(235, 235, 235, 255))
                             : over ? RGBA(235, 235, 235, 255) : RGBA(50, 50, 50, 230);
        gfx_rect(cx - 19 + k * 4, y + 5, 3, 3, col);
    }
}

/* Selection frame around an actor. */
void hud_bracket(int kind, float cx, float y, uint8_t alpha) {
    int id = kind == BRACKET_SELECTED ? A_SELECTED : kind == BRACKET_TARGET ? A_TARGET : A_TARGET_H;
    Image *im = get(id);
    uint32_t col = RGBA(255, 255, 255, alpha);
    if (im) image_draw(im, cx - im->w / 2.0f, y - im->h, im->w, im->h, col);
    else gfx_rect(cx - 20, y - 3, 40, 3, kind == BRACKET_SELECTED ? COL_GOLD : COL_RED);
}

/* Control hints above the HUD. */
void hud_hints(const char *text) {
    int w = text_width(FONT_SMALL, text) + 10;
    gfx_rect(SCREEN_W - w, HUD_Y - 11, w, 11, RGBA(0, 0, 0, 150));
    text_draw(FONT_SMALL, SCREEN_W - 5, HUD_Y - 11, text, COL_DIM, 2);
}
