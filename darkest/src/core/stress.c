/*
 * stress.c - the stress system (the heart of Darkest Dungeon's psychology).
 *
 * CONCEPT: every hero has a stress meter 0..200.
 *   - Stress rises from monster attacks, crits, ally deaths, darkness, traps...
 *   - At 100 the hero takes a RESOLVE CHECK: usually an AFFLICTION (selfish,
 *     paranoid, abusive... - bad buffs plus random "act-outs" that waste or
 *     ruin turns), but sometimes a VIRTUE (good buffs, stress drops to 45).
 *   - At 200 the hero has a HEART ATTACK (death's door, or death if already
 *     there).
 *   - Afflictions/virtues are cleared in town or fade after the expedition.
 * Many modifiers scale the amount: the hero's stress-received buffs, how much
 * higher the dungeon level is than the hero's level, and darkness.
 *
 * Each rule cites where it came from; numbers are shared/rules.json values
 * unless noted.
 */
#include "stress.h"

#include <math.h>
#include <string.h>

#include "effects.h"
#include "formulas.h"
#include "quirk.h"
#include "trinket.h"

/* Core thresholds and chances (fractions are probabilities). */
#define STRESS_MAX 200
#define RESOLVE_AT 100
#define VIRTUE_STRESS 45          /* stress_affliction_critically_resisted */
#define HEART_ATTACK_STRESS 170   /* stress left after surviving a heart attack */
#define AFFLICTION_BASE 0.75f     /* affliction_base_onset_chance */
#define AFFLICTION_MIN 0.05f
#define AFFLICTION_MAX 0.99f
#define TRAIT_BASE_WEIGHT 4       /* base_affliction_history_WF / base_virtue_history_WF */
#define STRESS_MULT_MIN 0.2f      /* stress_dmg_buff_min */

/* rules.json monster_crit / player_crit / deaths_door / death / pass / killing blow */
#define CRIT_STRESS 10
#define CRIT_STRESS_COMPANION 5
#define CRIT_RELIEF 3
#define CRIT_RELIEF_COMPANION_CHANCE 0.25f
#define DEATH_PARTY_STRESS 12
#define DEATH_PARTY_CHANCE 0.5f
#define KILL_RELIEF 3
#define KILL_RELIEF_CHANCE 0.5f

/* rules.json effectiveDifficultyStressDmgModifiers / AfflictionOnsetModifiers, indexed by
 * dungeon difficulty minus hero level (clamped 0..6) */
/* Stress/onset get worse when the dungeon outlevels the hero. */
static const float diff_stress[7] = {0, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f};
static const float diff_onset[7] = {0, 0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.33f};

/* rules.json darkness: stress chance / damage increase by torch level (per game mode) */
/* Darkness adds both extra stress chance and extra stress damage. */
static void darkness(int torch, float *chance, float *dmg) {
    const float *d = dark_for(torch);
    *chance = d[DK_STRESS_CHANCE] / 100.0f;
    *dmg = d[DK_STRESS_DMG] / 100.0f;
}

/* How many levels the dungeon exceeds the hero (clamped into table range 0..6). */
static int diff_index(int level, const Hero *h) {
    int d = level - h->level;
    return d < 0 ? 0 : d > 6 ? 6 : d;
}

/* Multiplier on stress taken: 1 + sum of the hero's stress-received buffs.
 * In battle the actor's live buffs are used; out of battle we use the hero's
 * quirks, trinkets and current affliction directly. Never below 0.2x. */
