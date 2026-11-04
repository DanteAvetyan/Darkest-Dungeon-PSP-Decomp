/*
 * battle.c - the combat engine: turn order, damage, death, bosses.
 *
 * Reading guide (top to bottom):
 *   events/setup/queries  - bookkeeping and "who is where / what can hit what"
 *   positions             - moving actors between ranks
 *   damage & death        - HP loss, death's door, corpses, boss transformations
 *   turn flow             - rounds, initiative, DoTs, stun, AI turns
 *   perform               - resolving a chosen skill (hit roll, damage, effects)
 * Helpers live elsewhere: formulas.c (hit/crit/damage numbers), effects.c
 * (status effects), ai.c (monster choices), stress.c (stress/afflictions).
 * Rules recovered from the original are written up in docs/mechanics.md.
 */
#include "battle.h"
#include "dungeon.h"

#include <string.h>

#include "effects.h"
#include "formulas.h"
#include "quirk.h"
#include "trinket.h"
#include "stress.h"

/* ---------------------------------------------------------------- events */

/* Forget recorded events (the UI calls this after playing them back). */
void battle_clear_events(Battle *b) { b->nevents = 0; }

/* Append an event. Silently dropped if the buffer is full (MAX_EVENTS). */
void battle_event(Battle *b, EventType t, int actor, int target, int value, int skill) {
    if (b->nevents >= MAX_EVENTS) return;
    BattleEvent *e = &b->events[b->nevents++];
    e->type = (uint8_t)t;
    e->actor = (int8_t)actor;
    e->target = (int8_t)target;
    e->value = (int16_t)value;
    e->skill = (int16_t)skill;
}

/* ---------------------------------------------------------------- setup */

/* Reset a Battle to an empty fight. torch (0-100) is the party's light, which
 * changes hit chances and stress (see formulas.c). -1 means "none yet". */
void battle_init(Battle *b, uint32_t seed, int torch) {
    memset(b, 0, sizeof(*b));
    rng_seed(&b->rng, seed);
    b->torch = torch;
    b->current = -1;
    b->forced_skill = -1;
    b->last_skill = b->last_performer = -1;
    b->dungeon = -1;
}

/* Find a free Actor slot (initialised to "no guard / no prisoner / no pool"). */
static int alloc_actor(Battle *b) {
    /* prefer a never-used slot; otherwise recycle a dead monster's slot */
    int slot = -1;
    for (int i = 0; i < MAX_ACTORS && slot < 0; i++)
        if (!b->actors[i].used) slot = i;
    for (int i = 0; i < MAX_ACTORS && slot < 0; i++)
        if (!b->actors[i].alive && !b->actors[i].is_hero) slot = i;
    if (slot < 0) return -1;
    memset(&b->actors[slot], 0, sizeof(Actor));
    b->actors[slot].used = 1;
    b->actors[slot].guarded_by = -1;
    b->actors[slot].prisoner = -1;
    b->actors[slot].captured_by = -1;
    b->actors[slot].pool = -1;
    return slot;
}

/* Seat a roster hero in the fight: copy HP, carry in bleed/blight from the
 * dungeon halls, apply stress-start effects, quirk and trinket buffs, and
 * expedition buffs (which tick down one fight each). */
int battle_add_hero(Battle *b, Hero *h, int rank) {
    int i = alloc_actor(b);
    if (i < 0) return -1;
    Actor *a = &b->actors[i];
    a->side = SIDE_HEROES;
    a->is_hero = 1;
    a->cls = h->cls;
    a->hero = h;
    a->rank = (uint8_t)rank;
    a->size = 1;
    a->max_hp = (int16_t)base_stat(a, STAT_MAX_HP);
    a->hp = h->hp > 0 ? h->hp : a->max_hp;
    for (int k = 0; k < HERO_DOTS; k++) { /* bleed / blight carried in from the halls */
        a->bleed[k] = h->bleed[k];
        a->blight[k] = h->blight[k];
    }
    memset(h->bleed, 0, sizeof(h->bleed));
    memset(h->blight, 0, sizeof(h->blight));
    if (a->hp > a->max_hp) a->hp = a->max_hp;
    a->alive = 1;
    stress_battle_start(b, i);
    quirks_to_actor(a);
    trinkets_to_actor(a);
    /* expedition buffs from curios and camping; each counts down one fight */
    for (int k = 0; k < h->nebuffs; k++) {
        HeroBuff *eb = &h->ebuffs[k];
        effects_add_buff(a, eb->stat, eb->sub, eb->mode, eb->amount, -1, eb->amount < 0);
    }
    int n = 0;
    for (int k = 0; k < h->nebuffs; k++) {
        HeroBuff eb = h->ebuffs[k];
        if (eb.battles > 0) eb.battles--;
        if (eb.battles != 0) h->ebuffs[n++] = eb;
    }
    h->nebuffs = (uint8_t)n;
    return i;
}

/* Seat a monster of class mc. Shared-health monsters (e.g. Formless Flesh)
 * join a pool: all members mirror one HP total. Spawn effects run last. */
int battle_add_monster(Battle *b, int mc, int rank) {
    int i = alloc_actor(b);
    if (i < 0) return -1;
    Actor *a = &b->actors[i];
    const DDMonsterClass *m = &dd_monsters[mc];
    a->born_round = (int16_t)(b->round > 0 ? b->round : 1);
    a->side = SIDE_MONSTERS;
    a->cls = (int16_t)mc;
    a->look = (int16_t)mc;
    a->rank = (uint8_t)rank;
    a->size = m->size ? m->size : 1;
    a->max_hp = a->hp = m->hp;
    a->alive = 1;
    a->is_corpse = (m->types & (1 << MT_CORPSE)) != 0;
    if (m->shared_health) {
        int p = 0;
        while (p < b->npools && b->pool_id[p] != m->shared_health) p++;
        if (p == b->npools && b->npools < 4) {
            b->pool_id[p] = m->shared_health;
            b->pool_hp[p] = b->pool_max[p] = 0;
            b->npools++;
        }
        if (p < b->npools) {
            a->pool = (int8_t)p;
            b->pool_hp[p] = (int16_t)(b->pool_hp[p] + a->hp);
            b->pool_max[p] = (int16_t)(b->pool_max[p] + a->max_hp);
            for (int k = 0; k < MAX_ACTORS; k++)
                if (b->actors[k].used && b->actors[k].pool == p) {
                    b->actors[k].hp = b->pool_hp[p];
                    b->actors[k].max_hp = b->pool_max[p];
                }
        }
    }
    for (int e = 0; e < m->spawn_eff_count; e++) effects_apply(b, dd_skill_effects[m->spawn_eff_first + e], i, i, 1, 0);
    return i;
}

/* ---------------------------------------------------------------- queries */

