/*
 * trinket.c - trinket rolls, stat summation and text. Hero.trinkets[] holds
 * (dd_trinkets index + 1), 0 = empty. Mirrors quirk.c closely.
 */
#include "trinket.h"

#include <stdio.h>
#include <string.h>

#include "effects.h"

/* Uniform pick among trinkets of a rarity that may drop in this dungeon
 * (two passes: count candidates, then walk to the k-th). */
int trinket_roll(Rng *r, int rarity, int dungeon) {
    int n = 0;
    for (int i = 0; i < dd_trinket_count; i++)
        n += dd_trinkets[i].rarity == rarity && (dd_trinkets[i].dungeon < 0 || dd_trinkets[i].dungeon == dungeon);
    if (!n) return -1;
    int k = rng_range(r, 0, n - 1);
    for (int i = 0; i < dd_trinket_count; i++)
        if (dd_trinkets[i].rarity == rarity && (dd_trinkets[i].dungeon < 0 || dd_trinkets[i].dungeon == dungeon) && k-- == 0)
            return i;
    return -1;
}

/* Sum of unconditional trinket buffs for a stat (used by out-of-battle
 * formulas such as max HP). */
float trinket_stat(const Hero *h, int stat, int sub) {
    float v = 0;
    for (int s = 0; s < HERO_TRINKETS; s++) {
        if (!h->trinkets[s]) continue;
        const DDTrinket *t = &dd_trinkets[h->trinkets[s] - 1];
        for (int k = 0; k < t->buff_count; k++) {
            const DDBuff *b = &dd_buffs[dd_buff_list[t->buff_first + k]];
            int on = b->rule == RULE_ALWAYS || b->rule == RULE_IN_DUNGEON;
            if (b->rule_not) on = !on;
            if (on && b->stat == stat && ((stat != STAT_RES && stat != STAT_EFFECT_CHANCE && stat != STAT_SURPRISE) || b->sub == sub)) v += b->amount;
        }
    }
    return v;
}

/* Copy trinket buffs onto a battle Actor (see quirks_to_actor). */
void trinkets_to_actor(Actor *a) {
    if (!a->hero) return;
    for (int s = 0; s < HERO_TRINKETS; s++) {
        if (!a->hero->trinkets[s]) continue;
        const DDTrinket *t = &dd_trinkets[a->hero->trinkets[s] - 1];
        for (int k = 0; k < t->buff_count; k++) {
            int bi = dd_buff_list[t->buff_first + k];
            const DDBuff *b = &dd_buffs[bi];
            if (b->stat == STAT_MAX_HP || b->stat == STAT_NONE) continue; /* max HP is in hero_max_hp */
            int before = a->nbuffs;
            effects_add_buff(a, b->stat, b->sub, b->mode, b->amount, -1, 0);
            if (a->nbuffs > before) {
                a->buffs[a->nbuffs - 1].rule_buff = (int16_t)bi;
                a->buffs[a->nbuffs - 1].from_effect = 0; /* permanent, like quirk buffs */
            }
        }
    }
}

/* Equip rules: class-restricted trinkets, and "limit 1" duplicates. */
const char *trinket_equip_error(const Hero *h, int t, int slot) {
    const DDTrinket *d = &dd_trinkets[t];
    if (d->cls >= 0 && d->cls != h->cls) return "Only for another class.";
    if (d->limit == 1)
        for (int s = 0; s < HERO_TRINKETS; s++)
            if (s != slot && h->trinkets[s] == t + 1) return "Only one per hero.";
    return NULL;
}

/* Display name for a rarity. */
const char *trinket_rarity_name(int rarity) {
    static const char *const names[TR_COUNT] = {"Very Common", "Common", "Uncommon", "Rare", "Very Rare", "Ancestral",
        "Trophy", "Crow", "Ancestral", "Madman", "Collector", "Courtier", "Darkest Dungeon"};
    return rarity >= 0 && rarity < TR_COUNT ? names[rarity] : "";
}

/* One stat change as sheet text ("DODGE +5", "Stun res -10%"). */
/* Format one buff as short text, e.g. "SPD +3" or "Stun res -10%". Some stats
 * are shown as raw points, others as percentages (amount*100). */
int stat_change_text(int stat, int sub, int mode, float amount, char *buf, int len) {
    static const char *const stat_name[STAT_COUNT] = {"", "ACC", "DODGE", "PROT", "SPD", "CRIT", "DMG", "DMG", "HP", "",
        "Healing recv", "Crit recv", "Stress", "Stress heal recv", "Food", "Virtue", "Healing", "", "", "Dmg recv"};
    static const char *const res_name[RES_COUNT] = {"Stun", "Blight", "Bleed", "Disease", "Move", "Debuff", "Deathblow", "Trap"};
    if (stat == STAT_NONE || stat >= STAT_COUNT) { if (len) buf[0] = 0; return 0; }
    char name[24];
    if (stat == STAT_RES) snprintf(name, sizeof(name), "%s res", res_name[sub % RES_COUNT]);
    else if (stat == STAT_EFFECT_CHANCE) snprintf(name, sizeof(name), "%s chance", res_name[sub % RES_COUNT]);
    else if (stat == STAT_SURPRISE) snprintf(name, sizeof(name), sub ? "Surprise foes" : "Be surprised");
    else snprintf(name, sizeof(name), "%s", stat_name[stat]);
    int add = mode == BUFF_ADD, raw = add && stat == STAT_SPD;
    int pct = !(add && (stat == STAT_SPD || stat == STAT_ATK || stat == STAT_DEF)); /* ACC/DODGE/SPD are points */
    int v = (int)(amount * (raw ? 1 : 100) + (amount < 0 ? -0.5f : 0.5f));
    return snprintf(buf, len, "%s %+d%s", name, v, pct ? "%" : "");
}

/* One-line summary of all a trinket's buffs; conditional ones end in `*`. */
const char *trinket_describe(int t, char *buf, int len) {
    const DDTrinket *d = &dd_trinkets[t];
    int n = 0;
    buf[0] = 0;
    for (int k = 0; k < d->buff_count && n < len - 1; k++) {
        const DDBuff *b = &dd_buffs[dd_buff_list[d->buff_first + k]];
        if (b->stat == STAT_NONE || b->stat >= STAT_COUNT || b->stat == STAT_DMG_HI) continue; /* DMG shown once */
        char one[40];
        stat_change_text(b->stat, b->sub, b->mode, b->amount, one, sizeof(one));
        int cond = b->rule != RULE_ALWAYS && b->rule != RULE_IN_DUNGEON;
        n += snprintf(buf + n, len - n, "%s%s%s", n ? ", " : "", one, cond ? "*" : "");
    }
    return buf;
}
