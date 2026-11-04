/* Combat rules. Each function documents the behaviour recovered from the original binary
 * (see docs/mechanics.md for the evidence) and is re-implemented here from that description. */
#ifndef DD_FORMULAS_H
#define DD_FORMULAS_H

#include "battle.h"

/* Base (unbuffed) stat of an actor: heroes from class + weapon/armour level, monsters from class. */
float base_stat(const Actor *a, DDStat stat);

/* Base value with the actor's buffs applied: (base + adds) * (1 + multipliers). */
float stat_from_base(const Actor *a, DDStat stat, float base);
/* Context for conditional (rule) buffs: the battle, and the skill / target of the attack being worked out. */
void formulas_context(const Battle *b, const DDSkill *skill, const Actor *target);
int buff_rule_ok(const Actor *a, const DDBuff *bf);
float actor_resistance(const Actor *a, int res);
float actor_sub_stat(const Actor *a, int stat, int sub); /* live buffs with a sub type */
float f_surprise_light(int torch, int heroes);              /* light-level surprise bonus, 0..1 */

/* Chance that `skill` used by `att` hits `tgt` (before the always-hit cutoff). */
float f_hit_chance(const Battle *b, const Actor *att, const DDSkill *skill, const Actor *tgt);
/* Hit test for one uniform roll in [0, 1). */
int f_roll_hit(const Battle *b, const Actor *att, const DDSkill *skill, const Actor *tgt, float roll);
float f_crit_chance(const Battle *b, const Actor *att, const DDSkill *skill, const Actor *tgt);
/* Damage before protection for a hit (crit applies the crit multiplier). */
int f_damage(Battle *b, const Actor *att, const DDSkill *skill, const Actor *tgt, int crit);
/* Healing from a heal skill. */
int f_heal(Battle *b, const Actor *att, const DDSkill *skill, const Actor *tgt, int crit);
/* Heal::Source k_TypeData (decomp13): crit chance / multiplier of a heal by its source.
 * multi = a skill healing several targets. */
float f_heal_crit_chance(const Actor *healer, int multi);
float f_heal_crit_mult(const Actor *healer);
/* Chance an effect lands after the target's resistance. */
float f_effect_chance(const Actor *att, const DDEffect *e, const Actor *tgt, int resist, int crit);
/* Initiative roll for a round. */
int f_initiative(Battle *b, const Actor *a);
/* Death blow chance for a hero hit while on death's door. */
float f_deathblow_chance(const Actor *a);

/* Darkness bonuses (DK_*, in %) for a torch level, from the campaign mode's rules. */
const float *dark_for(int torch);
#endif