/* Display name: a hero's own name, or the monster class name. */
const char *actor_name(const Actor *a) {
    if (a->is_hero) return a->hero->name[0] ? a->hero->name : dd_heroes[a->cls].name;
    return dd_monsters[a->cls].name;
}

/* Which side this actor's attacks go to. A mind-controlled hero flips: `!a->side`
 * normally, but a controlled hero keeps targeting their own old side. */
int battle_enemy_side(const Battle *b, int actor) {
    const Actor *a = &b->actors[actor];
    return a->controlled ? a->side : !a->side;
}

/* The monster's base class id (shared by its A/B/C tier variants). */
static const char *base_of(const Actor *a) { return a->is_hero ? "" : dd_monsters[a->cls].sprite; }

/* How many living monsters of base class `base` there are. */
static int count_base(const Battle *b, const char *base) {
    int n = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (a->used && a->alive && !a->is_hero && strcmp(base_of(a), base) == 0) n++;
    }
    return n;
}

/* Who stands on a rank. Big monsters cover several ranks, so we test the range
 * [rank, rank+size). Returns actor index or -1.
 * Rank occupancy: an actor of size 2 at rank r covers r and r+1. */
int battle_actor_at(const Battle *b, int side, int rank) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (a->used && a->alive && a->side == side && rank >= a->rank && rank < a->rank + a->size) return i;
    }
    return -1;
}

/* Count living actors on a side (corpses optionally excluded - a corpse is a
 * "monster" you can hit, but it doesn't keep the fight going). */
static int side_alive(const Battle *b, int side, int count_corpses) {
    int n = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (a->used && a->alive && a->side == side && (count_corpses || !a->is_corpse)) n++;
    }
    return n;
}

/* Absolute dd_skills index of the skill in hero equip slot `slot`, or -1. */
int battle_hero_skill(const Battle *b, int actor, int slot) {
    const Actor *a = &b->actors[actor];
    if (!a->is_hero || slot < 0 || slot >= HERO_SKILL_SLOTS) return -1;
    int s = a->hero->equipped[slot];
    if (s < 0 || s >= dd_heroes[a->cls].skill_count) return -1;
    /* skills are stored as 5 consecutive levels per class skill */
    return dd_heroes[a->cls].skill_first + s * 5 + a->hero->skill_lvl[s];
}

/* Index into Actor.skill_uses[] for per-battle use limits. */
static int skill_use_slot(const Actor *a, int skill) {
    int first = a->is_hero ? dd_heroes[a->cls].skill_first : dd_monsters[a->cls].skill_first;
    int idx = skill - first;
    if (a->is_hero) idx /= 5;
    return idx >= 0 && idx < 16 ? idx : -1;
}

/* Valid targets of a skill as a bitmask (bit r-1 = rank r). Starts from the
 * skill's allowed ranks, then removes: empty ranks, stealthed enemies (unless
 * the skill ignores stealth or hits all), untargetable monsters, and self
 * for skills that can't target self. *side_out gets the side being aimed at. */
int battle_targets(const Battle *b, int actor, int skill, int *side_out) {
    const Actor *a = &b->actors[actor];
    const DDSkill *s = &dd_skills[skill];
    int enemy = battle_enemy_side(b, actor);
    int side = (s->target.flags & RK_FRIENDLY) || (s->target.flags & RK_SELF) ? !enemy : enemy;
    if (s->target.flags & RK_SELF) side = a->side;
    if (side_out) *side_out = side;
    if (s->target.flags & RK_SELF) return 1 << (a->rank - 1);
    int mask = 0, taunted = 0;
    /* stealthed/unmarked enemies can't be singled out unless the skill ignores stealth */
    for (int r = 1; r <= DD_RANKS; r++) {
        if (!(s->target.mask & (1 << (r - 1)))) continue;
        int t = battle_actor_at(b, side, r);
        if (t < 0) continue;
        const Actor *ta = &b->actors[t];
        if (t == actor && !(s->flags & SKF_SELF_TARGET_OK) && (s->target.flags & RK_FRIENDLY)) continue;
        if (side != a->side && ta->stealth && !(s->flags & SKF_IGNORE_STEALTH) &&
            !(s->target.flags & RK_ALL))
            continue;
        if (side != a->side && dd_monsters[ta->cls].flags & MF_CANNOT_BE_HIT && !ta->is_hero) continue;
        if (!a->is_hero && ta->is_hero && ta->controlled) continue; /* on the monsters' side for now */
        mask |= 1 << (r - 1);
    }
    (void)taunted;
    return mask;
}

/* A skill is usable if the user stands on a launch rank, its per-battle use
 * limit isn't spent, and (unless it's a move) some valid target exists. */
int battle_skill_usable(const Battle *b, int actor, int skill) {
    const Actor *a = &b->actors[actor];
    const DDSkill *s = &dd_skills[skill];
    if (!(s->launch.mask & (1 << (a->rank - 1)))) return 0;
    if (s->per_battle_limit > 0) {
        int slot = skill_use_slot(a, skill);
        if (slot >= 0 && a->skill_uses[slot] >= s->per_battle_limit) return 0;
    }
    if (s->type == SK_MOVE) return 1;
    return battle_targets(b, actor, skill, NULL) != 0;
}

/* ---------------------------------------------------------------- positions */

/* After deaths, close the gaps so survivors slide forward in the same order. */
static void compact_side(Battle *b, int side) {
    /* close gaps left by the dead: everyone slides forward keeping order */
    int next = 1;
    for (int r = 1; r <= DD_RANKS + 2; r++) {
        for (int i = 0; i < MAX_ACTORS; i++) {
            Actor *a = &b->actors[i];
            if (a->used && a->alive && a->side == side && a->rank == r) {
                a->rank = (uint8_t)next;
                next += a->size;
            }
        }
    }
}

/* Move an actor by `delta` ranks (positive = towards the back) by repeatedly
 * swapping places with the neighbour in that direction. Clamped to the line
 * length. Used by push/pull skills as well as voluntary movement. */
void battle_shift(Battle *b, int actor, int delta) {
    Actor *a = &b->actors[actor];
    if (!a->alive || delta == 0) return;
    int total = 0;
    for (int i = 0; i < MAX_ACTORS; i++)
        if (b->actors[i].used && b->actors[i].alive && b->actors[i].side == a->side) total += b->actors[i].size;
    int target = a->rank + delta;
    if (target < 1) target = 1;
    if (target + a->size - 1 > total) target = total - a->size + 1;
    for (int steps = 0; a->rank != target && steps < DD_RANKS * 2; steps++) {
        int step = target > a->rank ? 1 : -1;
        int other_rank = step > 0 ? a->rank + a->size : a->rank - 1;
        int o = battle_actor_at(b, a->side, other_rank);
        if (o < 0) break;
        Actor *oa = &b->actors[o];
        if (step > 0) {
            oa->rank = a->rank;
            a->rank = (uint8_t)(oa->rank + oa->size);
        } else {
            a->rank = oa->rank;
            oa->rank = (uint8_t)(a->rank + a->size);
        }
    }
    battle_event(b, EV_MOVE, actor, -1, a->rank, -1);
}

