/*
 * campaign.h - everything that persists between play sessions.
 *
 * The Campaign struct is the SAVE FILE: it is written to save.bin as a raw
 * memory dump (see campaign_save) and read back the same way. Consequences:
 *   - It must contain no pointers (all fixed-size arrays and plain numbers).
 *   - If any struct inside it (Campaign, Hero, Dungeon, QuestOffer) changes
 *     layout, bump SAVE_VERSION in campaign.c so old saves are rejected
 *     instead of misread.
 * One global instance, g_camp, is used by the whole game.
 *
 * The town loop in a nutshell: each WEEK you hire heroes from the stage coach,
 * pick a quest from the board, send a party of 4 into a dungeon, come back,
 * spend gold/heirlooms on buildings, relieve stress, and call
 * campaign_next_week().
 */
#ifndef DD_CAMPAIGN_H
#define DD_CAMPAIGN_H

#include "battle.h"
#include "dungeon.h"

#define ROSTER_MAX 28     /* stage_coach roster_size_upgrades top */
#define RECRUITS 7          /* stage_coach number_of_recruits_upgrades top */
#define TREES_MAX 24
#define BOARD_MAX 16
#define LOG_MAX 16
#define MISSING_BUSY 9     /* Hero.busy while away (go_missing) */
#define STASH_MAX 48       /* estate trinket inventory */
#define WAGON_MAX 12       /* nomad_wagon number_of_trinkets_upgrades top */

/* One quest on the board (the contract the player can accept). */
typedef struct {
    uint8_t dungeon, level, length, quest; /* level 1/3/5, length 1..3, QT_* */
    uint8_t plot;                          /* story quest (unused since the Old Road starts the campaign directly) */
    uint8_t heir;                          /* heirloom reward type, HEIR_* */
} QuestOffer;

/* A dead hero's memorial entry (the graveyard screen). */
typedef struct {
    char name[16];
    int16_t cls;
    uint8_t level;
    int16_t week;
} Fallen;

#define GRAVEYARD_MAX 24
#define TOWN_EVENTS_MAX 64

/* The save-game record. Fields roughly group as: roster/recruits/party;
 * money & progress (gold, week, quests_done, heirlooms, building upgrades);
 * the quest board and current expedition; trinket stash & wagon; town-event
 * state; logs/journal; game-mode flags. */
typedef struct {
    uint32_t magic, version;
    Hero roster[ROSTER_MAX];
    int nroster;
    Hero recruits[RECRUITS];
    int gold;
    int week;
    int party[4];          /* roster indices by rank (1..4), -1 empty */
    int in_dungeon;
    Fallen graveyard[GRAVEYARD_MAX];
    int nfallen;
    int quests_done;
    int highest_level;     /* highest dungeon difficulty completed */
    int heirlooms[HEIR_COUNT];
    uint8_t upgrades[TREES_MAX];  /* levels bought per dd_trees entry */
    QuestOffer board[BOARD_MAX];
    int nboard;
    Dungeon dungeon;
    uint32_t seed;
    int16_t stash[STASH_MAX]; /* unequipped trinkets (dd_trinkets index) */
    int nstash;
    int16_t wagon[WAGON_MAX]; /* Nomad Wagon stock this week: dd_trinkets index + 1, 0 = sold */
    uint8_t dd_done;          /* Darkest Dungeon quests completed (they unlock in order) */
    uint8_t game_won;         /* the Heart of Darkness is dead */
    /* town events (town_event.c) */
    int8_t event;             /* active this week (dd_town_events), -1 none */
    int8_t event_force;       /* guaranteed next by a finished quest, -1 none */
    uint8_t event_visits;     /* town visits since the last event */
    uint8_t event_seen;       /* the event card was shown */
    uint8_t event_free;       /* free upgrades used (bit = tag) */
    uint8_t event_missed[TOWN_EVENTS_MAX];
    int16_t event_last[TOWN_EVENTS_MAX]; /* week fired + 1, 0 never */
    uint32_t journal;         /* journal pages found (bit = page) */
    uint8_t mode;             /* GM_*: Radiant, Darkest, Stygian */
    uint16_t deaths;          /* heroes lost in the whole campaign (the graveyard keeps the last 24) */
    char log[LOG_MAX][128];   /* activity log since the hamlet was last shown */
    uint8_t nlog, log_unseen;
    uint32_t boss_done;       /* bit boss*3 + level index: caretaker boss goals (Ruins..Cove, 8 bosses) */
    uint8_t upgrade_log[64];  /* building upgrades in purchase order (dd_trees index), newest last */
    uint8_t nupgrade_log;
    int16_t hoard[40];        /* the Shrieker's hoard: lost trinkets (dd_trinkets index) */
    uint8_t nhoard;
    uint8_t plot_done;        /* bit 0: Wolves at the Door won (not repeatable), bit 1: first Ruins quest done */
    uint8_t game_over;        /* Stygian limits reached: 1 weeks, 2 dead heroes */
} Campaign;

