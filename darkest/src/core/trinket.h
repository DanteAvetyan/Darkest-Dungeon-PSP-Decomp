/*
 * trinket.h - equippable items. Each trinket is a list of buffs; heroes wear
 * two. Rolled as loot (by rarity), bought at the Nomad Wagon, or won as rewards.
 * Trinkets (trinkets/base.entries.trinkets.json): two slots per hero, buffs judged like quirk buffs. */
#ifndef DD_TRINKET_H
#define DD_TRINKET_H
#include "battle.h"
#include "rng.h"

/* Random trinket of a rarity that can drop in dungeon (origin_dungeon trinkets only drop there,
 * -1 = only trinkets without an origin); -1 if none. */
int trinket_roll(Rng *r, int rarity, int dungeon);
/* Out-of-battle sum of the hero's trinket buffs for a stat (always / in-dungeon rules only). */
float trinket_stat(const Hero *h, int stat, int sub);
/* Pushes the equipped trinkets' buffs onto a battle actor (rules judged live). */
void trinkets_to_actor(Actor *a);
/* 0 if hero h may wear trinket t in slot `slot`, else a reason (class only / one per hero). */
const char *trinket_equip_error(const Hero *h, int t, int slot);
/* "SPD +3, DMG +15%, Stress +15%, ACC -5" (conditional buffs marked with *). */
const char *trinket_describe(int t, char *buf, int len);
int stat_change_text(int stat, int sub, int mode, float amount, char *buf, int len);
const char *trinket_rarity_name(int rarity);
#endif