/* A hero's voluntary "move" action: shift to target_rank. Costs the turn
 * (battle_pass ends it). Afflicted heroes may refuse. */
void battle_move(Battle *b, int actor, int target_rank) {
    Actor *a = &b->actors[actor];
    if (stress_refuses(b, actor, 1)) { /* afflicted heroes may refuse to move */
        battle_pass(b, actor);
        return;
    }
    battle_shift(b, actor, target_rank - a->rank);
    battle_pass(b, actor);
}

/* ---------------------------------------------------------------- death */

/* Puts a captured hero back at the end of the party line. */
/* Captors (Hag's cauldron, Bosun's anchor...) hold a hero off the line. Letting
 * go puts the hero at the back and reverts the monster to its empty form.
 * Puts a captured hero back at the end of the party line. */
static void release_prisoner(Battle *b, int captor) {
    Actor *c = &b->actors[captor];
    int h = c->prisoner;
    if (h < 0) return;
    Actor *hero = &b->actors[h];
    c->prisoner = -1;
    hero->captured_by = -1;
    if (hero->alive) {
        int last = 0;
        for (int i = 0; i < MAX_ACTORS; i++) {
            const Actor *o = &b->actors[i];
            if (o->used && o->alive && o->side == SIDE_HEROES && o->rank > 0) last = o->rank + o->size - 1 > last ? o->rank + o->size - 1 : last;
        }
        hero->rank = (uint8_t)(last + 1);
        battle_event(b, EV_RELEASED, h, captor, 0, -1);
    }
    /* the captor goes back to its empty form */
    const DDMonsterClass *m = &dd_monsters[c->cls];
    if (c->alive && m->captor_empty >= 0) c->cls = c->look = m->captor_empty;
}

/* End-of-fight test: no monsters left => heroes win (over=1); no heroes left
 * (captured heroes count as alive) => defeat (over=2). */
static void check_over(Battle *b) {
    if (b->over) return;
    if (!side_alive(b, SIDE_MONSTERS, 0)) {
        b->over = 1;
        battle_event(b, EV_BATTLE_WON, -1, -1, 0, -1);
    } else if (!side_alive(b, SIDE_HEROES, 1)) {
        b->over = 2;
        battle_event(b, EV_BATTLE_LOST, -1, -1, 0, -1);
    }
}

/* Actor idx dies. For heroes: the roster record is marked dead. For monsters
 * this is where the interesting boss rules live, in order:
 *   1. TRANSFORM monsters (the Ancestor) become their next form instead
 *   2. death_damage: some deaths hurt another monster
 *   3. shared-health pool: the whole body dies at once
 *   4. life link: when the last of a class dies, linked monsters die too
 *   5. a corpse (blocking monster) may be left in the same ranks
 * Finally survivors close ranks and we check whether the fight ended. */
void battle_kill(Battle *b, int idx, int attacker) {
    Actor *a = &b->actors[idx];
    if (!a->alive) return;
    a->alive = 0;
    battle_event(b, EV_DEATH, idx, attacker, 0, -1);
    for (int i = 0; i < MAX_ACTORS; i++)
        if (b->actors[i].guarded_by == idx) b->actors[i].guarded_by = -1;
    if (a->prisoner >= 0) release_prisoner(b, idx);
    if (a->captured_by >= 0) b->actors[a->captured_by].prisoner = -1;
    if (a->is_hero) {
        a->hero->alive = 0;
        a->hero->hp = 0;
        stress_on_hero_death(b, idx);
    } else {
        stress_on_kill(b, attacker, idx);
        const DDMonsterClass *m = &dd_monsters[a->cls];
        const char *base = m->sprite;
        int transformed = 0;
        /* the new form may reuse this slot: keep what the rest of the death needs */
        int pool = a->pool, was_corpse = a->is_corpse, crit_kill = a->killed_by_crit, side = a->side;
        if (m->corpse >= 0 && (m->flags2 & MF2_TRANSFORM) && !b->over) {
            /* death_class with can_die_from_damage false: it changes form instead of dying
             * (Ancestor -> grown Ancestor -> gestating heart -> Heart of Darkness) */
            float frac = a->max_hp > 0 ? (float)a->hp / a->max_hp : 0;
            int rank = a->rank;
            int c = battle_add_monster(b, m->corpse, rank);
            if (c >= 0) {
                Actor *n = &b->actors[c];
                if (m->carry_hp > 0) { /* carry_over_hp_min_percent (Hypothesis: max of the HP left and this) */
                    float f = frac > m->carry_hp ? frac : m->carry_hp;
                    n->hp = (int16_t)(n->max_hp * f + 0.5f);
                }
                battle_event(b, EV_SHAPESHIFT, c, idx, 0, -1);
                for (int e = 0; e < m->death_eff_count; e++)
                    effects_apply(b, dd_skill_effects[m->death_eff_first + e], c, c, 1, 0);
                transformed = 1;
            }
        }
        /* death_damage: a flawed reflection's death wounds the Ancestor */
        if (m->death_dmg_base)
            for (int i = 0; i < MAX_ACTORS; i++) {
                Actor *o = &b->actors[i];
                if (o->used && o->alive && !o->is_hero && strcmp(dd_monsters[o->cls].sprite, m->death_dmg_base) == 0)
                    battle_damage(b, i, m->death_dmg, DEATH_DAMAGE, 0);
            }
        /* shared health: the whole body dies together */
        if (pool >= 0)
            for (int i = 0; i < MAX_ACTORS; i++)
                if (i != idx && b->actors[i].used && b->actors[i].alive && b->actors[i].pool == pool) {
                    b->actors[i].alive = 0;
                    battle_event(b, EV_DEATH, i, attacker, 0, -1);
                }
        /* life link: when the last of a base class dies, everything linked to it dies too */
        if (!count_base(b, base))
            for (int i = 0; i < MAX_ACTORS; i++) {
                Actor *o = &b->actors[i];
                const char *link = o->used && o->alive && !o->is_hero ? dd_monsters[o->cls].life_link : NULL;
                if (link && strcmp(link, base) == 0) battle_kill(b, i, attacker);
            }
        int corpse = m->corpse;
        int no_corpse = crit_kill && (m->flags & MF_NO_CORPSE_ON_CRIT);
        if (corpse >= 0 && !was_corpse && !no_corpse && !b->over && !transformed) {
            /* corpse takes the same ranks; it blocks until destroyed */
            int rank = a->rank, dead_cls = a->cls; /* the corpse may reuse this very slot */
            int c = battle_add_monster(b, corpse, rank);
            if (c >= 0) {
                b->actors[c].look = (int16_t)dead_cls;
                battle_event(b, EV_CORPSE, c, -1, 0, -1);
                check_over(b);
                return;
            }
        }
        compact_side(b, side);
        check_over(b);
        return;
    }
    compact_side(b, a->side);
    check_over(b);
}

