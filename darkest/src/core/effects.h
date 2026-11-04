/* Applying skill effects (stun, bleed, buffs, marks, moves...) and buff bookkeeping. */
#ifndef DD_EFFECTS_H
#define DD_EFFECTS_H

#include "battle.h"

/* Apply one effect from `performer`'s skill. `target` is the actor the skill hit (or tried to);
 * the effect's own target type decides who actually receives it. */
void effects_apply(Battle *b, int effect, int performer, int target, int hit, int crit);

/* Apply all effects of a skill for one target. */
void effects_apply_skill(Battle *b, int skill, int performer, int target, int hit, int crit);

/* Adds the skill's instant bonuses that apply against `target` to the performer for the
 * duration of one hit; returns the buff count to restore afterwards (actor.nbuffs = value). */
int effects_push_instant(Battle *b, int skill, int performer, int target);

/* Buff helpers */
void effects_add_buff(Actor *a, int stat, int sub, int mode, float amount, int rounds, int negative);
void effects_clear_debuffs(Actor *a);
void effects_apply_deaths_door(Battle *b, int idx);
void effects_leave_deaths_door(Battle *b, int idx);

/* Start of the actor's turn: bleed/blight damage and countdowns. Returns total damage dealt. */
int effects_tick_dots(Battle *b, int idx);
/* End of the actor's turn: count down buffs and timed states. */
void effects_end_turn(Battle *b, int idx);

/* A stunned actor's turn was skipped: apply the stun recovery buff. */
void effects_stun_recovery(Battle *b, int idx);

/* Boss mechanics (battle.c) */
void battle_capture(Battle *b, int performer, int hero);
void battle_control(Battle *b, int hero, int rounds);

/* Shared with battle.c */
void battle_kill(Battle *b, int idx, int attacker);
void battle_damage(Battle *b, int idx, int amount, int attacker, int is_dot);
void battle_heal(Battle *b, int idx, int amount);
void battle_shift(Battle *b, int actor, int delta);

/* KS_* key status (tagged, poisoned, stunned, bleeding) on an actor. */
int effects_key_status(const Actor *t, int ks);
#endif
