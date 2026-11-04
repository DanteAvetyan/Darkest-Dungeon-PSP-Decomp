/*
 * curio.h - CURIOS are interactive objects in rooms and halls (chests,
 * bookshelves, altars...). Investigating one rolls a weighted result: loot, a
 * quirk, a trap-like effect, a teleport... Using the right provision item on
 * it (e.g. a key on a locked chest) switches to a different, better result
 * table.  Data: curio_type_library.csv.
 */
#ifndef DD_CURIO_H
#define DD_CURIO_H

#include "battle.h"
#include "dungeon.h"
#include "loot.h"

/* What the caller must do after curio_use(). */
enum { CURIO_DONE, CURIO_TELEPORT, CURIO_SUMMON };

/* Items from the party's provisions that do something on this curio; returns count. */
int curio_usable_items(const Dungeon *d, int curio, int out[ITEM_COUNT]);
/* Investigate (item = -1) or use an item. `who` is the investigating hero.
 * Writes a message; returns CURIO_TELEPORT if the party should be moved to a random room. */
/* Loot found by the last curio_use (shown in the loot window). */
extern Loot curio_loot[LOOT_MAX];
extern int curio_nloot;
int curio_use(Dungeon *d, Hero *party[], int n, int who, int curio, int item, uint32_t seed, char *msg, int msglen);
/* Applies an effect outside battle (curios, camping, hunger). */
/* One out-of-battle DoT tick (each new hall tile): returns the damage (HP stays >= 1). */
int hero_dot_tick(Hero *h);
void hero_apply_effect_offline(Dungeon *d, Hero *party[], int n, int who, int effect, Rng *r);

#endif