/* Apply `amount` HP damage. Core rule for HEROES: hitting 0 HP doesn't kill -
 * they go to DEATH'S DOOR (deaths_door=1, with a penalty buff set). Any
 * further damage there rolls a "deathblow" resist chance; failing it kills.
 * Monsters just die at 0. Shared pools lose HP together.
 * Applies HP damage with death's door rules for heroes. */
void battle_damage(Battle *b, int idx, int amount, int attacker, int is_dot) {
    Actor *a = &b->actors[idx];
    if (!a->alive || amount <= 0) return;
    /* the Ancestor only loses HP when his flawed reflections die (death_damage) */
    if (!a->is_hero && (dd_monsters[a->cls].flags2 & MF2_NO_DIRECT_DAMAGE) && attacker != DEATH_DAMAGE) return;
    b->damaged[idx] = 1;
    if (a->pool >= 0) {
        /* shared health pool: every part shows and loses the same HP */
        int p = a->pool;
        b->pool_hp[p] = (int16_t)(b->pool_hp[p] - amount);
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].used && b->actors[i].pool == p) b->actors[i].hp = b->pool_hp[p] > 0 ? b->pool_hp[p] : 0;
        if (b->pool_hp[p] <= 0) battle_kill(b, idx, attacker);
        return;
    }
    if (a->is_hero && a->deaths_door) {
        if (rng_chance(&b->rng, f_deathblow_chance(a))) {
            battle_kill(b, idx, attacker);
        } else {
            battle_event(b, EV_DEATHBLOW_RESISTED, idx, attacker, 0, -1);
        }
        return;
    }
    a->hp -= amount;
    if (a->hp > 0) return;
    a->hp = 0;
    if (a->is_hero) {
        a->deaths_door = 1;
        effects_apply_deaths_door(b, idx);
        battle_event(b, EV_DEATHS_DOOR, idx, attacker, 0, -1);
        stress_on_deaths_door(b, idx);
        /* some captors let go of a prisoner at death's door */
        if (a->captured_by >= 0 && dd_monsters[b->actors[a->captured_by].cls].captor_release_dd)
            release_prisoner(b, a->captured_by);
    } else {
        battle_kill(b, idx, attacker);
    }
    (void)is_dot;
}

/* Heal, scaled by the target's heal-received stat. Any heal above 0 HP pulls a
 * hero back from death's door (and removes its penalties). */
void battle_heal(Battle *b, int idx, int amount) {
    Actor *a = &b->actors[idx];
    if (!a->alive || amount <= 0) return;
    float mul = 1 + actor_stat(a, STAT_HEAL_RECEIVED);
    amount = (int)(amount * mul + 0.5f);
    if (a->pool >= 0) {
        int p = a->pool;
        b->pool_hp[p] = (int16_t)(b->pool_hp[p] + amount > b->pool_max[p] ? b->pool_max[p] : b->pool_hp[p] + amount);
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].used && b->actors[i].pool == p) b->actors[i].hp = b->pool_hp[p];
        battle_event(b, EV_HEAL, idx, -1, amount, -1);
        return;
    }
    a->hp += amount;
    if (a->hp > a->max_hp) a->hp = a->max_hp;
    if (a->deaths_door && a->hp > 0) {
        a->deaths_door = 0;
        effects_leave_deaths_door(b, idx);
    }
    battle_event(b, EV_HEAL, idx, -1, amount, -1);
}

/* ---------------------------------------------------------------- captors */

/* A captor monster seizes a hero: the hero leaves the line (rank 0) and the
 * monster changes to its "full" form. The captor must belong to the same
 * boss group as the performer (captor_for). */
void battle_capture(Battle *b, int performer, int hero) {
    Actor *h = &b->actors[hero];
    if (!h->is_hero || !h->alive || h->captured_by >= 0) return;
    const char *pbase = base_of(&b->actors[performer]);
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor *c = &b->actors[i];
        if (!c->used || !c->alive || c->is_hero || c->prisoner >= 0) continue;
        const DDMonsterClass *m = &dd_monsters[c->cls];
        if (!m->captor_for || m->captor_full < 0 || strcmp(m->captor_for, pbase) != 0) continue;
        c->cls = c->look = m->captor_full;
        c->prisoner = (int8_t)hero;
        h->captured_by = (int8_t)i;
        h->guarded_by = -1;
        h->rank = 0; /* off the battle line */
        compact_side(b, SIDE_HEROES);
        battle_event(b, EV_CAPTURED, hero, i, 0, -1);
        return;
    }
}

/* Mind control (Siren): hero fights for the monsters for `rounds` rounds. */
void battle_control(Battle *b, int hero, int rounds) {
    Actor *h = &b->actors[hero];
    if (!h->is_hero || !h->alive) return;
    h->controlled = (uint8_t)rounds;
    battle_event(b, EV_CONTROLLED, hero, -1, rounds, -1);
}

/* ---------------------------------------------------------------- rounds and turns */

/* ---------------------------------------------------------------- bonus actions
 * Bosses can act OUTSIDE the normal initiative order (e.g. a reaction after
 * an ally is hurt). check_bonus decides which fire; push_bonus queues them;
 * battle_begin_turn drains the queue before the next regular actor. */

/* Living monsters, and (via *size) the number of ranks they fill. */
static int monster_count(const Battle *b, int *size) {
    int n = 0, sz = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (a->used && a->alive && !a->is_hero) { n++; sz += a->size; }
    }
    if (size) *size = sz;
    return n;
}

/* Queue a bonus action (at most one per actor); `front` = run it next. */
static void push_bonus(Battle *b, int actor, int skill, int front) {
    if (b->nbonus >= 8) return;
    for (int i = 0; i < b->nbonus; i++)
        if (b->bonus_actor[i] == actor) return;
    if (front) {
        memmove(&b->bonus_actor[1], &b->bonus_actor[0], b->nbonus);
        memmove(&b->bonus_skill[1], &b->bonus_skill[0], b->nbonus * sizeof(int16_t));
        b->bonus_actor[0] = (int8_t)actor;
        b->bonus_skill[0] = (int16_t)skill;
    } else {
        b->bonus_actor[b->nbonus] = (int8_t)actor;
        b->bonus_skill[b->nbonus] = (int16_t)skill;
    }
    b->nbonus++;
}

