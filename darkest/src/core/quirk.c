/*
 * quirk.c - quirk bookkeeping. A hero's quirks are stored as `quirks[13]`
 * holding (dd_quirks index + 1), 0 = empty slot, so 0 never means "quirk 0".
 * `quirk_locked` is a bit per slot: locked quirks cannot be replaced or
 * removed (the sanitarium can pay to un-lock them).
 */
#include "quirk.h"

#include <stdio.h>
#include <string.h>

#include "effects.h"

/* Does the hero have quirk q? */
int quirk_has(const Hero *h, int q) {
    for (int i = 0; i < HERO_QUIRKS; i++)
        if (h->quirks[i] == q + 1) return 1;
    return 0;
}

/* Index of the special "fated" quirk (looked up once, then cached). */
int quirk_fated(void) {
    static int idx = -2;
    if (idx == -2) {
        idx = -1;
        for (int i = 0; i < dd_quirk_count; i++)
            if (!strcmp(dd_quirks[i].id, "fated")) idx = i;
    }
    return idx;
}

/* Number of quirks of a category (positive / negative / disease). */
static int count_kind(const Hero *h, int positive, int disease) {
    int n = 0;
    for (int i = 0; i < HERO_QUIRKS; i++) {
        if (!h->quirks[i]) continue;
        const DDQuirk *q = &dd_quirks[h->quirks[i] - 1];
        n += disease ? q->disease : (!q->disease && q->positive == positive);
    }
    return n;
}

/* Weighted random quirk the hero doesn't already have, from the requested
 * category. Same two-pass roulette as ai.c: sum the weights, roll, walk. */
int quirk_random(Rng *r, const Hero *h, int positive, int disease) {
    float total = 0;
    for (int i = 0; i < dd_quirk_count; i++) {
        const DDQuirk *q = &dd_quirks[i];
        if (q->disease != disease || (!disease && q->positive != positive) || quirk_has(h, i)) continue;
        total += q->weight;
    }
    if (total <= 0) return -1;
    float x = rng_float(r) * total;
    for (int i = 0; i < dd_quirk_count; i++) {
        const DDQuirk *q = &dd_quirks[i];
        if (q->disease != disease || (!disease && q->positive != positive) || quirk_has(h, i)) continue;
        if (x < q->weight) return i;
        x -= q->weight;
    }
    return -1;
}

/* Empty a slot and unlock it. */
void quirk_remove(Hero *h, int slot) {
    if (slot < 0 || slot >= HERO_QUIRKS) return;
    h->quirks[slot] = 0;
    h->quirk_locked &= (uint16_t)~(1u << slot);
}

/* Give a hero quirk q, obeying the rules in order:
 *   1. an INCOMPATIBLE quirk (Tough vs Fragile) is replaced unless locked;
 *   2. if the category is full, a random unlocked one of the same category is
 *      replaced;
 *   3. otherwise take the first free slot.
 * Returns 1 if the quirk was added. */
int quirk_add(Hero *h, int q, Rng *r) {
    if (q < 0 || q >= dd_quirk_count || quirk_has(h, q)) return 0;
    const DDQuirk *nq = &dd_quirks[q];
    /* an incompatible quirk is replaced (e.g. Tough over Fragile), unless locked */
    for (int i = 0; i < HERO_QUIRKS; i++) {
        int have = h->quirks[i] - 1;
        if (have < 0) continue;
        if (have == nq->incompatible[0] || have == nq->incompatible[1]) {
            if (h->quirk_locked & (1u << i)) return 0;
            h->quirks[i] = (uint8_t)(q + 1);
            h->quirk_locked &= (uint16_t)~(1u << i);
            return 1;
        }
    }
    int max = nq->disease ? QUIRK_MAX_DISEASES : nq->positive ? QUIRK_MAX_POSITIVE : QUIRK_MAX_NEGATIVE;
    if (count_kind(h, nq->positive, nq->disease) >= max) {
        int cand[HERO_QUIRKS], n = 0;
        for (int i = 0; i < HERO_QUIRKS; i++) {
            if (!h->quirks[i] || (h->quirk_locked & (1u << i))) continue;
            const DDQuirk *o = &dd_quirks[h->quirks[i] - 1];
            if (o->disease == nq->disease && (nq->disease || o->positive == nq->positive)) cand[n++] = i;
        }
        if (!n) return 0;
        int i = cand[rng_range(r, 0, n - 1)];
        h->quirks[i] = (uint8_t)(q + 1);
        h->quirk_locked &= (uint16_t)~(1u << i);
        return 1;
    }
    for (int i = 0; i < HERO_QUIRKS; i++)
        if (!h->quirks[i]) {
            h->quirks[i] = (uint8_t)(q + 1);
            return 1;
        }
    return 0;
}

