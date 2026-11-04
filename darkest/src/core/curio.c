/*
 * curio.c - resolving curio results and applying effects OUTSIDE battle.
 * Out of battle there is no Battle/Actor, so effects here operate directly on
 * the persistent Hero records (HP, stress, carried buffs, bleed/blight).
 */
#include "curio.h"
#include "loot.h"
#include "quirk.h"
#include "trinket.h"

/* Result list of the most recent curio, read by the UI loot window. */
Loot curio_loot[LOOT_MAX];
int curio_nloot;

#include <stdio.h>
#include <string.h>

#include "campaign.h"
#include "stress.h"

/* Which carried provisions have a special result on this curio (deduplicated). */
int curio_usable_items(const Dungeon *d, int curio, int out[ITEM_COUNT]) {
    const DDCurio *c = &dd_curios[curio];
    int n = 0;
    for (int i = 0; i < c->item_count; i++) {
        int it = dd_curio_results[c->item_first + i].item;
        if (it >= ITEM_COUNT || !d->items[it]) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= out[k] == it;
        if (!dup) out[n++] = it;
    }
    return n;
}

/* Add a buff that travels with the hero between fights ("expedition buff"). */
static void add_ebuff(Hero *h, int stat, int sub, int mode, float amount, int battles) {
    if (h->nebuffs >= HERO_BUFFS || stat == STAT_NONE) return;
    HeroBuff *b = &h->ebuffs[h->nebuffs++];
    b->stat = (uint8_t)stat;
    b->sub = (uint8_t)sub;
    b->mode = (uint8_t)mode;
    b->amount = amount;
    b->battles = (int8_t)battles;
}

/* Out-of-battle damage never kills: HP floors at 1. */
static void hurt(Hero *h, int dmg) {
    h->hp = (int16_t)(h->hp - dmg > 0 ? h->hp - dmg : 1); /* outside battle nobody drops below 1 */
}

/* Add bleed/blight to a hero: fill a free slot, else overwrite the weakest. */
static void add_dot(Dot *d, int amount, int rounds) {
    int k = 0; /* a free slot, else the weakest */
    for (int i = 0; i < HERO_DOTS; i++) {
        if (!d[i].rounds) { k = i; break; }
        if (d[i].amount * d[i].rounds < d[k].amount * d[k].rounds) k = i;
    }
    d[k].amount = (int8_t)amount;
    d[k].rounds = (int8_t)rounds;
}

/* Each new hall square, a bleeding/blighted hero takes one tick of damage. */
int hero_dot_tick(Hero *h) {
    int dmg = 0;
    for (int i = 0; i < HERO_DOTS; i++) {
        if (h->bleed[i].rounds > 0) { dmg += h->bleed[i].amount; h->bleed[i].rounds--; }
        if (h->blight[i].rounds > 0) { dmg += h->blight[i].amount; h->blight[i].rounds--; }
    }
    if (dmg > 0 && h->alive) hurt(h, dmg);
    return dmg;
}

/* The out-of-battle twin of effects_apply(): interpret an effect's ops on
 * the party's Hero records. Supports the ops that make sense in a dungeon
 * hall (stress, heal, DoTs, cures, torch, buffs). Everything else is ignored. */
void hero_apply_effect_offline(Dungeon *d, Hero *party[], int n, int who, int effect, Rng *r) {
    const DDEffect *e = &dd_effects[effect];
    for (int k = 0; k < n; k++) {
        Hero *h = party[k];
        if (!h || !h->alive) continue;
        int all = e->target == ET_GLOBAL || e->target == ET_PERFORMER_GROUP_OTHER || e->target == ET_TARGET_GROUP;
        if (!all && k != who) continue;
        for (int o = 0; o < e->op_count; o++) {
            const DDEffOp *op = &dd_eff_ops[e->op_first + o];
            switch (op->op) {
            case OP_STRESS: stress_add(h, (int)op->v, d->level, rng_u32(r)); break;
            case OP_HEAL_STRESS: stress_add(h, -(int)op->v, d->level, rng_u32(r)); break;
            case OP_HEAL:
                h->hp = (int16_t)(h->hp + (int)op->v > hero_max_hp(h) ? hero_max_hp(h) : h->hp + (int)op->v);
                break;
            case OP_BLEED: add_dot(h->bleed, (int)op->v, e->duration > 0 ? e->duration : 3); break; /* ticks per hall tile */
            case OP_BLIGHT: add_dot(h->blight, (int)op->v, e->duration > 0 ? e->duration : 3); break;
            case OP_CURE: memset(h->bleed, 0, sizeof(h->bleed)); memset(h->blight, 0, sizeof(h->blight)); break;
            case OP_CURE_BLEED: memset(h->bleed, 0, sizeof(h->bleed)); break;
            case OP_CURE_BLIGHT: memset(h->blight, 0, sizeof(h->blight)); break;
            case OP_CLEAR_DEBUFF: { /* negative carried stat changes (death's door recovery included) */
                int m = 0;
                for (int b = 0; b < h->nebuffs; b++)
                    if (h->ebuffs[b].amount >= 0) h->ebuffs[m++] = h->ebuffs[b];
                h->nebuffs = (uint8_t)m;
                break;
            }
            case OP_TORCH_INC: d->torch = d->torch + (int)op->v > 100 ? 100 : d->torch + (int)op->v; break;
            case OP_TORCH_DEC: d->torch = d->torch - (int)op->v < 0 ? 0 : d->torch - (int)op->v; break;
            case OP_STAT_ADD:
            case OP_STAT_MUL:
                add_ebuff(h, op->arg, 0, op->op == OP_STAT_MUL ? BUFF_MULTIPLY : BUFF_ADD, op->v, -1);
                break;
            case OP_BUFF: {
                const DDBuff *bf = &dd_buffs[op->arg];
                add_ebuff(h, bf->stat, bf->sub, bf->mode, bf->amount, e->duration > 0 ? e->duration : -1);
                break;
            }
            default: break;
            }
        }
    }
}