/* AI::MonsterBonusInitiativeDesire: queue extra actions whose timing and condition match. */
static void check_bonus(Battle *b, int when) {
    int size, count = monster_count(b, &size);
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor *a = &b->actors[i];
        if (!a->used || !a->alive || a->is_hero || a->prisoner >= 0) continue;
        int brain = dd_monsters[a->cls].brain;
        if (brain < 0) continue;
        const DDBrain *br = &dd_brains[brain];
        for (int d = 0; d < br->bonus_count; d++) {
            const DDBonusDesire *bd = &dd_bonus_desires[br->bonus_first + d];
            if (!(bd->when & when)) continue;
            if (bd->ignore_if_stun && a->stunned) continue;
            if (count < bd->mon_min || count > bd->mon_max) continue;
            if (bd->size_limit && size > bd->size_limit) continue;
            int ok = 0;
            switch (bd->type) {
            case BI_GUARANTEED: ok = 1; break;
            case BI_LAST_SKILL:
                ok = b->last_skill >= 0 && b->last_performer >= 0 && bd->trigger &&
                     b->actors[b->last_performer].side == a->side && strcmp(dd_skills[b->last_skill].id, bd->trigger) == 0;
                break;
            case BI_ALLY_DAMAGED:
                for (int k = 0; k < MAX_ACTORS && !ok; k++)
                    ok = b->damaged[k] && k != i && !b->actors[k].is_hero && bd->ally && strcmp(base_of(&b->actors[k]), bd->ally) == 0;
                break;
            case BI_ALLY_COUNT: {
                int n = bd->ally ? count_base(b, bd->ally) : 0;
                ok = n >= bd->ally_min && n <= bd->ally_max;
                break;
            }
            }
            if (ok) {
                int sk = -1; /* the override skill of this very variant */
                const DDMonsterClass *mc = &dd_monsters[a->cls];
                for (int k = 0; bd->skill && k < mc->skill_count; k++)
                    if (strcmp(dd_skills[mc->skill_first + k].id, bd->skill) == 0) sk = mc->skill_first + k;
                battle_event(b, EV_BONUS_TURN, i, -1, 0, sk);
                push_bonus(b, i, sk, when == BW_ROUND_START);
                break;
            }
        }
    }
}

/* ---------------------------------------------------------------- round upkeep
 * Things that happen once per round before anyone acts: captor damage,
 * shapeshifting, companion buffs. */

static void round_upkeep(Battle *b) {
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor *a = &b->actors[i];
        if (!a->used || !a->alive || a->is_hero) continue;
        const DDMonsterClass *m = &dd_monsters[a->cls];
        /* captors hurt their prisoner every round */
        if (a->prisoner >= 0 && m->captor_damage > 0) {
            Actor *p = &b->actors[a->prisoner];
            int dmg = (int)(p->max_hp * m->captor_damage + 0.999f);
            battle_event(b, EV_HIT, a->prisoner, i, dmg, -1);
            battle_damage(b, a->prisoner, dmg, i, 1);
        }
        /* shape shifters change form (formless flesh) */
        if (m->shift_count && m->shift_every && (b->round - 1) % m->shift_every == 0) {
            float total = 0;
            for (int k = 0; k < m->shift_count; k++)
                if (m->shift_ranks[k] & (1 << (a->rank - 1))) total += m->shift_chance[k];
            float x = rng_float(&b->rng) * total;
            for (int k = 0; k < m->shift_count && total > 0; k++) {
                if (!(m->shift_ranks[k] & (1 << (a->rank - 1)))) continue;
                if (x < m->shift_chance[k]) {
                    if (m->shift_class[k] != a->cls) {
                        a->cls = a->look = m->shift_class[k];
                        battle_event(b, EV_SHAPESHIFT, i, -1, 0, -1);
                    }
                    break;
                }
                x -= m->shift_chance[k];
            }
            m = &dd_monsters[a->cls];
        }
        /* companions: buffs while the partner stands, plus a little healing */
        if (m->companion) {
            int with = count_base(b, m->companion) > 0;
            if (with && !a->companion_on) {
                for (int k = 0; k < m->companion_buff_count; k++) {
                    const DDBuff *bf = &dd_buffs[dd_buff_list[m->companion_buff_first + k]];
                    effects_add_buff(a, bf->stat, bf->sub, bf->mode, bf->amount, -1, 5);
                }
                a->companion_on = 1;
            } else if (!with && a->companion_on) {
                int n = 0;
                for (int k = 0; k < a->nbuffs; k++)
                    if (a->buffs[k].from_effect != 5) a->buffs[n++] = a->buffs[k];
                a->nbuffs = n;
                a->companion_on = 0;
            }
            if (with && m->companion_heal > 0) battle_heal(b, i, (int)(a->max_hp * m->companion_heal + 0.5f));
        }
    }
}

/* SURPRISE: at fight start one side may be caught off guard. Surprised monsters
 * act after every hero in round 1; surprised heroes have their formation
 * shuffled. Light level, scouting and buffs shift the odds (constants below). */
#define SURPRISE_BASE 0.10f    /* base chance each side is caught off guard (wiki Surprise) */
#define SURPRISE_SCOUTED 0.25f /* monsters' base chance in a scouted area; the party can't be surprised */
#define SURPRISE_MAX 0.65f     /* cap for both sides */

static float surprise_clamp(float c) { return c < 0 ? 0 : c > SURPRISE_MAX ? SURPRISE_MAX : c; }

/* Decide who (if anyone) is surprised. First gather what the monster set allows
 * (can surprise / can be surprised / always) and party buff sums, then roll
 * monsters-surprised first, else heroes-surprised. forced<0 disables it. */
int battle_roll_surprise(Battle *b, int scouted, int forced) {
    int can = 0, can_be = 0, always = 0;
    float party = 0, foes = 0; /* summed party_surprise_chance / monsters_surprise_chance buffs */
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (!a->used || !a->alive) continue;
        if (a->is_hero) {
            party += actor_sub_stat(a, STAT_SURPRISE, 0);
            foes += actor_sub_stat(a, STAT_SURPRISE, 1);
        } else {
            uint8_t f = dd_monsters[a->cls].flags;
            can |= (f & MF_CAN_SURPRISE) != 0;
            can_be |= (f & MF_CAN_BE_SURPRISED) != 0;
            always |= (f & MF_ALWAYS_SURPRISE) != 0;
        }
    }
    b->surprise = SURPRISE_NONE;
    if (forced < 0) return SURPRISE_NONE;
    if (forced == SURPRISE_HEROES || always) {
        b->surprise = SURPRISE_HEROES;
    } else {
        float light_m = f_surprise_light(b->torch, 0), light_h = f_surprise_light(b->torch, 1);
        if (can_be && rng_chance(&b->rng, surprise_clamp((scouted ? SURPRISE_SCOUTED : SURPRISE_BASE) + light_m + foes)))
            b->surprise = SURPRISE_MONSTERS;
        else if (can && !scouted && rng_chance(&b->rng, surprise_clamp(SURPRISE_BASE + light_h + party)))
            b->surprise = SURPRISE_HEROES;
    }
    if (b->surprise == SURPRISE_HEROES) {
        /* the formation is thrown into disarray: shuffle the heroes' ranks */
        int idx[DD_RANKS], n = 0;
        for (int i = 0; i < MAX_ACTORS && n < DD_RANKS; i++)
            if (b->actors[i].used && b->actors[i].alive && b->actors[i].is_hero) idx[n++] = i;
        for (int k = n - 1; k > 0; k--) {
            int j = rng_range(&b->rng, 0, k);
            uint8_t t = b->actors[idx[k]].rank;
            b->actors[idx[k]].rank = b->actors[idx[j]].rank;
            b->actors[idx[j]].rank = t;
        }
    }
    return b->surprise;
}

