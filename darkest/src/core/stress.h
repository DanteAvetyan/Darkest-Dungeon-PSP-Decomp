/*
 * stress.h - public hooks of the stress system.
 * The battle engine and dungeon call these at the moments stress can change
 * (hits, crits, kills, deaths, corridor steps, town visits). All the numbers
 * live in stress.c.
 * Stress, the resolve check (afflictions / virtues), act-outs and heart attacks.
 * Rules recovered from the original; see docs/mechanics.md ("Stress"). */
#ifndef DD_STRESS_H
#define DD_STRESS_H

#include "battle.h"

/* ---- battle ----------------------------------------------------------------------- */

/* `attacker` landed a hit on `target` (crit != 0 if it was a critical hit). */
void stress_on_hit(Battle *b, int attacker, int target, int crit);
/* `attacker` missed or was dodged by everything. */
void stress_on_miss(Battle *b, int attacker);
/* A hero landed a killing blow. */
void stress_on_kill(Battle *b, int attacker, int victim);
/* A skill effect with OP_STRESS / OP_HEAL_STRESS reached `target`. */
void stress_apply_effect(Battle *b, int source, int target, float amount, int is_heal);
/* A hero dropped to 0 HP and is now on death's door. */
void stress_on_deaths_door(Battle *b, int idx);
/* A hero died. */
void stress_on_hero_death(Battle *b, int idx);
/* A hero passed their turn. */
void stress_on_pass(Battle *b, int idx);
/* Start of a hero's turn. Returns 1 if an affliction act-out used up the turn. */
int stress_turn_start(Battle *b, int idx);
/* End of every round. */
void stress_on_round_end(Battle *b);
/* An afflicted hero may refuse help: kind 0 heal, 1 move, 2 buff. Returns 1 if refused. */
int stress_refuses(Battle *b, int idx, int kind);
/* Applies trait buffs to a hero entering battle. */
void stress_battle_start(Battle *b, int idx);

/* ---- dungeon / town ------------------------------------------------------------------ */

/* One corridor step (backwards != 0 when retreating) with the given torch and dungeon level.
 * Returns the number of heroes that took stress. */
int stress_on_corridor_step(Hero *party[], int n, int torch, int level, int backwards, uint32_t seed);
/* Stress relief in town; clears afflictions. */
void stress_on_town_week(Hero *h, int relief);
/* End of an expedition: virtues fade. */
void stress_on_expedition_end(Hero *h);
/* Direct stress outside battle (curios, traps). Positive damages, negative relieves. */
void stress_add(Hero *h, int amount, int level, uint32_t seed);

/* Name of the hero's affliction/virtue, or NULL if none; *virtue set to 1 for virtues. */
const char *stress_affliction_name(const Hero *h, int *virtue);

#endif