/* Use a curio. item<0 = investigate with bare hands, else spend that item.
 * Picks ONE result by weighted random from the right result list, then acts
 * on its type (loot, effect, quirk, purge, scouting, teleport, summon,
 * disease) and writes a one-line message for the UI. */
int curio_use(Dungeon *d, Hero *party[], int n, int who, int curio, int item, uint32_t seed, char *msg, int msglen) {
    const DDCurio *c = &dd_curios[curio];
    Rng r;
    rng_seed(&r, seed);
    int first = item < 0 ? c->res_first : c->item_first;
    int count = item < 0 ? c->res_count : c->item_count;
    float total = 0;
    for (int i = 0; i < count; i++) {
        const DDCurioResult *res = &dd_curio_results[first + i];
        if (item < 0 || res->item == item) total += res->weight;
    }
    if (item >= 0) d->items[item]--;
    const DDCurioResult *pick = NULL;
    float x = rng_float(&r) * total;
    for (int i = 0; i < count && !pick; i++) {
        const DDCurioResult *res = &dd_curio_results[first + i];
        if (item >= 0 && res->item != item) continue;
        if (x < res->weight) pick = res;
        else x -= res->weight;
    }
    Hero *h = who >= 0 && who < n ? party[who] : NULL;
    const char *name = h ? h->name : "The party";
    if (!pick || pick->type == CR_NOTHING) {
        snprintf(msg, msglen, "%s: nothing of value.", c->name);
        return CURIO_DONE;
    }
    switch (pick->type) {
    case CR_LOOT: {
        Loot *l = curio_loot;
        int nl = curio_nloot = loot_draw(&r, pick->effect, pick->draws > 0 ? pick->draws : 1, d->level, d->dungeon, l, 0);
        loot_take(d, l, nl);
        int len = snprintf(msg, msglen, "%s:", c->name);
        for (int i = 0; i < nl && len < msglen; i++) {
            char b[40];
            len += snprintf(msg + len, msglen - len, "%s %s", i ? "," : "", loot_label(&l[i], b, sizeof(b)));
        }
        if (!nl) snprintf(msg + len, msglen > len ? msglen - len : 0, " nothing of value.");
        break;
    }
    case CR_EFFECT:
        hero_apply_effect_offline(d, party, n, who, pick->effect, &r);
        snprintf(msg, msglen, "%s: %s (%s)", c->name, dd_effects[pick->effect].name, name);
        break;
    case CR_QUIRK: {
        int q = h ? quirk_random(&r, h, pick->draws > 0, 0) : -1;
        if (q >= 0 && quirk_add(h, q, &r)) snprintf(msg, msglen, "%s: %s gains %s.", c->name, name, dd_quirks[q].name);
        else snprintf(msg, msglen, "%s: nothing happens.", c->name);
        break;
    }
    case CR_PURGE: { /* removes a negative quirk (or a disease) */
        int cand[HERO_QUIRKS], n = 0;
        for (int i = 0; h && i < HERO_QUIRKS; i++)
            if (h->quirks[i] && !dd_quirks[h->quirks[i] - 1].positive) cand[n++] = i;
        if (n) {
            int i = cand[rng_range(&r, 0, n - 1)];
            snprintf(msg, msglen, "%s: %s is purged of %s.", c->name, name, dd_quirks[h->quirks[i] - 1].name);
            quirk_remove(h, i);
        } else {
            if (h) stress_add(h, -10, d->level, rng_u32(&r));
            snprintf(msg, msglen, "%s: %s feels cleansed.", c->name, name);
        }
        break;
    }
    case CR_SCOUTING:
        for (int i = 0; i < d->nrooms; i++) d->rooms[i].scouted = 1;
        for (int i = 0; i < d->ncorr; i++) d->corr[i].scouted = 1;
        snprintf(msg, msglen, "%s: the way ahead is revealed.", c->name);
        break;
    case CR_TELEPORT:
        snprintf(msg, msglen, "%s: the party is pulled elsewhere!", c->name);
        return CURIO_TELEPORT;
    case CR_SUMMON: /* summon_mash_shambler: something answers the flame */
        snprintf(msg, msglen, "%s: something stirs in the dark...", c->name);
        return CURIO_SUMMON;
    case CR_DISEASE: { /* resisted by disease resistance (+ quirks, trinkets) */
        float res = h ? dd_heroes[h->cls].res[RES_DISEASE] + quirk_stat(h, STAT_RES, RES_DISEASE) + trinket_stat(h, STAT_RES, RES_DISEASE) : 1;
        int q = h && !rng_chance(&r, res) ? quirk_random(&r, h, 0, 1) : -1;
        if (q >= 0 && quirk_add(h, q, &r)) snprintf(msg, msglen, "%s: %s contracts %s!", c->name, name, dd_quirks[q].name);
        else snprintf(msg, msglen, "%s: %s resists the sickness.", c->name, name);
        break;
    }
    default:
        snprintf(msg, msglen, "%s: nothing happens.", c->name);
        break;
    }
    return CURIO_DONE;
}