/* corpse life_time .alive_round_limit 3: "a corpse will disappear on its own at the end of the
 * fourth round since its appearance" (wiki Corpse); the ranks behind it close up */
#define CORPSE_ROUNDS 3
/* Remove corpses that have lasted long enough; ranks behind close up. */
static void rot_corpses(Battle *b) {
    int gone = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor *a = &b->actors[i];
        if (a->used && a->alive && a->is_corpse && b->round >= a->born_round + CORPSE_ROUNDS) {
            a->alive = 0;
            battle_event(b, EV_DEATH, i, -1, 0, -1);
            gone = 1;
        }
    }
    if (gone) {
        compact_side(b, SIDE_MONSTERS);
        check_over(b);
    }
}

/* Stall penalty (Battle::GetIsStallPenaltyEnabled / UpdateStallPenalty / PerformStallEffects,
 * decomp9; rules.json stall_*; wiki Stalling): a round with <= 2 counted enemies left, at least 3
 * heroes, an enemy the DoTs won't finish, and fewer than 2 active hero skills is a stalling round.
 * 3 in a row (one more if an enemy is 2+ ranks wide): party stress (effect stall_stress, 5);
 * the 4th: reinforcements from the dungeon's "stall" mash, then the count resets. When every
 * enemy left has accelerate_stall_penalty, the 1st round warns and the 2nd summons. */
#define STALL_PARTY 3
#define STALL_ENEMIES 2
#define STALL_SKILLS 2
#define STALL_STRESS_ROUND 3
#define STALL_SUMMON_ROUND 4
#define STALL_STRESS 5
/* Called once per round end. Counts consecutive "stalling" rounds and applies
 * the escalating penalty described above. */
static void stall_check(Battle *b) {
    int heroes = 0, enemies = 0, tough = 0, off = 0, fast = 1, wide = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (!a->used || !a->alive) continue;
        if (a->is_hero) { heroes++; continue; }
        const DDMonsterClass *m = &dd_monsters[a->cls];
        if (m->flags2 & MF2_NO_STALL) off = 1;
        if (a->is_corpse || (m->flags2 & MF2_STALL_SKIP)) continue;
        enemies++;
        if (!(m->flags2 & MF2_STALL_FAST)) fast = 0;
        if (a->size >= 2) wide = 1;
        int dots = 0;
        for (int k = 0; k < MAX_DOTS; k++) dots += (a->bleed[k].rounds > 0 ? a->bleed[k].amount * a->bleed[k].rounds : 0) +
                                                (a->blight[k].rounds > 0 ? a->blight[k].amount * a->blight[k].rounds : 0);
        if (dots < a->hp) tough = 1;
    }
    int stalling = !off && b->dungeon >= 0 && heroes >= STALL_PARTY && enemies > 0 && enemies <= STALL_ENEMIES && tough &&
                   b->active_skills < STALL_SKILLS;
    b->active_skills = 0;
    if (!stalling) {
        b->stall_rounds = 0;
        return;
    }
    b->stall_rounds++;
    int n = b->stall_rounds - (wide ? 1 : 0);
    int summon = fast ? b->stall_rounds >= 2 : n >= STALL_SUMMON_ROUND;
    if (summon) {
        int enc = dungeon_pick_encounter(&b->rng, b->dungeon, b->level, ENC_STALL);
        b->stall_rounds = 0;
        if (enc < 0) return;
        battle_event(b, EV_STALL, -1, -1, 2, -1);
        const DDEncounter *e = &dd_encounters[enc];
        for (int k = 0; k < e->count; k++) { /* into the free ranks behind the survivors */
            int used = 0;
            for (int i = 0; i < MAX_ACTORS; i++)
                if (b->actors[i].used && b->actors[i].alive && b->actors[i].side == SIDE_MONSTERS) used += b->actors[i].size;
            int size = dd_monsters[e->monster[k]].size ? dd_monsters[e->monster[k]].size : 1;
            if (used + size > DD_RANKS) break;
            int m = battle_add_monster(b, e->monster[k], used + 1);
            if (m >= 0) {
                b->actors[m].no_loot = 1; /* stall_summon: summon_can_spawn_loot false */
                battle_event(b, EV_SUMMON, m, -1, 0, -1);
            }
        }
    } else if (fast) {
        battle_event(b, EV_STALL, -1, -1, 0, -1);
    } else if (n >= STALL_STRESS_ROUND) {
        battle_event(b, EV_STALL, -1, -1, 1, -1);
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].used && b->actors[i].alive && b->actors[i].is_hero) stress_apply_effect(b, -1, i, STALL_STRESS, 0);
    }
}

/* Begin a new round: rot corpses / check stalling, roll initiative for every
 * acting actor (a monster with 2 turns per round appears twice), sort
 * highest-first, then run upkeep and "round start" bonus actions. */
static void start_round(Battle *b) {
    if (b->round > 0) {
        rot_corpses(b);
        stall_check(b);
    }
    b->round++;
    for (int i = 0; i < MAX_ACTORS; i++) b->actors[i].round_turn = 0;
    b->norder = 0;
    b->round_finished = 0;
    int key[MAX_ACTORS * 2];
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor *a = &b->actors[i];
        if (!a->used || !a->alive) continue;
        /* corpses and structures (0 turns per round) don't act on their own */
        if (a->is_corpse) continue;
        int turns = a->is_hero ? 1 : dd_monsters[a->cls].turns_per_round;
        for (int t = 0; t < turns && b->norder < MAX_ACTORS * 2; t++) {
            key[b->norder] = f_initiative(b, a);
            b->order[b->norder++] = i;
        }
    }
    /* surprised monsters go after the slowest hero in the first round (Battle::RollCampaignInitiative) */
    if (b->round == 1 && b->surprise == SURPRISE_MONSTERS) {
        int slowest = 1 << 30;
        for (int i = 0; i < b->norder; i++)
            if (b->actors[b->order[i]].is_hero && key[i] < slowest) slowest = key[i];
        for (int i = 0; i < b->norder && slowest < (1 << 30); i++) {
            const Actor *a = &b->actors[b->order[i]];
            if (!a->is_hero && (dd_monsters[a->cls].flags & MF_CAN_BE_SURPRISED) && key[i] >= slowest)
                key[i] = slowest - (1 << 20) + key[i]; /* keeps their own order */
        }
    }
    /* highest initiative first (insertion sort; at most a dozen entries) */
    for (int i = 1; i < b->norder; i++) {
        int k = key[i], o = b->order[i], j = i - 1;
        while (j >= 0 && key[j] < k) {
            key[j + 1] = key[j];
            b->order[j + 1] = b->order[j];
            j--;
        }
        key[j + 1] = k;
        b->order[j + 1] = o;
    }
    b->cursor = 0;
    round_upkeep(b);
    check_bonus(b, BW_ROUND_START);
}

