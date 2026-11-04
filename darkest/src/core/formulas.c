/*
 * formulas.c - the numbers behind combat: hit chance, crit, damage, initiative.
 * Re-implemented from the behaviour documented in docs/mechanics.md.
 * Numbers marked "rules.json" come from the game's shared/rules.json.
 *
 * Quick tour of the core rules, all expressed as fractions (0.05 = 5%):
 *   HIT    chance = attacker accuracy + skill accuracy - target dodge (DEF)
 *          A roll of 0.95+ always hits (so nothing is ever <5% to hit).
 *   CRIT   chance = attacker crit stat + skill crit (+ torch-light bonus)
 *   DAMAGE random in [weapon lo, weapon hi] * (1 + skill mod); a crit uses the
 *          max * 1.5 for heroes; then protection (PROT, a %) removes a share.
 *   SPEED  initiative = SPD + random 0..7; highest acts first.
 * "Darkness": the torch level adds or removes bonuses (dark_for) - low light
 * makes monsters hit harder but gives heroes crit/stress changes, etc.
 */
#include "formulas.h"

#include <math.h>
#include <string.h>
#include <stddef.h>

#include "quirk.h"
#include "trinket.h"

/* Tunable constants. Names say what they are; comments give the data source. */
#define CUTOFF_ALWAYS_HIT 0.95f      /* rules.json cutoffAlwaysHit: a roll this high always hits */
#define HERO_CRIT_MULT 1.5f          /* rules.json player_crit_multiplier_table */
#define MONSTER_CRIT_MULT 1.0f       /* code constant: monsters crit for their max damage */
#define MISS_STREAK_ACC 0.04f        /* rules.json consecutive_miss_reduction_acc_buff */
#define INITIATIVE_RANGE 8           /* rules.json initiative_base: + random 0..7 */
#define SPEED_TIEBREAK 1.001f        /* code constant: faster actor wins ties */
#define DEATHBLOW_BASE 1.0f          /* rules.json deathblow_chance */
#define EFFECT_CRIT_ROLL_MOD -0.2f   /* rules.json effect_crit_roll_mod */
#define HEAL_MULT_MIN 0.0f           /* rules.json heal_buff_percent_min/max */
#define HEAL_MULT_MAX 2.0f

/* The active rule set for the campaign's game mode (Radiant/Darkest/Stygian);
 * the campaign points this at dd_rules[mode] when a game starts. */
const DDRules *g_rules; /* set to &dd_rules[mode] once the data is loaded (game_init / campaign) */

/* rules.json "darkness" (per game mode): the DK_* bonuses (in %) for this torch level. Bands are
 * (lower, upper]; a torch of exactly 0 falls into the darkest band. */
const float *dark_for(int torch) {
    for (int i = 0; i < 4; i++)
        if (torch > g_rules->dark_lower[i]) return g_rules->dark[i];
    return g_rules->dark[4];
}

/* ---------------------------------------------------------------- stats */

/* Unbuffed stat. Heroes: gear tier tables (weapon_lvl/armour_lvl pick the row).
 * Max HP is also scaled by quirks and trinkets here. Monsters: class values. */
float base_stat(const Actor *a, DDStat stat) {
    if (a->is_hero) {
        const DDHeroClass *h = &dd_heroes[a->cls];
        const DDWeapon *w = &h->weapon[a->hero->weapon_lvl];
        const DDArmour *ar = &h->armour[a->hero->armour_lvl];
        switch (stat) {
        case STAT_ATK: return w->atk;
        case STAT_DEF: return ar->def;
        case STAT_PROT: return ar->prot;
        case STAT_SPD: return (float)(w->spd + ar->spd);
        case STAT_CRIT: return w->crit;
        case STAT_DMG_LO: return w->dmg_lo;
        case STAT_DMG_HI: return w->dmg_hi;
        case STAT_MAX_HP: return (float)(int)(ar->hp * (1 + quirk_stat(a->hero, STAT_MAX_HP, 0) + trinket_stat(a->hero, STAT_MAX_HP, 0)) + 0.5f);
        default: return 0;
        }
    }
    const DDMonsterClass *m = &dd_monsters[a->cls];
    switch (stat) {
    case STAT_DEF: return m->def;
    case STAT_PROT: return m->prot;
    case STAT_SPD: return m->spd;
    case STAT_MAX_HP: return m->hp;
    default: return 0;
    }
}

