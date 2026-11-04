/*
 * dungeon.h - the data model of one expedition and its map.
 *
 * MAP MODEL: a graph.  ROOMS are nodes placed on a small grid; CORRIDORS
 * (halls) are edges.  Each hall is split into 1..10 SEGMENTS (squares), and
 * every square or room can hold something (a battle, curio, trap, hunger
 * check, obstacle...).  The party is always either in a room or at a position
 * along a corridor.
 *
 * Dungeon is the *whole* expedition state (map + torch + inventory + loot),
 * and is saved to disk, so changing it means bumping SAVE_VERSION.
 *
 * The layout generator is the demake's own design (the original's map rules live in code
 * that isn't reproduced); encounters come from the game's own .mash tables. */
#ifndef DD_DUNGEON_H
#define DD_DUNGEON_H

#include <stdint.h>
#include "rng.h"

/* Fixed capacities (the struct is saved raw, so no dynamic memory). */
#define DG_MAX_ROOMS 32      /* the Darkest Dungeon's third map has 31 */
#define DG_MAX_CORRIDORS 48  /* ... and 43 halls */
#define DG_TRINKETS 16
#define DG_MAX_SEGMENTS 10   /* generated halls use 1-3 squares; the fixed maps up to 10 */
#define DG_GRID_W 8
#define DG_GRID_H 6

/* What a room or hall square contains. */
typedef enum { SPOT_EMPTY, SPOT_BATTLE, SPOT_CURIO, SPOT_TRAP, SPOT_ENTRANCE, SPOT_BOSS, SPOT_OBSTACLE, SPOT_HUNGER,
               SPOT_GHOST /* the Ancestor's apparition in the final hall: speaks, then fades */ } SpotContent;
/* same values as QT_* in dd_types.h */
typedef enum { QUEST_EXPLORE, QUEST_CLEANSE, QUEST_BOSS } QuestType;
/* Heirlooms (estate currency for building upgrades). */
typedef enum { HEIR_BUST, HEIR_PORTRAIT, HEIR_DEED, HEIR_CREST, HEIR_COUNT } HeirType;
extern const char *const heir_names[HEIR_COUNT];
typedef enum { LEN_SHORT = 1, LEN_MEDIUM = 2, LEN_LONG = 3 } QuestLength;
/* Dungeon.plot: 0 generated quest, PLOT_OLD_ROAD, or PLOT_DD + n for plot quest n (dd_plot_quests: 0..3 the
 * Darkest Dungeon, PQ_INVASION, PQ_CROW). */
enum { PLOT_NONE, PLOT_OLD_ROAD, PLOT_DD };
#define DUNGEON_DD_QUEST(d) ((d)->plot >= PLOT_DD ? (d)->plot - PLOT_DD : -1)

/* A room (graph node). gx,gy = grid cell. `content` is its main feature, and
 * `curio` may be an extra guarded object. */
typedef struct {
    int8_t gx, gy;
    uint8_t content;   /* SpotContent */
    uint8_t visited;
    uint8_t scouted;   /* revealed on the map by a curio */
    uint8_t done;      /* battle won / curio used */
    uint8_t wall;      /* which room_wall background */
    int16_t encounter; /* dd_encounters index (content SPOT_BATTLE / SPOT_BOSS), -1 none */
    int16_t curio;     /* dd_curios index, -1 none (a guarded curio has both) */
    uint8_t curio_done;
} Room;

/* A hall (graph edge) joining rooms a and b, made of `nseg` squares. Per-square
 * arrays: seg = content, seg_done = bit0 resolved / bit1 walked, seg_enc =
 * encounter-or-curio index, wall = which background art variant. */
typedef struct {
    uint8_t a, b;      /* room indices */
    uint8_t nseg;
    uint8_t seg[DG_MAX_SEGMENTS];
    uint8_t seg_done[DG_MAX_SEGMENTS];
    int16_t seg_enc[DG_MAX_SEGMENTS];  /* encounter or curio index, like Room.encounter */
    uint8_t wall[DG_MAX_SEGMENTS];
    uint8_t explored;
    uint8_t scouted;   /* contents known (scouting / map curio): traps show on the map and floor */
} Corridor;

/* Provisions (bought at embark, used in the dungeon and on curios). */
typedef enum {
    ITEM_FOOD, ITEM_TORCH, ITEM_SHOVEL, ITEM_BANDAGE, ITEM_ANTIVENOM, ITEM_HERBS, ITEM_HOLY_WATER,
    ITEM_KEY, ITEM_LAUDANUM, ITEM_FIREWOOD, ITEM_COUNT
} ItemId;

/* The full expedition state. dungeon/level/length/quest describe the contract.
 * room/corridor/pos say where the party stands. Everything else is progress
 * (torch, items carried, loot found, which parts were scouted/visited). */
