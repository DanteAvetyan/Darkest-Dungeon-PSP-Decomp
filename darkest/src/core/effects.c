/*
 * effects.c - applies skill EFFECTS (stun, bleed, buffs, moves, marks...) and
 * keeps buff / damage-over-time bookkeeping.
 *
 * A skill points at a list of effects (dd_skill_effects). Each effect has a
 * chance, a target kind, conditions and a list of OPS (stun, push, heal...).
 * effects_apply() decides who receives it and rolls the chance;
 * apply_op() performs one op on one receiver.
 */
#include "effects.h"

#include <string.h>

#include "formulas.h"
#include "quirk.h"
#include "stress.h"

/* ---------------------------------------------------------------- buffs */

/* Attach a stat buff to an actor. rounds<0 = until the battle ends. `negative`
 * flags it as a debuff so "clear debuffs" can remove it later. */
void effects_add_buff(Actor *a, int stat, int sub, int mode, float amount, int rounds, int negative) {
    if (stat == STAT_NONE || amount == 0) return;
    if (a->nbuffs >= MAX_BUFFS) return;
    Buff *bf = &a->buffs[a->nbuffs++];
    bf->stat = (uint8_t)stat;
    bf->sub = (uint8_t)sub;
    bf->mode = (uint8_t)mode;
    bf->amount = amount;
    bf->rounds = (int8_t)rounds;
    bf->from_effect = (uint8_t)negative;
    bf->rule_buff = -1;
}

/* Remove all negative buffs (cleanse/clear_debuff). */
void effects_clear_debuffs(Actor *a) {
    int n = 0;
    for (int i = 0; i < a->nbuffs; i++)
        if (!a->buffs[i].from_effect) a->buffs[n++] = a->buffs[i];
    a->nbuffs = n;
}

/* Add a buff defined in the data tables (dd_buffs[buff]). */
static void add_named_buff(Actor *a, int buff, int rounds_override) {
    const DDBuff *bd = &dd_buffs[buff];
    int rounds = rounds_override != 0 ? rounds_override : (bd->duration_type == DUR_ROUNDS ? bd->duration : -1);
    effects_add_buff(a, bd->stat, bd->sub, bd->mode, bd->amount, rounds, bd->amount < 0);
}

/* Look up a data buff by its string id (linear search; used rarely). */
static int find_buff(const char *id) {
    for (int i = 0; i < dd_buff_count; i++)
        if (strcmp(dd_buffs[i].id, id) == 0) return i;
    return -1;
}

/* Entering death's door gives the hero class-specific penalty buffs. They get
 * from_effect=2 as a tag so leaving death's door can strip exactly these. */
void effects_apply_deaths_door(Battle *b, int idx) {
    Actor *a = &b->actors[idx];
    const DDHeroClass *h = &dd_heroes[a->cls];
    for (int i = 0; i < h->dd_buff_count; i++) {
        add_named_buff(a, dd_buff_list[h->dd_buff_first + i], -1);
        a->buffs[a->nbuffs - 1].from_effect = 2; /* tagged so leaving death's door removes them */
    }
}

/* Healed back from death's door: remove the penalties, then add the "recovery"
 * debuffs (the lasting cost of a close call). They also go on the roster hero
 * so they persist until the party camps or returns to town. */