static float trait_stress_mult(const Hero *h, const Actor *a) {
    /* STAT_STRESS_DMG_RECEIVED buffs (virtues, afflictions, camping...) */
    float m = 1;
    if (a) {
        for (int i = 0; i < a->nbuffs; i++)
            if (a->buffs[i].stat == STAT_STRESS_DMG_RECEIVED &&
                (a->buffs[i].rule_buff < 0 || buff_rule_ok(a, &dd_buffs[a->buffs[i].rule_buff])))
                m += a->buffs[i].amount;
    } else {
        m += quirk_stat(h, STAT_STRESS_DMG_RECEIVED, 0) + trinket_stat(h, STAT_STRESS_DMG_RECEIVED, 0);
    }
    if (!a && h->affliction) {
        const DDTrait *t = &dd_traits[h->affliction - 1];
        for (int i = 0; i < t->buff_count; i++) {
            const DDBuff *bf = &dd_buffs[dd_buff_list[t->buff_first + i]];
            if (bf->stat == STAT_STRESS_DMG_RECEIVED) m += bf->amount;
        }
    }
    return m < STRESS_MULT_MIN ? STRESS_MULT_MIN : m;
}

/* ---------------------------------------------------------------- core */

/* Choose which affliction or virtue to give. Every trait of the right type
 * has weight 4, plus 1 per time this hero already rolled it (so patterns
 * repeat) - roulette selection like in ai.c. */

static int pick_trait(Hero *h, int virtue, Rng *r) {
    float w[32], total = 0;
    int n = dd_trait_count < 32 ? dd_trait_count : 32;
    for (int i = 0; i < n; i++) {
        w[i] = dd_traits[i].type == (virtue ? TRAIT_VIRTUE : TRAIT_AFFLICTION)
                   ? TRAIT_BASE_WEIGHT + (i < 16 ? h->trait_history[i] : 0)
                   : 0;
        total += w[i];
    }
    float x = rng_float(r) * total;
    for (int i = 0; i < n; i++) {
        if (w[i] <= 0) continue;
        if (x < w[i]) return i;
        x -= w[i];
    }
    return 0;
}

/* Put the active trait's buffs onto the battle actor (tagged from_effect=3
 * so they can be removed again cleanly). */
static void apply_trait_buffs(Actor *a) {
    if (!a || !a->hero || !a->hero->affliction) return;
    const DDTrait *t = &dd_traits[a->hero->affliction - 1];
    for (int i = 0; i < t->buff_count; i++) {
        const DDBuff *bf = &dd_buffs[dd_buff_list[t->buff_first + i]];
        effects_add_buff(a, bf->stat, bf->sub, bf->mode, bf->amount, -1, 3);
    }
}

/* Strip exactly the trait buffs added above. */
static void remove_trait_buffs(Actor *a) {
    if (!a) return;
    int n = 0;
    for (int i = 0; i < a->nbuffs; i++)
        if (a->buffs[i].from_effect != 3) a->buffs[n++] = a->buffs[i];
    a->nbuffs = n;
}

/* Behaviors::GenerateAfflictionOrVirtueForHero: at 100 stress, affliction with
 * clamp(0.75 + onset[difficulty] - resolve bonus, 5%, 99%), otherwise a virtue. */
/* ---- called when stress reaches 100: decides affliction vs virtue ---- */
static void resolve_check(Battle *b, Hero *h, Actor *a, int idx, int level, Rng *r) {
    /* resolve_check_percent (quirks, trinkets) lowers the affliction chance */
    float chance = AFFLICTION_BASE + diff_onset[diff_index(level, h)] -
                   (a ? actor_stat(a, STAT_RESOLVE) : quirk_stat(h, STAT_RESOLVE, 0) + trinket_stat(h, STAT_RESOLVE, 0));
    if (chance < AFFLICTION_MIN) chance = AFFLICTION_MIN;
    if (chance > AFFLICTION_MAX) chance = AFFLICTION_MAX;
    int virtue = rng_float(r) >= chance;
    int t = pick_trait(h, virtue, r);
    h->affliction = (uint8_t)(t + 1);
    if (t < 16 && h->trait_history[t] < 255) h->trait_history[t]++;
    if (virtue) h->stress = VIRTUE_STRESS;
    apply_trait_buffs(a);
    if (b) battle_event(b, virtue ? EV_VIRTUE : EV_AFFLICTION, idx, -1, t, -1);
}