typedef struct {
    uint8_t dungeon, level, length, quest;
    Room rooms[DG_MAX_ROOMS];
    int nrooms;
    Corridor corr[DG_MAX_CORRIDORS];
    int ncorr;
    int room;          /* current room, or -1 while in a corridor */
    int corridor;      /* current corridor, -1 in a room */
    int pos;           /* position along the corridor: 0 = room a side, nseg+1 = room b side */
    int torch;         /* 0..100 */
    int torches;       /* (unused, see items[ITEM_TORCH]) */
    int gold;          /* collected this expedition */
    float xpos;        /* party position along the current corridor, in pixels */
    uint8_t items[ITEM_COUNT];
    uint16_t steps;    /* corridor squares walked (hunger checks) */
    uint8_t camped;    /* firewood used this expedition */
    uint8_t heir;      /* heirloom reward type (HEIR_*) */
    uint16_t heirlooms[HEIR_COUNT]; /* found this expedition */
    uint8_t plot;      /* PLOT_*: the Old Road or a Darkest Dungeon expedition */
    uint8_t cell;      /* map units per minimap cell: 1 generated grid, 6 fixed plot maps (tiles) */
    uint8_t goals_done;/* goal curios activated (Darkest Dungeon 2 and 3) */
    uint8_t ntrinkets;
    int16_t trinkets[DG_TRINKETS]; /* found this expedition (dd_trinkets index) */
    int8_t town_event; /* town event active at embark (resolve XP bonus), -1 none */
    uint32_t journal;  /* journal pages known (bit = page): campaign pages at embark + found here */
    uint8_t cond_met;
    uint8_t intro_done;/* the loading card (title, art, tip) was shown */
    uint8_t goal_shown;/* "Quest Complete!" was offered (return to town / keep exploring) */  /* conditional hall packs already met (bit = kind - ENC_COND_DARK); limit 1 */
} Dungeon;

/* Item names and store prices (original provision store). */
extern const char *const item_names[ITEM_COUNT];
extern const int item_prices[ITEM_COUNT];

/* Build a random map for a quest contract (see dungeon.c for the algorithm). */
void dungeon_generate(Dungeon *d, Rng *r, int dungeon, int level, int length, int quest);
/* The fixed opening expedition (the Old Road). */
void dungeon_tutorial(Dungeon *d, Rng *r);
void dungeon_plot(Dungeon *d, Rng *r, int qn); /* Darkest Dungeon expedition qn (0..3) */
/* Corridors touching a room, written to out[] (max 4); returns count. */
int dungeon_room_exits(const Dungeon *d, int room, int out[4]);
/* Scouting roll on entering a room (rules.json scouting_chance_base + light + party bonus):
 * reveals the neighbouring rooms and their corridors; a crit (scouting_crit_success) one ring further.
 * Returns 0 nothing, 1 scouted, 2 crit. */
int dungeon_scout(Dungeon *d, Rng *r, float bonus);
int dungeon_other_end(const Corridor *c, int room);
int dungeon_goal_met(const Dungeon *d);
/* A curio that can appear in this dungeon (dd_curios index). */
int dungeon_pick_curio(Rng *r, int dungeon);
/* The raid inventory as stacks (items, gold, heirlooms, trinkets) for the inventory panel. */
struct Loot;
int dungeon_inventory_slots(const Dungeon *d, struct Loot *out, int max);
/* Share of the 16 raid inventory slots in use (base stack limits). */
float dungeon_inventory_fill(const Dungeon *d);
/* Rolls the conditional hall packs (Shambler in darkness, Collector with a full bag) when a hall
 * battle starts; returns the encounter that replaces it (marked as met) or -1. */
int dungeon_roll_conditional(Rng *r, Dungeon *d);
/* Picks an encounter of the given kind (ENC_HALL / ENC_ROOM) for this dungeon and level. */
int dungeon_pick_encounter(Rng *r, int dungeon, int level, int kind);
/* Percent of rooms visited, and room battles left. */
int dungeon_explored_percent(const Dungeon *d);
int dungeon_return_content(Dungeon *d, Corridor *c, Rng *r);
int dungeon_battles_left(const Dungeon *d);
/* Room holding the boss (QUEST_BOSS), or -1. */
int dungeon_boss_room(const Dungeon *d);
/* Quest reward in gold for this dungeon level/length. */
int dungeon_reward(const Dungeon *d);
/* Heirlooms of type d->heir paid on success. */
int dungeon_heirloom_reward(const Dungeon *d);
/* Rooms reachable from `from` through visited rooms (for quick travel on the map). */
int dungeon_reachable(const Dungeon *d, int from, int to);

#endif