/* CONTEXT: conditional buffs ("+10% vs beasts", "only in the first round")
 * need to know the current attack. Rather than pass these through every
 * call, formulas_context() stores them in file-level statics just before a
 * stat is read. (A quirk of the design: always call it first.)
 * Stat value = (base + additive buffs) * (1 + sum of multiplier buffs)  — RaidStat::GetValue */
static const Battle *ctx_b;
static const DDSkill *ctx_skill;
static const Actor *ctx_target;

void formulas_context(const Battle *b, const DDSkill *skill, const Actor *target) {
    ctx_b = b;
    ctx_skill = skill;
    ctx_target = target;
}

/* Is this buff's condition currently true? One case per DDBuffRule.
 * rule_not inverts the answer ("when NOT in the front rank").
 * Buff rule_type conditions, judged in battle (out-of-battle rules never apply here). */
int buff_rule_ok(const Actor *a, const DDBuff *bf) {
    int ok;
    switch (bf->rule) {
    case RULE_ALWAYS: case RULE_IN_DUNGEON: ok = 1; break;
    case RULE_SKILL: ok = ctx_skill && bf->rule_s && strcmp(ctx_skill->id, bf->rule_s) == 0; break;
    case RULE_MONSTER_TYPE:
        ok = ctx_target && !ctx_target->is_hero && bf->rule_type >= 0 &&
             (dd_monsters[ctx_target->cls].types & (1 << bf->rule_type));
        break;
    case RULE_MONSTER_TYPE_COUNT_MIN: {
        int n = 0;
        for (int i = 0; ctx_b && i < MAX_ACTORS; i++) {
            const Actor *m = &ctx_b->actors[i];
            n += m->used && m->alive && !m->is_hero && !m->is_corpse && bf->rule_type >= 0 &&
                 (dd_monsters[m->cls].types & (1 << bf->rule_type));
        }
        ok = n >= (int)bf->rule_f;
        break;
    }
    case RULE_LIGHT_ABOVE: ok = ctx_b && ctx_b->torch > bf->rule_f; break;
    case RULE_LIGHT_BELOW: ok = ctx_b && ctx_b->torch < bf->rule_f; break;
    case RULE_HP_BELOW: ok = a->max_hp > 0 && (float)a->hp / a->max_hp < bf->rule_f; break;
    case RULE_FIRST_ROUND: ok = ctx_b && ctx_b->round <= 1; break;
    case RULE_RANGED: ok = ctx_skill && ctx_skill->type == SK_RANGED; break;
    case RULE_MELEE: ok = ctx_skill && ctx_skill->type == SK_MELEE; break;
    case RULE_STRESS_ABOVE: ok = a->is_hero && a->hero && a->hero->stress > bf->rule_f; break;
    case RULE_HP_ABOVE: ok = a->max_hp > 0 && (float)a->hp / a->max_hp > bf->rule_f; break;
    case RULE_DEATHS_DOOR: ok = a->deaths_door; break;
    case RULE_IN_RANK: ok = a->rank == (int)bf->rule_f + 1; break;
    default: ok = 0; break; /* camp, town activity, corridor walking */
    }
    return bf->rule_not ? !ok : ok;
}

/* Fold all of an actor's buffs for `stat` into a base value:
 * additive buffs sum first, then multiplier buffs scale the total. */
float stat_from_base(const Actor *a, DDStat stat, float base) {
    float add = base, mul = 0;
    for (int i = 0; i < a->nbuffs; i++) {
        const Buff *bf = &a->buffs[i];
        if (bf->stat != stat || stat == STAT_RES || stat == STAT_EFFECT_CHANCE || stat == STAT_SURPRISE) continue;
        if (bf->rule_buff >= 0 && !buff_rule_ok(a, &dd_buffs[bf->rule_buff])) continue;
        if (bf->mode == BUFF_MULTIPLY) mul += bf->amount;
        else add += bf->amount;
    }
    return add * (1 + mul);
}