/* Stress hit 200. In battle: a hero already on death's door dies, otherwise
 * drops to 0 HP at death's door (flag remembered for the special recovery).
 * Outside battle: left at 1 HP and 170 stress. */
static void heart_attack(Battle *b, Hero *h, int idx) {
    if (b) battle_event(b, EV_HEART_ATTACK, idx, -1, 0, -1);
    if (b && idx >= 0) {
        Actor *a = &b->actors[idx];
        if (a->deaths_door) {
            battle_kill(b, idx, -1);
        } else {
            a->hp = 0;
            a->deaths_door = 1;
            a->heart_attacked = 1;
            effects_apply_deaths_door(b, idx);
            battle_event(b, EV_DEATHS_DOOR, idx, -1, 0, -1);
        }
    } else {
        /* outside battle a heart attack leaves the hero at 0 HP... on death's door next fight */
        h->hp = 1;
    }
    h->stress = HEART_ATTACK_STRESS;
}

/* Actor::ApplyRawStressDamage: stress rises; crossing 100 triggers the resolve check (stress
 * stays capped at 100 until it resolves); an afflicted hero reaching 200 has a heart attack. */
static void raw_stress(Battle *b, Hero *h, int idx, int amount, int level, Rng *r) {
    if (!h->alive || amount == 0) return;
    Actor *a = b && idx >= 0 ? &b->actors[idx] : NULL;
    if (amount < 0) {
        /* Actor::ApplyStressHeal: can't go below 0; an affliction ends at 0.
         * stress_heal_received_percent quirks scale the relief. */
        float hm = 1 + quirk_stat(h, STAT_STRESS_HEAL_RECEIVED, 0) + trinket_stat(h, STAT_STRESS_HEAL_RECEIVED, 0);
        amount = (int)(amount * (hm < 0 ? 0 : hm) - 0.5f);
        int before = h->stress;
        h->stress = (int16_t)(h->stress + amount < 0 ? 0 : h->stress + amount);
        if (b && before > h->stress) battle_event(b, EV_STRESS_HEAL, idx, -1, before - h->stress, -1);
        if (h->stress == 0 && h->affliction && dd_traits[h->affliction - 1].type == TRAIT_AFFLICTION) {
            h->affliction = 0;
            remove_trait_buffs(a);
        }
        return;
    }
    int before = h->stress;
    int s = h->stress + amount;
    int afflicted = h->affliction && dd_traits[h->affliction - 1].type == TRAIT_AFFLICTION;
    if (s > STRESS_MAX) s = STRESS_MAX;
    h->stress = (int16_t)s;
    if (b) battle_event(b, EV_STRESS, idx, -1, s - before, -1);
    if (!h->affliction && s >= RESOLVE_AT) {
        h->stress = RESOLVE_AT;
        resolve_check(b, h, a, idx, level, r);
    } else if (afflicted && s >= STRESS_MAX && before < STRESS_MAX) {
        heart_attack(b, h, idx);
    } else if (h->affliction && !afflicted && s >= STRESS_MAX) {
        h->stress = STRESS_MAX; /* virtuous heroes are capped instead */
    }
}

/* Actor::ApplyStressDamage: amount x (1 + modifiers, min 0.2) x (difficulty + darkness), rounded */
/* Public-ish stress damage in battle: applies all multipliers (buffs,
 * difficulty gap, darkness) then calls raw_stress. */
static void stress_damage(Battle *b, int idx, float base, int scaled) {
    Actor *a = &b->actors[idx];
    if (!a->is_hero || !a->alive) return;
    Hero *h = a->hero;
    float factor = 1;
    if (scaled) {
        float dc, dd;
        darkness(b->torch, &dc, &dd);
        factor = 1 + diff_stress[diff_index(b->level, h)] + dd;
    }
    int amount = (int)roundf(base * trait_stress_mult(h, a) * factor);
    if (amount > 0) raw_stress(b, h, idx, amount, b->level, &b->rng);
}

