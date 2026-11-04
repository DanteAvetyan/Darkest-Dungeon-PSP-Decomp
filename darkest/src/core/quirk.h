/*
 * quirk.h - QUIRKS are permanent personality traits ("Tough", "Kleptomaniac",
 * "Slow Reflexes"). Each is a bundle of buffs (some conditional). Heroes carry
 * up to 5 positive and 5 negative, plus up to 3 diseases. Quirks can be gained
 * from curios, expeditions and town; the sanitarium treats them.
 * Quirks and diseases (shared/quirk/quirk_library.json; gain/lock rules from rules.json and the wiki). */
#ifndef DD_QUIRK_H
#define DD_QUIRK_H
#include "battle.h"
#include "rng.h"

#define QUIRK_MAX_POSITIVE 5 /* rules.json quirks_max_positive */
#define QUIRK_MAX_NEGATIVE 5
#define QUIRK_MAX_DISEASES 3

/* Weighted random quirk of the kind the hero doesn't have yet; -1 if none. */
int quirk_random(Rng *r, const Hero *h, int positive, int disease);
/* Adds quirk q: an incompatible one is swapped out, a full category replaces a random unlocked
 * quirk. Returns 1 if added. */
int quirk_add(Hero *h, int q, Rng *r);
void quirk_remove(Hero *h, int slot);
int quirk_has(const Hero *h, int q);
/* dd_quirks index of "fated" (-1 if missing). */
int quirk_fated(void);
#define FATED_CHANCE 0.05f
/* Out-of-battle sum of the hero's quirk buffs for a stat (always / in-dungeon rules only). */
float quirk_stat(const Hero *h, int stat, int sub);
/* Pushes the quirk buffs onto a battle actor (rules judged live). */
void quirks_to_actor(Actor *a);
/* End of an expedition: roll new quirks (success/failure formulas) and lock negatives (25%).
 * Writes a short summary to msg. */
/* New quirks after an expedition; their names go to gained / lost-side text ("" when none). */
void quirks_after_expedition(Hero *h, int success, Rng *r, char *pos, char *neg, int len);
#endif