void effects_leave_deaths_door(Battle *b, int idx) {
    Actor *a = &b->actors[idx];
    int n = 0;
    for (int i = 0; i < a->nbuffs; i++)
        if (a->buffs[i].from_effect != 2) a->buffs[n++] = a->buffs[i];
    a->nbuffs = n;
    /* "mortality" recovery debuffs: for this battle, and carried on the hero until the party camps
     * or returns to town (HeroBuff battles = HB_UNTIL_CAMP; not stacked) */
    const DDHeroClass *h = &dd_heroes[a->cls];
    /* recovery_heart_attack_buffs replace the mortality ones after a heart attack */
    int first = a->heart_attacked && h->ha_recovery_count ? h->ha_recovery_first : h->dd_recovery_first;
    int count = a->heart_attacked && h->ha_recovery_count ? h->ha_recovery_count : h->dd_recovery_count;
    a->heart_attacked = 0;
    for (int i = 0; i < count; i++) {
        const DDBuff *bd = &dd_buffs[dd_buff_list[first + i]];
        add_named_buff(a, dd_buff_list[first + i], -1);
        Hero *hh = a->hero;
        if (!hh) continue;
        int have = 0;
        for (int k = 0; k < hh->nebuffs; k++)
            have |= hh->ebuffs[k].battles == HB_UNTIL_CAMP && hh->ebuffs[k].stat == bd->stat && hh->ebuffs[k].sub == bd->sub;
        if (have || hh->nebuffs >= HERO_BUFFS) continue;
        HeroBuff *eb = &hh->ebuffs[hh->nebuffs++];
        eb->stat = bd->stat;
        eb->sub = bd->sub;
        eb->mode = bd->mode;
        eb->amount = bd->amount;
        eb->battles = HB_UNTIL_CAMP;
    }
}

/* ---------------------------------------------------------------- dots / turn upkeep
 * DoT = damage over time (bleed, blight). Each actor has a few slots; every
 * slot is {damage per tick, rounds left}. Stacking effects fill free slots. */

/* Put a new DoT in a free slot (rounds <= 0 means empty). */
static void add_dot(Dot *dots, int amount, int rounds) {
    for (int i = 0; i < MAX_DOTS; i++)
        if (dots[i].rounds <= 0) {
            dots[i].amount = (int8_t)amount;
            dots[i].rounds = (int8_t)rounds;
            return;
        }
}

/* Total damage per tick across all active slots. */
static int dot_total(const Dot *dots) {
    int t = 0;
    for (int i = 0; i < MAX_DOTS; i++)
        if (dots[i].rounds > 0) t += dots[i].amount;
    return t;
}

/* One round passes: every active slot has one fewer round to live. */
static void dot_countdown(Dot *dots) {
    for (int i = 0; i < MAX_DOTS; i++)
        if (dots[i].rounds > 0) dots[i].rounds--;
}

/* Raid::ApplyDot at the start of the actor's turn: bleed + blight hit as one damage instance,
 * then DoT and buff durations count down. */
int effects_tick_dots(Battle *b, int idx) {
    Actor *a = &b->actors[idx];
    int bleed = dot_total(a->bleed), blight = dot_total(a->blight);
    int total = bleed + blight;
    if (total > 0) {
        battle_event(b, bleed >= blight ? EV_BLEED : EV_BLIGHT, idx, -1, total, -1);
        battle_damage(b, idx, total, -1, 1);
    }
    dot_countdown(a->bleed);
    dot_countdown(a->blight);
    int horror = dot_total(a->horror);
    if (horror > 0 && a->alive && a->is_hero) stress_apply_effect(b, -1, idx, (float)horror, 0);
    dot_countdown(a->horror);
    int n = 0;
    for (int i = 0; i < a->nbuffs; i++) {
        Buff *bf = &a->buffs[i];
        if (bf->rounds > 0 && --bf->rounds == 0) continue;
        a->buffs[n++] = *bf;
    }
    a->nbuffs = n;
    return total;
}

/* End of a turn: tick down the "rounds left" counters of mark, stealth,
 * riposte, immobilise, control, and guard (a guard ending frees its ward). */
void effects_end_turn(Battle *b, int idx) {
    Actor *a = &b->actors[idx];
    if (a->marked) a->marked--;
    if (a->stealth) a->stealth--;
    if (a->riposte) a->riposte--;
    if (a->immobile) a->immobile--;
    if (a->controlled) a->controlled--;
    if (a->guard_rounds && --a->guard_rounds == 0) {
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].guarded_by == idx) b->actors[i].guarded_by = -1;
    }
}

/* ---------------------------------------------------------------- effects */