/* Stress healing in battle (kills, crits by allies, camp-like effects). */
static void stress_relief(Battle *b, int idx, float base) {
    Actor *a = &b->actors[idx];
    if (!a->is_hero || !a->alive) return;
    int amount = (int)roundf(base);
    if (amount > 0) raw_stress(b, a->hero, idx, -amount, b->level, &b->rng);
}

/* "Witness" stress: other heroes may be stressed (or relieved) when something
 * happens to an ally, each with its own chance. */
static void companions(Battle *b, int idx, float chance, float amount, int heal) {
    const Actor *a = &b->actors[idx];
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *o = &b->actors[i];
        if (i == idx || !o->used || !o->alive || !o->is_hero || o->side != a->side) continue;
        if (!rng_chance(&b->rng, chance)) continue;
        if (heal) stress_relief(b, i, amount);
        else stress_damage(b, i, amount, 1);
    }
}

/* ---------------------------------------------------------------- battle hooks */

/* Hit landed. A crit by a monster stresses the victim (and watchers); a crit
 * by a hero relieves the party a little. */
void stress_on_hit(Battle *b, int attacker, int target, int crit) {
    if (!crit) return;
    const Actor *a = &b->actors[attacker], *t = &b->actors[target];
    float dc, dd;
    darkness(b->torch, &dc, &dd);
    if (!a->is_hero && t->is_hero) {
        /* Battle::CalcStressFromCrit */
        if (rng_chance(&b->rng, 1.0f + dc)) stress_damage(b, target, CRIT_STRESS, 1);
        companions(b, target, g_rules->crit_comp_chance + dc, CRIT_STRESS_COMPANION, 0);
    } else if (a->is_hero && !t->is_hero && !(dd_monsters[t->cls].flags2 & MF2_NO_CRIT_RELIEF)) {
        /* Battle::CalcReliefFromCrit (not on corpses and the like: can_relieve_stress_from_crit) */
        stress_relief(b, attacker, CRIT_RELIEF);
        companions(b, attacker, CRIT_RELIEF_COMPANION_CHANCE, CRIT_RELIEF, 1);
    }
}

void stress_on_miss(Battle *b, int attacker) { (void)b; (void)attacker; /* only quirk/trinket driven */ }

/* A killing blow may relieve the killer's stress by chance. */
void stress_on_kill(Battle *b, int attacker, int victim) {
    if (victim >= 0 && !b->actors[victim].is_hero && (dd_monsters[b->actors[victim].cls].flags2 & MF2_NO_KILL_RELIEF)) return;
    if (attacker >= 0 && b->actors[attacker].is_hero && rng_chance(&b->rng, KILL_RELIEF_CHANCE))
        stress_relief(b, attacker, KILL_RELIEF);
}

/* Entry for skill effects that explicitly add/remove stress (bark skills, etc.). */
void stress_apply_effect(Battle *b, int source, int target, float amount, int is_heal) {
    (void)source;
    if (is_heal) stress_relief(b, target, amount);
    else stress_damage(b, target, amount, 1);
}

/* A hero hitting death's door spikes party stress. */
void stress_on_deaths_door(Battle *b, int idx) {
    Hero *h = b->actors[idx].hero;
    int repeat = h->deaths_door_visits > 0;
    if (h->deaths_door_visits < 255) h->deaths_door_visits++;
    stress_damage(b, idx, repeat ? g_rules->dd_self_rep : g_rules->dd_self, 0);
    companions(b, idx, repeat ? g_rules->dd_comp_chance_rep : g_rules->dd_comp_chance, (int)(repeat ? g_rules->dd_comp_rep : g_rules->dd_comp), 0);
}

/* An ally died: every other hero gets stress (with a chance). */
void stress_on_hero_death(Battle *b, int idx) {
    companions(b, idx, DEATH_PARTY_CHANCE, DEATH_PARTY_STRESS, 0);
}

