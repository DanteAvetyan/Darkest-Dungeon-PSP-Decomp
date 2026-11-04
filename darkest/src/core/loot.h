/*
 * loot.h - loot generation. A LOOT TABLE is a weighted list of entries; each
 * entry may be gold, a gem, heirlooms, a supply item, a trinket of some rarity,
 * a journal page, or ANOTHER table (so tables nest). A "draw" picks one entry.
 * Loot from the game's loot tables (loot/<name>.loot.json): monster drops, darkness bonuses, curios. */
#ifndef DD_LOOT_H
#define DD_LOOT_H
#include "battle.h"
#include "dungeon.h"

/* One stack of found loot. `ref` meaning depends on `kind`. */
typedef struct Loot {
    uint8_t kind;   /* LK_GOLD, LK_GEM, LK_HEIRLOOM, LK_ITEM, LK_TRINKET */
    int16_t ref;    /* gem index, HEIR_*, ItemId, dd_trinkets index */
    int16_t amount;
} Loot;

#define LOOT_MAX 16

/* Draws `draws` times from table id (dd_loot_ids index) and stacks the results into out[0..n);
 * returns the new count. */
int loot_draw(Rng *r, int table, int draws, int level, int dungeon, Loot *out, int n);
/* Drops of every monster killed in the battle, plus the low-light bonus. */
int loot_battle(Rng *r, const Battle *b, int level, int dungeon, int torch, Loot *out);
/* Adds the loot to the expedition (gems are carried as their sale value). */
void loot_take(Dungeon *d, const Loot *l, int n);
/* Page an LK_JOURNAL loot gives (the first unknown in its range), or -1 if all are known. */
int loot_journal_page(const Dungeon *d, const Loot *l);
/* "250 Gold", "Jade", "2 Busts", "Torch". */
const char *loot_label(const Loot *l, char *buf, int len);
#endif