/* Does the target have the status an effect requires (e.g. "only vs marked")? */
static int target_has_status(const Actor *t, int ks);
int effects_key_status(const Actor *t, int ks) { return target_has_status(t, ks); }
static int target_has_status(const Actor *t, int ks) {
    switch (ks) {
    case KS_MARKED: return t->marked > 0;
    case KS_BLIGHTED: return dot_total(t->blight) > 0;
    case KS_BLEEDING: return dot_total(t->bleed) > 0;
    case KS_STUNNED: return t->stunned;
    default: return 1;
    }
}

/* Effects can be limited to monster types (e.g. only vs unholy). Heroes never match. */
static int monster_type_ok(const Actor *t, int types) {
    if (!types) return 1;
    if (t->is_hero) return 0;
    return (dd_monsters[t->cls].types & types) != 0;
}

/* Resistance that guards against an op, or -1 if it isn't resisted. */
static int resist_for(const DDEffOp *op, int negative) {
    switch (op->op) {
    case OP_STUN: return RES_STUN;
    case OP_BLEED: return RES_BLEED;
    case OP_BLIGHT: return RES_BLIGHT;
    case OP_PUSH: case OP_PULL: case OP_SHUFFLE_TARGET: return RES_MOVE;
    case OP_STAT_ADD: case OP_STAT_MUL: case OP_BUFF: return negative ? RES_DEBUFF : -1;
    default: return -1;
    }
}

/* Do ONE op to ONE receiver. A big switch, one case per DDEffectOp (see the
 * enum in dd_types.h). `rounds` is how long a status lasts: -1 for "until the
 * battle ends", else the effect's duration. */