void stress_on_pass(Battle *b, int idx) { stress_damage(b, idx, g_rules->pass_stress, 0); }

void stress_on_round_end(Battle *b) { (void)b; }

void stress_battle_start(Battle *b, int idx) { apply_trait_buffs(&b->actors[idx]); }

/* Afflicted heroes may refuse healing/moves/buffs (kind 0/1/2). */
int stress_refuses(Battle *b, int idx, int kind) {
    const Actor *a = &b->actors[idx];
    if (!a->is_hero || !a->hero->affliction) return 0;
    const DDTrait *t = &dd_traits[a->hero->affliction - 1];
    float c = kind == 0 ? t->block_heal : kind == 1 ? t->block_move : t->block_buff;
    if (c > 0 && rng_chance(&b->rng, c)) {
        battle_event(b, EV_REFUSE, idx, -1, kind, -1);
        return 1;
    }
    return 0;
}

/* Act-outs: Actor::RollActOutCombatStartTurnData picks one weighted entry of the trait's
 * combat_start_turn_act_outs (including "nothing"). */
/* At the start of a hero's turn an affliction may "act out" instead of obeying:
 * random commands, attacking an ally, hurting themselves, barking stress at
 * the party... Returns 1 if the act-out used the whole turn. */
int stress_turn_start(Battle *b, int idx) {
    Actor *a = &b->actors[idx];
    if (!a->is_hero || !a->hero->affliction) return 0;
    const DDTrait *t = &dd_traits[a->hero->affliction - 1];
    float total = 0;
    for (int i = 0; i < t->act_count; i++) {
        const DDActOut *o = &dd_actouts[t->act_first + i];
        if (o->hero_cls < 0 || o->hero_cls == a->cls) total += o->weight;
    }
    if (total <= 0) return 0;
    float x = rng_float(&b->rng) * total;
    const DDActOut *pick = NULL;
    for (int i = 0; i < t->act_count && !pick; i++) {
        const DDActOut *o = &dd_actouts[t->act_first + i];
        if (o->hero_cls >= 0 && o->hero_cls != a->cls) continue;
        if (x < o->weight) pick = o;
        else x -= o->weight;
    }
    if (!pick || pick->type == ACT_NOTHING) return 0;
    battle_event(b, EV_ACTOUT, idx, -1, pick->type, -1);

    switch (pick->type) {
    case ACT_BARK_STRESS:
        for (int i = 0; i < MAX_ACTORS; i++) {
            Actor *o = &b->actors[i];
            if (i != idx && o->used && o->alive && o->is_hero && pick->effect >= 0)
                effects_apply(b, pick->effect, idx, i, 1, 0);
        }
        return 0;
    case ACT_CHANGE_POS: {
        int d = pick->value != 0 ? (int)pick->value : (rng_range(&b->rng, 0, 1) ? 1 : -1);
        battle_shift(b, idx, d);
        return 1;
    }
    case ACT_IGNORE_COMMAND:
    case ACT_RETREAT:
        return 1;
    case ACT_RANDOM_COMMAND:
        battle_hero_auto(b, idx);
        return 1;
    case ACT_ATTACK_SELF:
        battle_damage(b, idx, (int)ceilf(a->max_hp * pick->value), idx, 0);
        return 1;
    case ACT_ATTACK_FRIENDLY: {
        int allies[MAX_ACTORS], n = 0;
        for (int i = 0; i < MAX_ACTORS; i++)
            if (i != idx && b->actors[i].used && b->actors[i].alive && b->actors[i].is_hero) allies[n++] = i;
        if (!n) return 1;
        int v = allies[rng_range(&b->rng, 0, n - 1)];
        battle_damage(b, v, (int)ceilf(b->actors[v].max_hp * pick->value), idx, 0);
        if (pick->effect >= 0) effects_apply(b, pick->effect, idx, v, 1, 0);
        return 1;
    }
    case ACT_MARK_SELF:
        if (pick->effect >= 0) effects_apply(b, pick->effect, idx, idx, 1, 0);
        return 0;
    case ACT_STRESS_HEAL_SELF:
    case ACT_BUFF_RANDOM_ALLY:
    case ACT_STRESS_HEAL_PARTY:
    case ACT_BUFF_PARTY: {
        if (pick->effect < 0) return 0;
        if (pick->type == ACT_STRESS_HEAL_SELF) { effects_apply(b, pick->effect, idx, idx, 1, 0); return 0; }
        int allies[MAX_ACTORS], n = 0;
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b->actors[i].used && b->actors[i].alive && b->actors[i].is_hero && (i != idx || pick->type == ACT_BUFF_PARTY || pick->type == ACT_STRESS_HEAL_PARTY))
                allies[n++] = i;
        if (!n) return 0;
        if (pick->type == ACT_BUFF_RANDOM_ALLY) effects_apply(b, pick->effect, idx, allies[rng_range(&b->rng, 0, n - 1)], 1, 0);
        else for (int k = 0; k < n; k++) effects_apply(b, pick->effect, idx, allies[k], 1, 0);
        return 0;
    }
    case ACT_HEAL_SELF:
        battle_heal(b, idx, (int)ceilf(a->max_hp * pick->value));
        return 0;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- dungeon / town */

/* Walking a hallway tile: darkness and long corridors stress each hero by
 * chance. Retreating (backwards) uses its own chance/amount. */
int stress_on_corridor_step(Hero *party[], int n, int torch, int level, int backwards, uint32_t seed) {
    /* rules.json hallway_stress: forward 30% for 2, backing up 55% for 5, scaled by darkness */
    Rng r;
    rng_seed(&r, seed);
    float dc, dd;
    darkness(torch, &dc, &dd);
    float chance = (backwards ? g_rules->hall[3] : g_rules->hall[1]) + dc;
    float base = backwards ? g_rules->hall[2] : g_rules->hall[0];
    int hit = 0;
    for (int i = 0; i < n; i++) {
        Hero *h = party[i];
        if (!h || !h->alive || !rng_chance(&r, chance)) continue;
        int amount = (int)roundf(base * trait_stress_mult(h, NULL) * (1 + diff_stress[diff_index(level, h)] + dd));
        raw_stress(NULL, h, -1, amount, level, &r);
        hit++;
    }
    return hit;
}

/* Flat stress from curios/traps (negative = relief). */
void stress_add(Hero *h, int amount, int level, uint32_t seed) {
    Rng r;
    rng_seed(&r, seed);
    raw_stress(NULL, h, -1, amount, level, &r);
}

/* A week in the hamlet: relief is applied and any affliction/virtue clears. */
void stress_on_town_week(Hero *h, int relief) {
    if (relief <= 0) return;
    h->stress = (int16_t)(h->stress > relief ? h->stress - relief : 0);
    /* a stay at the Tavern or Abbey also ends the affliction */
    if (h->affliction && dd_traits[h->affliction - 1].type == TRAIT_AFFLICTION) h->affliction = 0;
}

/* Back from a trip: virtues fade away. */
void stress_on_expedition_end(Hero *h) {
    h->nebuffs = 0;
    memset(h->bleed, 0, sizeof(h->bleed));
    memset(h->blight, 0, sizeof(h->blight));
    if (h->affliction && dd_traits[h->affliction - 1].type == TRAIT_VIRTUE) h->affliction = 0;
    h->deaths_door_visits = 0;
}

/* Name of the current trait for UI/AI, and whether it is a virtue. */
const char *stress_affliction_name(const Hero *h, int *virtue) {
    if (!h->affliction) return NULL;
    const DDTrait *t = &dd_traits[h->affliction - 1];
    if (virtue) *virtue = t->type == TRAIT_VIRTUE;
    return t->name;
}