/* Sum of a stat from quirk buffs OUTSIDE battle. Only unconditional buffs
 * count, since conditions (like "vs beasts") need a battle context. */
float quirk_stat(const Hero *h, int stat, int sub) {
    float v = 0;
    for (int i = 0; i < HERO_QUIRKS; i++) {
        if (!h->quirks[i]) continue;
        const DDQuirk *q = &dd_quirks[h->quirks[i] - 1];
        for (int k = 0; k < q->buff_count; k++) {
            const DDBuff *b = &dd_buffs[dd_buff_list[q->buff_first + k]];
            int on = b->rule == RULE_ALWAYS || b->rule == RULE_IN_DUNGEON;
            if (b->rule_not) on = !on;
            if (on && b->stat == stat && ((stat != STAT_RES && stat != STAT_EFFECT_CHANCE && stat != STAT_SURPRISE) || b->sub == sub)) v += b->amount;
        }
    }
    return v;
}

/* Copy quirk buffs onto a battle Actor. rule_buff remembers which data buff
 * gates it, so formulas.c re-checks its condition live each time. */
void quirks_to_actor(Actor *a) {
    if (!a->hero) return;
    for (int i = 0; i < HERO_QUIRKS; i++) {
        if (!a->hero->quirks[i]) continue;
        const DDQuirk *q = &dd_quirks[a->hero->quirks[i] - 1];
        for (int k = 0; k < q->buff_count; k++) {
            int bi = dd_buff_list[q->buff_first + k];
            const DDBuff *b = &dd_buffs[bi];
            if (b->stat == STAT_MAX_HP) continue; /* already in hero_max_hp */
            int before = a->nbuffs;
            effects_add_buff(a, b->stat, b->sub, b->mode, b->amount, -1, 0);
            if (a->nbuffs > before) {
                a->buffs[a->nbuffs - 1].rule_buff = (int16_t)bi;
                a->buffs[a->nbuffs - 1].from_effect = 0; /* permanent: clear-debuff and trait swaps keep it */
            }
        }
    }
}

/* After a raid: roll for a new positive and/or negative quirk. Success lowers
 * the negative chance; high stress raises it. Then each negative quirk has a
 * 25% chance to lock. The new names are returned for the results screen. */
void quirks_after_expedition(Hero *h, int success, Rng *r, char *pos_out, char *neg_out, int len) {
    float s = h->stress > 100 ? 100 : h->stress;
    float pos = success ? 0.50f - 0.0010f * s : 0.40f - 0.0015f * s;
    float neg = success ? 0.30f + 0.0020f * s : 0.40f + 0.0030f * s;
    pos_out[0] = neg_out[0] = 0;
    if (rng_chance(r, pos)) {
        int q = quirk_random(r, h, 1, 0);
        if (q >= 0 && quirk_add(h, q, r)) snprintf(pos_out, len, "%s", dd_quirks[q].name);
    }
    if (rng_chance(r, neg)) {
        int q = quirk_random(r, h, 0, 0);
        if (q >= 0 && quirk_add(h, q, r)) snprintf(neg_out, len, "%s", dd_quirks[q].name);
    }
    /* rules.json quirk_chance_to_lock_negative */
    for (int i = 0; i < HERO_QUIRKS; i++) {
        if (!h->quirks[i] || dd_quirks[h->quirks[i] - 1].positive || dd_quirks[h->quirks[i] - 1].disease) continue;
        if (rng_chance(r, 0.25f)) h->quirk_locked |= (uint16_t)(1u << i);
    }
}