/* The final live value of a stat (base + buffs). */
float actor_stat(const Actor *a, DDStat stat) { return stat_from_base(a, stat, base_stat(a, stat)); }

/* Sum of the actor's live buffs for a stat with a sub type (STAT_RES, STAT_EFFECT_CHANCE). */
static float sub_buffs(const Actor *a, int stat, int sub) {
    float v = 0;
    for (int i = 0; i < a->nbuffs; i++)
        if (a->buffs[i].stat == stat && a->buffs[i].sub == sub &&
            (a->buffs[i].rule_buff < 0 || buff_rule_ok(a, &dd_buffs[a->buffs[i].rule_buff])))
            v += a->buffs[i].amount;
    return v;
}

/* Resistance % = class value + buffs, never below 0. */
static float resistance(const Actor *a, int res) {
    float v = (a->is_hero ? dd_heroes[a->cls].res[res] : dd_monsters[a->cls].res[res]) + sub_buffs(a, STAT_RES, res);
    if (v < 0) v = 0;
    return v;
}

float actor_resistance(const Actor *a, int res) { return resistance(a, res); }

float actor_sub_stat(const Actor *a, int stat, int sub) { return sub_buffs(a, stat, sub); }

/* Darkness changes surprise odds; returns a fraction to add to the base chance. */
float f_surprise_light(int torch, int heroes) {
    const float *d = dark_for(torch);
    return (heroes ? d[DK_H_SURP] : d[DK_M_SURP]) / 100.0f;
}

/* ---------------------------------------------------------------- attack */

/* Hit chance before the 5% always-hit rule. Monsters get a darkness accuracy
 * bonus; heroes get a darkness dodge bonus; heroes gain +4% per consecutive
 * miss (a "pity" mechanic so long miss streaks end). */
float f_hit_chance(const Battle *b, const Actor *att, const DDSkill *s, const Actor *tgt) {
    formulas_context(b, s, tgt);
    float acc = actor_stat(att, STAT_ATK) + s->atk;
    if (!att->is_hero) acc += dark_for(b->torch)[DK_M_ACC] / 100.0f;
    float dodge = actor_stat(tgt, STAT_DEF);
    if (tgt->is_hero) dodge += dark_for(b->torch)[DK_H_DEF] / 100.0f;
    float chance = acc - dodge;
    if (att->is_hero) chance += b->hero_miss_streak * MISS_STREAK_ACC;
    return chance;
}

/* Compare one random roll [0,1) against the hit chance. Corpses, "cannot
 * miss" skills and never-missed monsters always hit. */
int f_roll_hit(const Battle *b, const Actor *att, const DDSkill *s, const Actor *tgt, float roll) {
    if (tgt->is_corpse || (s->flags & SKF_CANNOT_MISS)) return 1;
    if (!tgt->is_hero && (dd_monsters[tgt->cls].flags2 & MF2_NEVER_MISSED)) return 1; /* can_be_missed false */
    if (roll >= CUTOFF_ALWAYS_HIT) return 1; /* 5% of rolls always hit */
    return roll < f_hit_chance(b, att, s, tgt);
}

/* Crit chance = attacker crit stat + skill crit + darkness bonus. */
float f_crit_chance(const Battle *b, const Actor *att, const DDSkill *s, const Actor *tgt) {
    formulas_context(b, s, tgt);
    (void)tgt;
    float c = actor_stat(att, STAT_CRIT) + s->crit;
    const float *d = dark_for(b->torch);
    c += (att->is_hero ? d[DK_H_CRIT] : d[DK_M_CRIT]) / 100.0f;
    return c;
}