/* Advance to the next actor who can actually act, returning its index (or -1
 * when the battle is over). Loops because many actors lose their turn:
 *   queued bonus actions run first; at round end we start a new round;
 *   each actor takes DoT damage first (may die); captured/stunned actors
 *   skip; mind-controlled heroes act automatically; stress may make a hero
 *   refuse. `guard` caps the loop so a bug can never hang the game. */
int battle_begin_turn(Battle *b) {
    b->forced_skill = -1;
    for (int guard = 0; guard < 64; guard++) {
        if (b->over) return -1;
        /* reactions to the last action (cannon after the fuse, prince when the piglet is hurt) */
        if (b->acted) {
            b->acted = 0;
            check_bonus(b, BW_POST_TURN);
            memset(b->damaged, 0, sizeof(b->damaged));
        }
        if (b->nbonus) {
            int idx = b->bonus_actor[0], skill = b->bonus_skill[0];
            b->nbonus--;
            memmove(&b->bonus_actor[0], &b->bonus_actor[1], b->nbonus);
            memmove(&b->bonus_skill[0], &b->bonus_skill[1], b->nbonus * sizeof(int16_t));
            Actor *a = &b->actors[idx];
            if (!a->used || !a->alive || a->stunned) continue;
            b->current = idx;
            b->forced_skill = skill;
            battle_event(b, EV_TURN, idx, -1, b->round, -1);
            return idx;
        }
        if (b->cursor >= b->norder) {
            if (b->round > 0 && !b->round_finished) {
                b->round_finished = 1;
                check_bonus(b, BW_ROUND_FINISH);
                if (b->nbonus) continue;
            }
            if (b->round > 0) stress_on_round_end(b);
            start_round(b);
            if (b->norder == 0 && b->nbonus == 0) return -1;
            continue;
        }
        int idx = b->order[b->cursor++];
        Actor *a = &b->actors[idx];
        if (!a->used || !a->alive) continue;
        b->current = idx;
        battle_event(b, EV_TURN, idx, -1, b->round, -1);

        effects_tick_dots(b, idx);
        if (!a->alive || b->over) continue;

        if (a->captured_by >= 0) { /* held in a cauldron or anchor: no turn */
            effects_end_turn(b, idx);
            continue;
        }
        if (a->stunned) {
            a->stunned = 0;
            effects_stun_recovery(b, idx);
            battle_event(b, EV_STUN_SKIP, idx, -1, 0, -1);
            effects_end_turn(b, idx);
            continue;
        }
        if (a->is_hero && a->controlled) {
            /* the controller's song costs the hero their sanity */
            for (int i = 0; i < MAX_ACTORS; i++)
                if (b->actors[i].used && b->actors[i].alive && !b->actors[i].is_hero && dd_monsters[b->actors[i].cls].control_stress) {
                    stress_apply_effect(b, i, idx, dd_monsters[b->actors[i].cls].control_stress, 0);
                    break;
                }
            if (!a->alive || b->over) continue;
            return idx; /* the caller lets battle_hero_auto act for them */
        }
        if (a->is_hero && stress_turn_start(b, idx)) {
            effects_end_turn(b, idx);
            continue;
        }
        a->perf_turn++; /* Actor::IncrementPerformingTurn */
        a->round_turn++;
        return idx;
    }
    return -1;
}

/* A hero acting on their own (mind control): pick a random usable skill and a
 * random valid target, trying random ranks a few times. */
void battle_hero_auto(Battle *b, int actor) {
    int opts[HERO_SKILL_SLOTS], n = 0;
    for (int s = 0; s < HERO_SKILL_SLOTS; s++) {
        int sk = battle_hero_skill(b, actor, s);
        if (sk >= 0 && battle_skill_usable(b, actor, sk)) opts[n++] = sk;
    }
    if (!n) {
        battle_pass(b, actor);
        return;
    }
    int sk = opts[rng_range(&b->rng, 0, n - 1)];
    int mask = battle_targets(b, actor, sk, NULL), rank = 1;
    for (int k = 0; k < 8; k++) {
        int r = rng_range(&b->rng, 1, DD_RANKS);
        if (mask & (1 << (r - 1))) { rank = r; break; }
    }
    battle_perform(b, actor, sk, rank);
}

/* End the actor's turn without (further) acting. Also the common ending of
 * every action - effects_end_turn ticks buff durations. */
void battle_pass(Battle *b, int actor) {
    b->last_skill = -1;
    b->acted = 1;
    if (b->actors[actor].is_hero) stress_on_pass(b, actor);
    effects_end_turn(b, actor);
    b->current = -1;
}

/* ---------------------------------------------------------------- performing skills */

static void counter_attack(Battle *b, int defender, int attacker);

/* Resolve one attack against a list of targets. Per target:
 *   roll to hit (accuracy vs dodge) -> maybe crit -> compute damage ->
 *   emit hit event -> apply damage -> stress effects -> hit-reaction effects.
 * Instant buffs from the skill are pushed temporarily around the numbers
 * (effects_push_instant) and popped (`a->nbuffs = restore`). */
