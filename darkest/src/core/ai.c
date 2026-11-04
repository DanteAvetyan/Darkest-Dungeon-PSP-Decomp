/*
 * ai.c - monster decision making.
 *
 * Monsters have no hand-written behaviour.  Each class points at a BRAIN made
 * of "desires" (data from the game's monster_brains file):
 *   SKILL desires: "when condition X holds, add weight W to skill S"
 *   TARGET desires: "prefer a hero who is marked / low HP / in rank 3..."
 * The algorithm is two weighted random draws:
 *   1. every valid skill desire adds weight to its skill; pick a skill with
 *      probability proportional to weight;
 *   2. for that skill, find legal target ranks; every matching target desire
 *      adds weight to that rank; pick a target the same way.
 * If nothing has any weight we fall back to a uniform random choice.
 * The original has more desire types; the common ones are covered here.
 */
#include <stddef.h>
#include <string.h>

#include "battle.h"
#include "effects.h"
#include "stress.h"

/* Roulette-wheel selection: returns index i with probability w[i]/sum(w), or
 * -1 if all weights are <= 0. The second loop is a float-rounding safety net. */
static int pick_weighted(Rng *r, const float *w, int n) {
    float total = 0;
    for (int i = 0; i < n; i++) total += w[i] > 0 ? w[i] : 0;
    if (total <= 0) return -1;
    float x = rng_float(r) * total;
    for (int i = 0; i < n; i++) {
        if (w[i] <= 0) continue;
        if (x < w[i]) return i;
        x -= w[i];
    }
    for (int i = n - 1; i >= 0; i--)
        if (w[i] > 0) return i;
    return -1;
}

/* Is the actor suffering any bleed or blight? */
static int has_dot(const Actor *a) {
    for (int i = 0; i < MAX_DOTS; i++)
        if (a->bleed[i].rounds > 0 || a->blight[i].rounds > 0) return 1;
    return 0;
}

/* Pre-compute headcounts (living heroes, marked heroes, monsters...) once per
 * decision. Desires carry [lo,hi] ranges on these counts, e.g. "only if 2 or
 * fewer monsters remain".
 * Battle counts for IsDesireValid (decomp8 0xe0ad90): inclusive min/max ranges per desire. */
static void battle_counts(const Battle *b, int cnt[DC_COUNT]) {
    memset(cnt, 0, sizeof(int) * DC_COUNT);
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (!a->used || !a->alive) continue;
        if (a->is_hero) {
            cnt[DC_HEROES]++;
            if (a->marked) cnt[DC_MARKED]++;
            else cnt[DC_NON_MARKED]++;
            if (!a->stunned) cnt[DC_NON_STUNNED]++;
            if (!a->deaths_door) cnt[DC_NON_DEATHS_DOOR]++;
            if (a->controlled) cnt[DC_CONTROLLED]++;
            int virtue = 0;
            if (a->hero && stress_affliction_name(a->hero, &virtue) && virtue) cnt[DC_VIRTUED]++;
        } else if (!a->is_corpse) {
            cnt[DC_MONSTERS]++;
            cnt[DC_MONSTERS_SIZE] += a->size;
            if (a->guarded_by >= 0) cnt[DC_GUARDED]++;
        }
    }
}

/* Test every condition on a skill desire: count ranges, which turn of the
 * fight/round it is, whether a named ally is alive, whether an ally is hurt,
 * whether a hero has a key status, whether a captor is free.
 * AI::MonsterSkillSelectionDesire::Functions::*: is this desire in play for the performer? */
static int desire_valid(const Battle *b, int actor, const DDSkillDesire *sd, const int cnt[DC_COUNT]) {
    const Actor *a = &b->actors[actor];
    for (int k = 0; k < DC_COUNT; k++)
        if (cnt[k] < sd->lo[k] || cnt[k] > sd->hi[k]) return 0;
    if (sd->first_only && a->round_turn > 1) return 0;
    if (sd->type == SD_TURN) return a->perf_turn == sd->turn; /* performing_turn: exactly this turn */
    if (sd->turn && a->perf_turn < sd->turn) return 0;       /* performing_turn_min */
    int ally_alive = 0, hurt = 0, key = 0, empty_captor = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *o = &b->actors[i];
        if (!o->used || !o->alive) continue;
        if (!o->is_hero && o->side == a->side) {
            if (sd->ally && i != actor && strncmp(dd_monsters[o->cls].id, sd->ally, strlen(sd->ally)) == 0) ally_alive = 1;
            if (!o->is_corpse && o->max_hp > 0 && (float)o->hp / o->max_hp <= sd->hp_ratio) hurt = 1;
            if (dd_monsters[o->cls].captor_full >= 0 && o->prisoner < 0) empty_captor = 1; /* a captor with room */
        }
        if (o->is_hero && sd->key_status && effects_key_status(o, sd->key_status)) key = 1;
    }
    switch (sd->type) {
    case SD_ALLY_ALIVE: return ally_alive;
    case SD_ALLY_DEAD: return !ally_alive;
    case SD_HEAL: return hurt;
    case SD_KEY_STATUS: return key;
    case SD_FILL_CAPTOR: return empty_captor;
    default: return 1;
    }
}

/* Public entry: fill *skill_out (absolute skill index) and *target_out (rank).
 * Returns 0 if the monster can do nothing (the caller then passes the turn).
 * A "forced" skill (from a boss bonus action) skips step 1. */