static void apply_op(Battle *b, const DDEffect *e, const DDEffOp *op, int performer, int recv, int crit) {
    Actor *r = &b->actors[recv];
    int rounds = (e->flags & EF_COMBAT_END) ? -1 : e->duration;
    switch (op->op) {
    case OP_STUN:
        if (r->alive) {
            r->stunned = 1;
            r->guarded_by = -1; /* a stunned guard stops guarding */
            for (int i = 0; i < MAX_ACTORS; i++)
                if (b->actors[i].guarded_by == recv) b->actors[i].guarded_by = -1;
            battle_event(b, EV_STUN, recv, performer, 0, -1);
        }
        break;
    case OP_UNSTUN: r->stunned = 0; break;
    case OP_BLEED:
        add_dot(r->bleed, (int)op->v, e->duration > 0 ? e->duration : 3);
        battle_event(b, EV_BLEED, recv, performer, (int)op->v, -1);
        break;
    case OP_BLIGHT:
        add_dot(r->blight, (int)op->v, e->duration > 0 ? e->duration : 3);
        battle_event(b, EV_BLIGHT, recv, performer, (int)op->v, -1);
        break;
    case OP_HORROR: /* heroes only; the base effect chance is the only roll (Hypothesis) */
        if (r->is_hero && r->alive) add_dot(r->horror, (int)op->v, e->duration > 0 ? e->duration : 3);
        break;
    case OP_CURE: memset(r->bleed, 0, sizeof(r->bleed)); memset(r->blight, 0, sizeof(r->blight)); break;
    case OP_CURE_BLEED: memset(r->bleed, 0, sizeof(r->bleed)); break;
    case OP_CURE_BLIGHT: memset(r->blight, 0, sizeof(r->blight)); break;
    case OP_HEAL: { /* hp_heal_percent on the healer; source_heal_type hero_skill heals can crit */
        float v = op->v * (1 + (performer >= 0 ? actor_stat(&b->actors[performer], STAT_HEAL_DEALT) : 0));
        if ((e->flags2 & EF2_HEAL_CAN_CRIT) && performer >= 0 && rng_float(&b->rng) < f_heal_crit_chance(&b->actors[performer], 0))
            v *= f_heal_crit_mult(&b->actors[performer]);
        battle_heal(b, recv, (int)(v + 0.5f));
        break;
    }
    case OP_STRESS: stress_apply_effect(b, performer, recv, op->v, 0); break;
    case OP_HEAL_STRESS: stress_apply_effect(b, performer, recv, op->v, 1); break;
    case OP_KILL: battle_kill(b, recv, performer); break;
    case OP_PUSH:
        if (!r->immobile) battle_shift(b, recv, (int)op->v);
        break;
    case OP_PULL:
        if (!r->immobile) battle_shift(b, recv, -(int)op->v);
        break;
    case OP_SHUFFLE_TARGET:
        if (!r->immobile) battle_shift(b, recv, rng_range(&b->rng, -2, 2));
        break;
    case OP_SHUFFLE_PARTY:
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].used && b->actors[i].alive && b->actors[i].side == r->side)
                battle_shift(b, i, rng_range(&b->rng, -2, 2));
        break;
    case OP_IMMOBILIZE: r->immobile = (uint8_t)(rounds > 0 ? rounds : 3); break;
    case OP_UNIMMOBILIZE: r->immobile = 0; break;
    case OP_MARK:
        r->marked = (uint8_t)(rounds > 0 ? rounds : 3);
        battle_event(b, EV_MARK, recv, performer, 0, -1);
        break;
    case OP_UNMARK: r->marked = 0; break;
    case OP_GUARD: {
        /* performer guards the receiver (swap flag: the receiver guards the performer) */
        int guard = (e->flags & EF_SWAP) ? recv : performer, ward = (e->flags & EF_SWAP) ? performer : recv;
        if (guard == ward) break;
        b->actors[ward].guarded_by = (int8_t)guard;
        b->actors[guard].guard_rounds = (uint8_t)(rounds > 0 ? rounds : 2);
        battle_event(b, EV_GUARD, guard, ward, 0, -1);
        break;
    }
    case OP_CLEAR_GUARDING:
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].guarded_by == recv) b->actors[i].guarded_by = -1;
        break;
    case OP_CLEAR_GUARDED: r->guarded_by = -1; break;
    case OP_RIPOSTE:
        r->riposte = (uint8_t)(rounds > 0 ? rounds : 2);
        battle_event(b, EV_RIPOSTE, recv, -1, 0, -1);
        break;
    case OP_STEALTH: r->stealth = (uint8_t)(rounds > 0 ? rounds : 2); break;
    case OP_UNSTEALTH: r->stealth = 0; break;
    case OP_CLEAR_DEBUFF: effects_clear_debuffs(r); break;
    case OP_TORCH_INC:
    case OP_TORCH_DEC: {
        int d = (int)op->v * (op->op == OP_TORCH_INC ? 1 : -1);
        b->torch += d;
        if (b->torch < 0) b->torch = 0;
        if (b->torch > 100) b->torch = 100;
        battle_event(b, EV_TORCH, performer, -1, b->torch, -1);
        break;
    }
    case OP_STAT_ADD:
    case OP_STAT_MUL: {
        int neg = op->v < 0;
        effects_add_buff(r, op->arg, 0, op->op == OP_STAT_MUL ? BUFF_MULTIPLY : BUFF_ADD, op->v,
                         rounds > 0 ? rounds : -1, neg);
        break;
    }
    case OP_BUFF:
        add_named_buff(r, op->arg, rounds);
        break;
    case OP_SUMMON: {
        /* fill the first free monster rank */
        int used = 0;
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].used && b->actors[i].alive && b->actors[i].side == SIDE_MONSTERS) used += b->actors[i].size;
        int size = dd_monsters[op->arg].size ? dd_monsters[op->arg].size : 1;
        if (used + size <= DD_RANKS) {
            int m = battle_add_monster(b, op->arg, used + 1);
            if (m >= 0) {
                b->actors[m].no_loot = (e->flags2 & EF2_SUMMON_NO_LOOT) != 0;
                battle_event(b, EV_SUMMON, m, performer, 0, -1);
            }
        }
        break;
    }
    case OP_CAPTURE: {
        int virtue = 0;
        if ((e->flags2 & EF2_VIRTUE_BLOCKS) && r->is_hero && r->hero && stress_affliction_name(r->hero, &virtue) && virtue) break;
        battle_capture(b, performer, recv);
        break;
    }
    case OP_CLEAR_CORPSES:
        for (int i = 0; i < MAX_ACTORS; i++) {
            Actor *c = &b->actors[i];
            if (c->used && c->alive && c->is_corpse && c->side == r->side) battle_kill(b, i, performer);
        }
        break;
    case OP_CONTROL: battle_control(b, recv, (int)op->v > 0 ? (int)op->v : 2); break;
    case OP_DISEASE: {
        Actor *t = &b->actors[recv];
        if (!t->is_hero || !t->hero || rng_chance(&b->rng, actor_resistance(t, RES_DISEASE))) break;
        int q = op->arg >= 0 ? op->arg : quirk_random(&b->rng, t->hero, 0, 1);
        if (q >= 0 && quirk_add(t->hero, q, &b->rng)) battle_event(b, EV_DEBUFF, recv, -1, 0, -1);
        break;
    }
    }
    (void)crit;
}