/* Damage dealt by one hit, after the target's protection (AddTarget + ApplyHealthDamage). */
int f_damage(Battle *b, const Actor *att, const DDSkill *s, const Actor *tgt, int crit) {
    formulas_context(b, s, tgt);
    /* heroes: weapon damage scaled by the skill; monsters: the skill's own damage */
    float base_lo = att->is_hero ? base_stat(att, STAT_DMG_LO) * (1 + s->dmg_mod) : s->dmg_lo;
    float base_hi = att->is_hero ? base_stat(att, STAT_DMG_HI) * (1 + s->dmg_mod) : s->dmg_hi;
    float lo = stat_from_base(att, STAT_DMG_LO, base_lo);
    float hi = stat_from_base(att, STAT_DMG_HI, base_hi);
    float dmg;
    if (crit) {
        dmg = hi * (att->is_hero ? HERO_CRIT_MULT : MONSTER_CRIT_MULT);
    } else {
        int ilo = (int)ceilf(lo), ihi = (int)ceilf(hi);
        dmg = (float)(ihi > ilo ? rng_range(&b->rng, ilo, ihi) : ilo);
    }
    if (!att->is_hero) dmg *= 1 + dark_for(b->torch)[DK_M_DMG] / 100.0f;

    float prot = (s->flags & SKF_IGNORE_PROT) ? 0 : actor_stat(tgt, STAT_PROT);
    if (prot < 0) prot = 0;
    float blocked = roundf(dmg * prot);
    if (prot > 0 && blocked < 1) blocked = 1; /* any protection blocks at least 1 */
    dmg -= blocked;
    float recv = 1 + actor_stat(tgt, STAT_DMG_RECEIVED); /* damage_received_percent (talisman: -100% vs Revelation) */
    dmg *= recv < 0 ? 0 : recv;
    if (dmg < 0) dmg = 0;
    return (int)roundf(dmg);
}

/* hero_skill 12% x2, hero_skill_multi_target 5% x2, monster_skill(_multi_target) 5% x1.5;
 * the roll is its own (Actor::ApplyHealthHealInternal), not the healer's crit stat */
float f_heal_crit_chance(const Actor *healer, int multi) { return healer->is_hero && !multi ? 0.12f : 0.05f; }
float f_heal_crit_mult(const Actor *healer) { return healer->is_hero ? 2.0f : 1.5f; }

/* Heal amount: random in the skill's heal range, scaled by the TARGET's
 * heal-received stat (clamped to 0x..2x), doubled/1.5x on a crit. */
int f_heal(Battle *b, const Actor *att, const DDSkill *s, const Actor *tgt, int crit) {
    float amount = (float)rng_range(&b->rng, s->heal_lo, s->heal_hi);
    float mult = 1 + actor_stat(tgt, STAT_HEAL_RECEIVED);
    if (mult < HEAL_MULT_MIN) mult = HEAL_MULT_MIN;
    if (mult > HEAL_MULT_MAX) mult = HEAL_MULT_MAX;
    amount *= mult;
    if (crit) amount *= f_heal_crit_mult(att);
    return (int)ceilf(amount);
}

/* Chance a status effect (stun, bleed...) lands. Attacker's chance buffs add,
 * target's resistance subtracts. The caller rolls against the returned value.
 * Effect lands when: roll <= chance - resistance (crit lowers the roll by 0.2). */
float f_effect_chance(const Actor *att, const DDEffect *e, const Actor *tgt, int res, int crit) {
    /* <x>_chance trinket buffs on the attacker raise the base chance against that resistance */
    float chance = e->chance + (res >= 0 ? sub_buffs(att, STAT_EFFECT_CHANCE, res) - resistance(tgt, res) : 0);
    if (crit) chance -= EFFECT_CRIT_ROLL_MOD;
    return chance;
}

/* Round order key: speed (+ tiny tiebreak) + random 0..7, scaled x1000 so it
 * stays an int. Higher acts first. */
int f_initiative(Battle *b, const Actor *a) {
    formulas_context(b, NULL, NULL);
    float v = actor_stat(a, STAT_SPD) * SPEED_TIEBREAK + rng_range(&b->rng, 0, INITIATIVE_RANGE - 1);
    return (int)(v * 1000);
}

/* Chance a hero on death's door dies from a hit: 100% minus their deathblow
 * resistance. */
float f_deathblow_chance(const Actor *a) {
    float res = resistance(a, RES_DEATHBLOW);
    if (res > 1) res = 1;
    return DEATHBLOW_BASE - res;
}