static void do_attack(Battle *b, int actor, int skill, int *targets, int ntargets, int allow_riposte) {
    Actor *a = &b->actors[actor];
    const DDSkill *s = &dd_skills[skill];
    int any_hit = 0;
    /* heroes roll hit/crit per target; monsters roll once for all targets (GenerateAttack) */
    float shared_roll = rng_float(&b->rng);
    for (int k = 0; k < ntargets; k++) {
        int t = targets[k];
        Actor *ta = &b->actors[t];
        if (!ta->alive) continue;
        float roll = a->is_hero ? rng_float(&b->rng) : shared_roll;
        int restore = effects_push_instant(b, skill, actor, t);
        int hit = f_roll_hit(b, a, s, ta, roll);
        /* quirk "fated": a miss may turn into a hit (Attack::AddTarget 0xedaef0; chance: Hypothesis) */
        if (!hit && a->hero && quirk_fated() >= 0 && quirk_has(a->hero, quirk_fated()) && rng_chance(&b->rng, FATED_CHANCE)) hit = 1;
        int crit = 0;
        if (hit && (s->flags & SKF_CRIT_VALID)) crit = rng_float(&b->rng) < f_crit_chance(b, a, s, ta);
        int dmg = hit && (s->dmg_lo || s->dmg_hi || a->is_hero) ? f_damage(b, a, s, ta, crit) : 0;
        a->nbuffs = restore;
        if (hit) {
            any_hit = 1;
            if (s->type == SK_SUPPORT) dmg = 0;
            if (!ta->is_hero && (dd_monsters[ta->cls].flags2 & MF2_NO_DIRECT_DAMAGE)) dmg = 0;
            battle_event(b, crit ? EV_CRIT : EV_HIT, t, actor, dmg, skill);
            ta->killed_by_crit = (uint8_t)crit;
            if (dmg > 0) {
                b->turns_without_damage = 0;
                battle_damage(b, t, dmg, actor, 0);
            }
            stress_on_hit(b, actor, t, crit);
            /* skill_reaction: hitting it does something to the attacker (the gestating heart) */
            if (!ta->is_hero && a->side != ta->side) {
                const DDMonsterClass *tm = &dd_monsters[ta->cls];
                for (int i = 0; i < tm->react_eff_count && a->alive; i++)
                    effects_apply(b, dd_skill_effects[tm->react_eff_first + i], t, actor, 1, 0);
            }
            if (crit && a->is_hero) {
                const DDHeroClass *h = &dd_heroes[a->cls];
                for (int i = 0; i < h->crit_eff_count; i++)
                    effects_apply(b, dd_skill_effects[h->crit_eff_first + i], actor, actor, 1, 0);
            }
        } else {
            battle_event(b, EV_MISS, t, actor, 0, skill);
        }
        effects_apply_skill(b, skill, actor, t, hit, crit);
        if (b->over) return;
    }
    /* "teleport" skills (White Cell Stalk): the fight ends and the dungeon moves the party */
    if (s->type == SK_TELEPORT && any_hit && !b->over) {
        b->over = 4;
        battle_event(b, EV_TELEPORT, actor, -1, 0, skill);
        return;
    }
    if (a->is_hero) b->hero_miss_streak = any_hit ? 0 : b->hero_miss_streak + 1;
    if (!any_hit) stress_on_miss(b, actor);

    if (!allow_riposte) return;
    for (int k = 0; k < ntargets && a->alive && !b->over; k++) {
        Actor *ta = &b->actors[targets[k]];
        if (ta->alive && ta->riposte && ta->side != a->side) counter_attack(b, targets[k], actor);
    }
}

/* Riposte: a defender with the riposte status immediately hits back with its
 * riposte skill (allow_riposte=0 stops infinite riposte chains). */
static void counter_attack(Battle *b, int defender, int attacker) {
    Actor *d = &b->actors[defender];
    int skill = d->is_hero ? dd_heroes[d->cls].riposte_skill : dd_monsters[d->cls].riposte_skill;
    if (skill < 0) return;
    const DDSkill *s = &dd_skills[skill];
    if (!(s->target.mask & (1 << (b->actors[attacker].rank - 1)))) return;
    battle_event(b, EV_RIPOSTE, defender, attacker, 0, skill);
    int t[1] = {attacker};
    do_attack(b, defender, skill, t, 1, 0);
}

/* The one entry point for "actor uses skill on the target at target_rank".
 * Records use counts, handles move skills, builds the list of affected
 * targets (single, all-in-mask, extra random targets), runs do_attack, then
 * ends the turn. */
void battle_perform(Battle *b, int actor, int skill, int target_rank) {
    Actor *a = &b->actors[actor];
    const DDSkill *s = &dd_skills[skill];
    int slot = skill_use_slot(a, skill);
    if (slot >= 0 && a->skill_uses[slot] < 255) a->skill_uses[slot]++;
    battle_event(b, EV_SKILL, actor, -1, target_rank, skill);
    b->last_skill = skill;
    b->last_performer = actor;
    b->acted = 1;
    if (a->is_hero && s->type != SK_MOVE && !(s->flags & SKF_NO_STALL_RESET)) b->active_skills++;
    b->summoned = 0;

    if (s->type == SK_MOVE) {
        battle_shift(b, actor, target_rank - a->rank);
        effects_end_turn(b, actor);
        b->current = -1;
        return;
    }

    int side;
    int mask = battle_targets(b, actor, skill, &side);
    int targets[MAX_ACTORS], n = 0;
    if (s->target.flags & RK_SELF) {
        targets[n++] = actor;
    } else if (s->target.flags & RK_ALL) {
        for (int r = 1; r <= DD_RANKS; r++) {
            if (!(mask & (1 << (r - 1)))) continue;
            int t = battle_actor_at(b, side, r);
            int dup = 0;
            for (int k = 0; k < n; k++) dup |= targets[k] == t;
            if (t >= 0 && !dup) targets[n++] = t;
        }
    } else {
        if (!(mask & (1 << (target_rank - 1)))) {
            for (target_rank = 1; target_rank <= DD_RANKS && !(mask & (1 << (target_rank - 1))); target_rank++) {}
        }
        int t = battle_actor_at(b, side, target_rank);
        if (t >= 0) targets[n++] = t;
        /* some monster skills add random extra targets */
        for (int x = 0; x < s->extra_targets && n < DD_RANKS; x++) {
            if (!rng_chance(&b->rng, s->extra_targets_chance)) continue;
            int r = rng_range(&b->rng, 1, DD_RANKS);
            int e = (mask & (1 << (r - 1))) ? battle_actor_at(b, side, r) : -1;
            int dup = 0;
            for (int k = 0; k < n; k++) dup |= targets[k] == e;
            if (e >= 0 && !dup) targets[n++] = e;
        }
    }

    int friendly = side == a->side;
    if (friendly) {
        for (int k = 0; k < n; k++) {
            int crit = 0;
            if (targets[k] != actor && stress_refuses(b, targets[k], s->heal_hi > 0 ? 0 : 2)) continue;
            if (s->heal_hi > 0) {
                crit = (s->flags & SKF_CRIT_VALID) && rng_float(&b->rng) < f_heal_crit_chance(a, n > 1);
                battle_heal(b, targets[k], f_heal(b, a, s, &b->actors[targets[k]], crit));
            }
            effects_apply_skill(b, skill, actor, targets[k], 1, crit);
        }
    } else {
        /* guards take hits meant for their ward */
        if (!(s->flags & SKF_IGNORE_GUARD))
            for (int k = 0; k < n; k++) {
                int g = b->actors[targets[k]].guarded_by;
                if (g >= 0 && b->actors[g].alive) targets[k] = g;
            }
        do_attack(b, actor, skill, targets, n, 1);
    }

    /* self movement after the skill (e.g. Holy Lance moves forward) */
    if (a->alive && (s->move_back || s->move_fwd)) battle_shift(b, actor, s->move_back - s->move_fwd);
    effects_end_turn(b, actor);
    b->current = -1;
}