extern Campaign g_camp;

/* Start a fresh campaign in the given game mode (Radiant/Darkest/Stygian):
 * clears everything and sets up the opening Old Road expedition. */
void campaign_new(uint32_t seed, int mode);
/* Adds a line to the activity log shown when the hamlet opens. */
void campaign_log(const char *fmt, ...);
/* The Shrieker's hoard: add a lost trinket; the Perch level its contents call for (PQ_PERCH + 0..2, -1 none). */
void hoard_add(int trinket);
int hoard_perch(void);
/* Stygian: sets g_camp.game_over when the week or death limit is passed; returns it. */
int campaign_check_game_over(void);
/* Save/load the whole struct to save.bin. Load returns 0 ok, <0 if missing,
 * wrong size, or wrong magic/version (an old save). */
int campaign_load(void);
int campaign_save(void);

/* Roll a brand-new hero of class cls: random name, 4 skills (one class-
 * guaranteed), 1-2 positive and 1-2 negative quirks. */
void hero_generate(Hero *h, Rng *r, int cls);
int hero_max_hp(const Hero *h);
/* Adds resolve XP; returns 1 if the hero levelled up. */
int hero_add_xp(Hero *h, int xp);
void campaign_refresh_recruits(Rng *r);
/* Moves recruit i into the roster; 0 on success. */
int campaign_hire(int i);
void campaign_dismiss(int roster_index);
/* Records a dead hero in the graveyard and removes them from the roster. */
void campaign_bury(int roster_index);
/* Next week in town: new recruits, idle heroes recover, new quest board. */
void campaign_next_week(void);
void campaign_make_board(Rng *r);
int dungeon_unlocked(int dungeon);
/* Quests finished needed to open a building (town building.json requirements), -1 = needs a
 * level-2+ dungeon (Survivalist). */
int building_unlocked(const char *id);
/* Upgrade helpers; return the gold cost or -1 if not available (max level / resolve too low). */
/* Building upgrades: tree index by id (-1), levels bought, the value a tree grants
 * (TV_DISCOUNT sums every bought level; other keys take the highest level reached, else base). */
int tree_find(const char *id);
int tree_level(const char *id);
float tree_value(const char *id, int key, float base);
/* Buys the next level; 0 ok, -1 maxed, -2 can't afford. */
int tree_buy(int tree);
int campaign_roster_size(void);
int upgrade_weapon_cost(const Hero *h);
int upgrade_armour_cost(const Hero *h);
int upgrade_skill_cost(const Hero *h, int skill);

/* Trinket inventory: 0 ok, -1 full. */
int stash_add(int trinket);
void stash_remove(int i);
/* Equips stash entry i on hero h in slot s (the worn one goes back to the stash); NULL or a reason. */
const char *trinket_equip(Hero *h, int s, int i);
/* Unequips slot s into the stash; 0 ok, -1 stash full. */
int trinket_unequip(Hero *h, int s);
/* Nomad Wagon: weekly stock, price after the cost upgrades, sale value. */
void wagon_restock(Rng *r);
int wagon_count(void);
int wagon_price(int trinket);
int trinket_sell_value(int trinket);

#endif