/* Apply one effect of a used skill. Steps:
 *   1. gate: hit/miss flags, monster type, required status of the target
 *   2. build the receiver list from the effect's target kind
 *   3. pre-pick a summon (weighted choice among summon ops, once per action)
 *   4. roll ONE random number for the effect; each op compares it with its own
 *      chance (base - the receiver's relevant resistance); failures show
 *      as RESIST events
 *   5. run the ops; emit one buff/debuff event if any stat changed.
 */
void effects_apply(Battle *b, int effect, int performer, int target, int hit, int crit) {
    const DDEffect *e = &dd_effects[effect];
    if (e->flags2 & EF2_INSTANT) return; /* counted during the attack itself, see effects_push_instant */
    if (hit && !(e->flags & EF_ON_HIT)) return;
    if (!hit && !(e->flags & EF_ON_MISS)) return;
    const Actor *t = &b->actors[target];
    if (!monster_type_ok(t, e->monster_types)) return;
    if (!target_has_status(t, e->key_status)) return;

    /* who receives it */
    int recv[MAX_ACTORS], nrecv = 0;
    const Actor *perf = &b->actors[performer];
    switch (e->target) {
    case ET_GLOBAL: /* party-wide things like the torch: applied once */
    case ET_PERFORMER: recv[nrecv++] = performer; break;
    case ET_TARGET: recv[nrecv++] = target; break;
    case ET_PERFORMER_GROUP_OTHER:
    case ET_TARGET_GROUP: {
        int side = e->target == ET_TARGET_GROUP ? t->side : perf->side;
        for (int i = 0; i < MAX_ACTORS; i++) {
            const Actor *a = &b->actors[i];
            if (!a->used || !a->alive || a->side != side) continue;
            if (e->target == ET_PERFORMER_GROUP_OTHER && i == performer) continue;
            recv[nrecv++] = i;
        }
        break;
    }
    }

    /* summon effects list several candidates with weights: one is picked, once per action */
    int summon_pick = -1;
    {
        float total = 0;
        for (int o = 0; o < e->op_count; o++)
            if (dd_eff_ops[e->op_first + o].op == OP_SUMMON) total += dd_eff_ops[e->op_first + o].v;
        if (total > 0 && !b->summoned) {
            float x = rng_float(&b->rng) * total;
            for (int o = 0; o < e->op_count && summon_pick < 0; o++) {
                const DDEffOp *op = &dd_eff_ops[e->op_first + o];
                if (op->op != OP_SUMMON) continue;
                if (x < op->v) summon_pick = o;
                else x -= op->v;
            }
        }
    }

    /* one roll per effect (EffectApply), lowered by 0.2 on a crit; each op checks its own resist */
    float roll = rng_float(&b->rng);
    for (int k = 0; k < nrecv; k++) {
        Actor *r = &b->actors[recv[k]];
        if (!r->alive && !(e->flags & EF_ON_DEATH) && !r->deaths_door) continue;
        int negative = (e->flags & EF_DEBUFF) != 0 && r->side != perf->side;
        int stat_changed = 0, stat_negative = 0;
        for (int o = 0; o < e->op_count; o++) {
            const DDEffOp *op = &dd_eff_ops[e->op_first + o];
            if (op->op == OP_SUMMON) {
                if (o == summon_pick && !b->summoned) {
                    b->summoned = 1;
                    /* summon_count: each extra monster re-picks among the weighted candidates */
                    for (int n = 0; n < e->summon_count; n++) {
                        const DDEffOp *pick = op;
                        if (n > 0) {
                            float total = 0, x;
                            for (int q = 0; q < e->op_count; q++)
                                if (dd_eff_ops[e->op_first + q].op == OP_SUMMON) total += dd_eff_ops[e->op_first + q].v;
                            x = rng_float(&b->rng) * total;
                            for (int q = 0; q < e->op_count; q++) {
                                const DDEffOp *c = &dd_eff_ops[e->op_first + q];
                                if (c->op != OP_SUMMON) continue;
                                if (x < c->v) { pick = c; break; }
                                x -= c->v;
                            }
                        }
                        apply_op(b, e, pick, performer, recv[k], crit);
                    }
                }
                continue;
            }
            int res = r->side != perf->side ? resist_for(op, negative) : -1;
            float chance = f_effect_chance(perf, e, r, res, crit && !(e->flags2 & EF2_NO_CRIT_ROLL));
            if (roll > chance) {
                if (res >= 0) battle_event(b, EV_RESIST, recv[k], performer, res, -1);
                continue;
            }
            apply_op(b, e, op, performer, recv[k], crit);
            if (op->op == OP_STAT_ADD || op->op == OP_STAT_MUL || op->op == OP_BUFF) {
                stat_changed = 1;
                stat_negative |= op->op == OP_BUFF ? dd_buffs[op->arg].amount < 0 : op->v < 0;
            }
        }
        if (stat_changed) battle_event(b, stat_negative ? EV_DEBUFF : EV_BUFF, recv[k], performer, 0, -1);
    }
}