int battle_ai_choose(Battle *b, int actor, int *skill_out, int *target_out) {
    const Actor *a = &b->actors[actor];
    const DDMonsterClass *m = &dd_monsters[a->cls];
    int nsk = m->skill_count;
    if (nsk <= 0) return 0;
    if (nsk > 16) nsk = 16;

    float w[16] = {0};
    int usable = 0;
    int forced = b->forced_skill;
    if (forced >= 0 && forced >= m->skill_first && forced < m->skill_first + nsk) {
        /* bonus action with a fixed skill: only the target is chosen */
        int side;
        int mask = battle_targets(b, actor, forced, &side);
        if (dd_skills[forced].target.flags & RK_SELF) mask = 1;
        if (mask || dd_skills[forced].target.mask == 0) {
            int r = 1, opts[DD_RANKS], n = 0;
            for (int k = 0; k < DD_RANKS; k++)
                if (mask & (1 << k)) opts[n++] = k + 1;
            if (n) r = opts[rng_range(&b->rng, 0, n - 1)];
            *skill_out = forced;
            *target_out = r;
            return 1;
        }
    }
    for (int i = 0; i < nsk; i++) usable += battle_skill_usable(b, actor, m->skill_first + i);
    if (!usable) return 0;

    const DDBrain *br = m->brain >= 0 ? &dd_brains[m->brain] : NULL;
    if (br) {
        int cnt[DC_COUNT];
        battle_counts(b, cnt);
        for (int d = 0; d < br->skill_count; d++) {
            const DDSkillDesire *sd = &dd_skill_desires[br->skill_first + d];
            if (!desire_valid(b, actor, sd, cnt)) continue;
            for (int i = 0; i < nsk; i++) {
                int sk = m->skill_first + i;
                if (!battle_skill_usable(b, actor, sk)) continue;
                if (sd->type == SD_PREFERRED) {
                    if (i == m->prefskill) w[i] += sd->chance; /* preferred_skill: the personality's skill */
                } else if (sd->skill && strcmp(dd_skills[sk].id, sd->skill) == 0) w[i] += sd->chance;
                else if (!sd->skill) w[i] += sd->chance / usable;
            }
        }
    }
    int pick = pick_weighted(&b->rng, w, nsk);
    if (pick < 0) {
        for (int i = 0; i < nsk; i++) w[i] = battle_skill_usable(b, actor, m->skill_first + i);
        pick = pick_weighted(&b->rng, w, nsk);
    }
    if (pick < 0) return 0;
    int skill = m->skill_first + pick;
    *skill_out = skill;

    int side;
    int mask = battle_targets(b, actor, skill, &side);
    float tw[DD_RANKS] = {0};
    int tgt[DD_RANKS];
    for (int r = 0; r < DD_RANKS; r++) tgt[r] = (mask & (1 << r)) ? battle_actor_at(b, side, r + 1) : -1;

    if (br) {
        /* lowest-health targets need the minimum first */
        int lowest_hp = -1;
        for (int r = 0; r < DD_RANKS; r++) {
            if (tgt[r] < 0) continue;
            const Actor *t = &b->actors[tgt[r]];
            if (lowest_hp < 0 || t->hp * b->actors[tgt[lowest_hp]].max_hp <
                                         b->actors[tgt[lowest_hp]].hp * t->max_hp)
                lowest_hp = r;
        }
        for (int d = 0; d < br->target_count; d++) {
            const DDTargetDesire *td = &dd_target_desires[br->target_first + d];
            if (td->skill && strcmp(dd_skills[skill].id, td->skill) != 0) continue;
            for (int r = 0; r < DD_RANKS; r++) {
                if (tgt[r] < 0) continue;
                const Actor *t = &b->actors[tgt[r]];
                int match = 0;
                switch (td->type) {
                case TD_RANDOM: match = 1; break;
                case TD_MARKED: match = t->marked > 0; break;
                case TD_HEALTH_LOW: match = r == lowest_hp; break;
                case TD_STUNNED: match = t->stunned; break;
                case TD_RANK: match = (td->param >> r) & 1; break;
                case TD_STRESS: match = t->is_hero && t->hero->stress >= 50; break;
                case TD_DOT: match = has_dot(t); break;
                case TD_GUARD: match = t->guarded_by >= 0; break;
                case TD_RESISTANCE_LOW: {
                    float mine = t->is_hero ? dd_heroes[t->cls].res[td->param] : dd_monsters[t->cls].res[td->param];
                    match = 1;
                    for (int o = 0; o < DD_RANKS; o++) {
                        if (tgt[o] < 0) continue;
                        const Actor *ot = &b->actors[tgt[o]];
                        float theirs = ot->is_hero ? dd_heroes[ot->cls].res[td->param] : dd_monsters[ot->cls].res[td->param];
                        if (theirs < mine) match = 0;
                    }
                    break;
                }
                default: break;
                }
                if (match) tw[r] += td->chance;
            }
        }
    }
    int r = pick_weighted(&b->rng, tw, DD_RANKS);
    if (r < 0) {
        for (int k = 0; k < DD_RANKS; k++) tw[k] = tgt[k] >= 0;
        r = pick_weighted(&b->rng, tw, DD_RANKS);
    }
    *target_out = r < 0 ? 1 : r + 1;
    return 1;
}
