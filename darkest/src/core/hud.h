/*
 * hud.h - the in-dungeon heads-up display. Used by BOTH the dungeon and battle
 * scenes so they look the same. Screen split: top HUD_Y pixels = the scene;
 * bottom HUD_H (90) pixels = side decor, hero panel (left), map/inventory
 * (right). Also exports shared widgets: torch meter, HP/stress bars under
 * actors, selection brackets, status icons.
 * Raid HUD shared by the corridor, room and battle screens, laid out like the original:
 * the scene fills the top 2/3, the bottom third holds the hero panel (portrait, skills,
 * HP/stress, stats, equipment) on the left and the dungeon map on the right. */
#ifndef DD_HUD_H
#define DD_HUD_H
#include <stdint.h>
#include "assets.h"
#include "battle.h"
#include "dungeon.h"
#include "loot.h"

#define HUD_H 90
#define HUD_Y (SCREEN_H - HUD_H)

/* Hero panel contents. Slots 0-3 skills, 4 move, 5 pass. */
/* A snapshot of what the hero panel shows. hud_hero() fills it either from the
 * roster Hero (out of battle) or from the live Actor (so buffs show in stats). */
typedef struct {
    const Hero *hero;
    int hp, max_hp;
    float acc, crit, dodge, prot;
    int dmg_lo, dmg_hi, spd;
    uint8_t usable; /* bit per slot */
    int sel;        /* highlighted slot, -1 none */
} HudHero;

/* Fills v from a hero outside battle, or from its battle actor (buffs included) if a != NULL. */
void hud_hero(HudHero *v, const Hero *h, const Actor *a);

/* Bottom HUD. party_frac: party position along the current corridor (0 = room a, 1 = room b). */
/* Right panel tab: >= 0 shows the inventory (panel_inventory) with that slot selected, -1 the map. */
void hud_show_inventory(int sel);
void hud_draw(const HudHero *v, const Dungeon *d, float party_frac);
/* Map of known rooms and halls inside a rectangle, centred on (cx, cy) grid units;
 * cursor: room to frame, -1 none. */
void hud_map(const Dungeon *d, float x, float y, float w, float h, float cx, float cy, int cursor, float party_frac);
/* Grid position of the party (rooms are at integer cells). */
void hud_party_pos(const Dungeon *d, float party_frac, float *gx, float *gy);

void hud_torch(int torch);
/* HP bar and stress pips centred under an actor; stress < 0 hides the pips. */
void hud_bars(float cx, float y, int hp, int max_hp, int stress, int deaths_door);

/* Corner brackets drawn around actors: selected performer / enemy target / ally target. */
enum { BRACKET_SELECTED, BRACKET_TARGET, BRACKET_TARGET_ALLY };
void hud_bracket(int kind, float cx, float y, uint8_t alpha);

/* Button hints along the bottom of the scene, above the HUD. */
void hud_hints(const char *text);
/* Inventory icon for a loot stack (LK_GOLD / LK_GEM / LK_HEIRLOOM / LK_ITEM / LK_TRINKET); NULL if missing. */
Image *hud_item_icon(int kind, int ref, int amount);
/* Loot window over the scene: item cards with amounts, the selected one named. */
void hud_loot(const char *title, const Loot *l, int n, int sel);
/* Weapon (kind 0) / armour (1) icon of a class at an upgrade level. */
Image *hud_hero_equip(int cls, int kind, int lvl);
void hud_release(void);
/* Status tray icons (overlays/tray_*.png) under battle actors. */
enum { TRAY_STUN, TRAY_TAG, TRAY_BLEED, TRAY_POISON, TRAY_HORROR, TRAY_GUARD, TRAY_RIPOSTE, TRAY_STEALTH,
       TRAY_BUFF, TRAY_DEBUFF, TRAY_DEATHSDOOR, TRAY_DD_EFFECTS, TRAY_DISEASE, TRAY_TOWN_EVENT, TRAY_COUNT };
Image *hud_tray(int t);
#endif