/* Some skills have "instant" bonuses that only count while that very attack
 * resolves (e.g. +damage vs marked). We temporarily add them to the attacker,
 * and return the old buff count so the caller can restore (`nbuffs = value`). */
int effects_push_instant(Battle *b, int skill, int performer, int target) {
    Actor *a = &b->actors[performer];
    const Actor *t = &b->actors[target];
    int before = a->nbuffs;
    const DDSkill *s = &dd_skills[skill];
    for (int i = 0; i < s->eff_count; i++) {
        const DDEffect *e = &dd_effects[dd_skill_effects[s->eff_first + i]];
        if (!(e->flags2 & EF2_INSTANT)) continue;
        if (!monster_type_ok(t, e->monster_types) || !target_has_status(t, e->key_status)) continue;
        for (int o = 0; o < e->op_count; o++) {
            const DDEffOp *op = &dd_eff_ops[e->op_first + o];
            if (op->op == OP_STAT_ADD || op->op == OP_STAT_MUL)
                effects_add_buff(a, op->arg, 0, op->op == OP_STAT_MUL ? BUFF_MULTIPLY : BUFF_ADD, op->v, 0, 0);
        }
    }
    return before;
}

/* Run all effects of a skill against one target. Two passes so that effects
 * flagged QUEUE (stuns, DoTs, self-kills) resolve after the ordinary ones. */
void effects_apply_skill(Battle *b, int skill, int performer, int target, int hit, int crit) {
    const DDSkill *s = &dd_skills[skill];
    /* queued effects resolve after the rest (kill_performer_group_other before kill_self_queued) */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < s->eff_count; i++) {
            int e = dd_skill_effects[s->eff_first + i];
            if (!(dd_effects[e].flags & EF_QUEUE) == !pass) effects_apply(b, e, performer, target, hit, crit);
        }
}

/* Applied by battle.c when a stunned actor's turn is skipped (rules.json stun_recovery_buffs). */
void effects_stun_recovery(Battle *b, int idx) {
    static int buff = -2;
    if (buff == -2) buff = find_buff("STUNRECOVERYBUFF");
    if (buff >= 0) add_named_buff(&b->actors[idx], buff, 0);
}
