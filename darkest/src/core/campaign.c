/*
 * campaign.c - the town-side game state: roster, recruits, quest board,
 * weekly progression, building upgrades, trinket stash/wagon, and save/load.
 */
#include "campaign.h"
#include "quirk.h"
#include "town_event.h"
#include "trinket.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "stress.h"

/* Save-file identity: magic = "PSDD" tag; version must match the struct
 * layout. BUMP SAVE_VERSION whenever Campaign/Hero/Dungeon/QuestOffer change. */
#define SAVE_MAGIC 0x44445350u /* "PSDD" */
#define SAVE_VERSION 24
#define SAVE_PATH "save.bin"

/* The one global campaign instance. */
Campaign g_camp;

/* Pool of random hero names. */
static const char *const first_names[] = {
    "Reynauld", "Dismas", "Junia", "Paracelsus", "Audrey", "Tardif", "Baldwin", "Alhazred",
    "Missandei", "Boudica", "Damian", "Ingrid", "Osric", "Wilhelm", "Margit", "Eldon",
    "Helena", "Corwin", "Sibyl", "Garrick", "Berta", "Lucan", "Mirela", "Tobias",
};

/* Max HP out of battle: armour-tier HP scaled by quirk and trinket bonuses,
 * rounded, never below 1. (battle.c's base_stat uses the same formula.) */
int hero_max_hp(const Hero *h) {
    float hp = dd_heroes[h->cls].armour[h->armour_lvl].hp * (1 + quirk_stat(h, STAT_MAX_HP, 0) + trinket_stat(h, STAT_MAX_HP, 0));
    return hp < 1 ? 1 : (int)(hp + 0.5f);
}

void hero_generate(Hero *h, Rng *r, int cls) {
    memset(h, 0, sizeof(*h));
    h->cls = (int16_t)cls;
    snprintf(h->name, sizeof(h->name), "%s", first_names[rng_range(r, 0, (int)(sizeof(first_names) / sizeof(first_names[0])) - 1)]);
    /* four random distinct starting skills, like the original's skill generation */
    int n = dd_heroes[cls].skill_count, picked = 0;
    int order[8];
    for (int i = 0; i < n && i < 8; i++) order[i] = i;
    for (int i = n - 1; i > 0; i--) {
        int j = rng_range(r, 0, i), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int i = 0; i < HERO_SKILL_SLOTS; i++) h->equipped[i] = -1;
    int g = dd_hero_guaranteed[cls]; /* generation_guaranteed skill first, then random ones */
    if (g >= 0 && g < n) h->equipped[picked++] = (int8_t)g;
    for (int i = 0; i < n && picked < HERO_SKILL_SLOTS; i++)
        if (order[i] != g) h->equipped[picked++] = (int8_t)order[i];
    /* hero generation: number_of_positive/negative_quirks_min 1 .. max 2 */
    for (int k = rng_range(r, 1, 2); k > 0; k--) quirk_add(h, quirk_random(r, h, 1, 0), r);
    for (int k = rng_range(r, 1, 2); k > 0; k--) quirk_add(h, quirk_random(r, h, 0, 0), r);
    h->hp = (int16_t)hero_max_hp(h);
    h->alive = 1;
}

/* The Shrieker steals unequipped trinkets; we remember them here so a quest
 * can win them back. Keeps the newest 40. */
void hoard_add(int t) {
    if (t < 0) return;
    for (int q = 0; q < DD_PLOT_ALL; q++) /* Quest::System::GetIsTrinketValidForRetention: plot rewards are kept out */
        if (dd_plot_quests[q].trinket == t) return;
    if (g_camp.nhoard >= 40) { /* keep the newest */
        memmove(&g_camp.hoard[0], &g_camp.hoard[1], sizeof(g_camp.hoard[0]) * 39);
        g_camp.nhoard = 39;
    }
    g_camp.hoard[g_camp.nhoard++] = (int16_t)t;
}

/* wiki Shrieker: the Perch stays offered while 8+ lost trinkets wait; its level follows the rarest
 * one (uncommon apprentice, rare veteran, very rare champion); commons alone never call it */
int hoard_perch(void) {
    if (g_camp.nhoard < HOARD_RETURN) return -1;
    int best = -1;
    for (int i = 0; i < g_camp.nhoard; i++) {
        int r = dd_trinkets[g_camp.hoard[i]].rarity, lv = r == TR_UNCOMMON ? 0 : r == TR_RARE ? 1 : r >= TR_VERY_RARE ? 2 : -1;
        if (lv > best) best = lv;
    }
    return best < 0 ? -1 : PQ_PERCH + best;
}

void campaign_log(const char *fmt, ...) {
    if (g_camp.nlog >= LOG_MAX) { /* keep the newest */
        memmove(g_camp.log[0], g_camp.log[1], sizeof(g_camp.log[0]) * (LOG_MAX - 1));
        g_camp.nlog = LOG_MAX - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_camp.log[g_camp.nlog++], sizeof(g_camp.log[0]), fmt, ap);
    va_end(ap);
    g_camp.log_unseen = 1;
}

/* Add resolve XP; loop because one big award can skip levels. Returns 1 if
 * any level was gained (and logs it for the hamlet screen). */
int hero_add_xp(Hero *h, int xp) {
    h->xp = (uint16_t)(h->xp + xp);
    int up = 0;
    while (h->level < 6 && h->xp >= g_rules->level_xp[h->level + 1]) {
        h->level++;
        up = 1;
    }
    if (up && h->alive) campaign_log("%s is now %s %s (Lvl. %d).", h->name, dd_ui_str[US_RESOLVE_TITLE + h->level], dd_heroes[h->cls].name, h->level);
    return up;
}

/* New stage-coach recruits for the week. How many comes from the coach's
 * upgrade level; the very first coach is scripted (see comment below). */
void campaign_refresh_recruits(Rng *r) {
    /* stage_coach first_hero_classes: the first coach after the Old Road brings a Plague Doctor
     * and a Vestal, plus a random hero for each of Reynauld and Dismas lost on the road (wiki) */
    int first = g_camp.week <= 2;
    int n = (int)tree_value("stage_coach.numrecruits", TV_AMOUNT, 2);
    if (first && g_camp.week == 2 && g_camp.nroster < 2) n += 2 - g_camp.nroster;
    if (n > RECRUITS) n = RECRUITS;
    for (int i = 0; i < RECRUITS; i++) {
        g_camp.recruits[i].alive = 0;
        if (i >= n) continue;
        int cls = rng_range(r, 0, dd_hero_count - 1);
        if (first)
            for (int c = 0; c < dd_hero_count; c++)
                if (strcmp(dd_heroes[c].id, i == 0 ? "plague_doctor" : "vestal") == 0) cls = c;
        hero_generate(&g_camp.recruits[i], r, cls);
    }
}

/* A dungeon opens after enough quests have been completed overall. */
int dungeon_unlocked(int dungeon) { return g_camp.quests_done >= dd_dungeon_unlock[dungeon]; }

/* Buildings appear in town as progress is made (quest-count thresholds). */
int building_unlocked(const char *id) {
    static const struct { const char *id; int quests; } req[] = {
        {"tavern", 2}, {"abbey", 2}, {"blacksmith", 3}, {"guild", 3}, {"sanitarium", 4},
    };
    if (strcmp(id, "camping_trainer") == 0) return g_camp.highest_level >= 3;
    for (unsigned i = 0; i < sizeof(req) / sizeof(req[0]); i++)
        if (strcmp(req[i].id, id) == 0) return g_camp.quests_done >= req[i].quests;
    return 1;
}

/* Roll this week's quest board. Number of quests depends on progress; each
 * quest's dungeon, difficulty (from the roster's levels), type and length are
 * drawn from weighted tables in the original's quest generation data
 * (quest.generation.json: quests per visit by progress, dungeon unlocks,
 * difficulty from the roster's resolve levels, type/length weights). */
void campaign_make_board(Rng *r) {
    g_camp.nboard = 0;
    int row = g_camp.quests_done < 8 ? g_camp.quests_done : 7;
    int want = dd_quests_per_visit[row] < BOARD_MAX ? dd_quests_per_visit[row] : BOARD_MAX;
    int levels[3], nl = 0, per_dungeon[DG_COUNT] = {0};
    static const int lv[3] = {1, 3, 5}, lo[3] = {0, 2, 4}, hi[3] = {2, 4, 6};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < g_camp.nroster; i++)
            if (g_camp.roster[i].level >= lo[k] && g_camp.roster[i].level <= hi[k]) { levels[nl++] = lv[k]; break; }
    if (!nl) levels[nl++] = 1;
    for (int guard = 0; g_camp.nboard < want && guard < 200; guard++) {
        int d = rng_range(r, 0, DG_REGULAR - 1);
        if (!dungeon_unlocked(d) || per_dungeon[d] >= 4) continue;
        float total = 0;
        for (int i = 0; i < dd_quest_option_count; i++)
            if (dd_quest_options[i].dungeon == d && dd_quest_options[i].row == row && dd_quest_options[i].quest <= QT_BOSS)
                total += dd_quest_options[i].chance;
        if (total <= 0) continue;
        float x = rng_float(r) * total;
        const DDQuestOption *o = NULL;
        for (int i = 0; i < dd_quest_option_count && !o; i++) {
            const DDQuestOption *c = &dd_quest_options[i];
            if (c->dungeon != d || c->row != row || c->quest > QT_BOSS) continue;
            if (x < c->chance) o = c;
            else x -= c->chance;
        }
        if (!o) continue;
        QuestOffer *q = &g_camp.board[g_camp.nboard++];
        q->dungeon = (uint8_t)d;
        q->level = (uint8_t)levels[rng_range(r, 0, nl - 1)];
        q->length = o->length;
        q->quest = o->quest;
        q->plot = 0;
        q->heir = (uint8_t)rng_range(r, 0, HEIR_COUNT - 1);
        per_dungeon[d]++;
    }
    /* the next Darkest Dungeon expedition is always offered (plot quests unlock in order;
     * dungeon_level 0, no resolve level cap: quest.restriction.json difficulty 6 = 99) */
    int perch = hoard_perch(); /* the Shrieker's Perch while its hoard holds 8+ lost trinkets */
    if (perch >= 0 && g_camp.nboard < BOARD_MAX) town_event_post_quest(perch);
    /* plot_tutorial_crypts (is_progression): the first Ruins quest, offered after the Old Road until done */
    if (!(g_camp.plot_done & 2) && g_camp.quests_done >= 1 && g_camp.nboard < BOARD_MAX) town_event_post_quest(PQ_TUTORIAL);
    if (g_camp.dd_done < DD_PLOT_QUESTS && !g_camp.game_won && g_camp.nboard < BOARD_MAX) {
        const DDPlotQuest *pq = &dd_plot_quests[g_camp.dd_done];
        QuestOffer *q = &g_camp.board[g_camp.nboard++];
        q->dungeon = DG_DARKEST;
        q->level = 6;
        q->length = pq->length;
        q->quest = pq->quest <= QT_BOSS ? pq->quest : QT_BOSS;
        q->plot = (uint8_t)(PLOT_DD + g_camp.dd_done);
        q->heir = HEIR_CREST;
    }
}

/* New game setup: reset struct, seed, apply mode rules, hand out the two
 * scripted starting heroes (Reynauld, Dismas), build the Old Road dungeon. */
void campaign_new(uint32_t seed, int mode) {
    memset(&g_camp, 0, sizeof(g_camp));
    g_camp.magic = SAVE_MAGIC;
    g_camp.version = SAVE_VERSION;
    g_camp.seed = seed;
    g_camp.gold = 0; /* scripts/starting_save/persist.estate.json */
    g_camp.week = 1;
    g_camp.event = g_camp.event_force = -1;
    g_camp.mode = (uint8_t)(mode >= 0 && mode < GM_COUNT ? mode : GM_DARKEST);
    g_rules = &dd_rules[g_camp.mode];
    Rng r;
    rng_seed(&r, seed);
    /* scripts/starting_save: heirlooms in the wallet, Reynauld and Dismas (level 0, stress 10,
     * fixed quirks and skills, persist.roster.json), and the game begins inside the Old Road
     * with 2 provisions (persist.raid.json). */
    static const int heir_start[HEIR_COUNT] = {10, 10, 10, 20}; /* bust, portrait, deed, crest */
    for (int k = 0; k < HEIR_COUNT; k++) g_camp.heirlooms[k] = (uint16_t)heir_start[k];
    static const struct { const char *cls, *name, *quirks[3], *skills[4]; } start[2] = {
        {"crusader", "Reynauld", {"warrior_of_light", "god_fearing", "kleptomaniac"},
         {"smite", "zealous_accusation", "stunning_blow", "bulwark_of_faith"}},
        {"highwayman", "Dismas", {"hard_noggin", "quick_reflexes", "known_cheat"},
         {"opened_vein", "pistol_shot", "grape_shot_blast", "take_aim"}},
    };
    for (int i = 0; i < 2; i++)
        for (int c = 0; c < dd_hero_count; c++)
            if (strcmp(dd_heroes[c].id, start[i].cls) == 0) {
                Hero *h = &g_camp.roster[g_camp.nroster++];
                hero_generate(h, &r, c);
                snprintf(h->name, sizeof(h->name), "%s", start[i].name);
                h->stress = 10;
                memset(h->quirks, 0, sizeof(h->quirks));
                for (int k = 0; k < 3; k++)
                    for (int q = 0; q < dd_quirk_count; q++)
                        if (strcmp(dd_quirks[q].id, start[i].quirks[k]) == 0) quirk_add(h, q, &r);
                for (int k = 0; k < HERO_SKILL_SLOTS; k++) h->equipped[k] = -1;
                for (int k = 0, n = 0; k < 4; k++)
                    for (int sk = 0; sk < dd_heroes[c].skill_count; sk++)
                        if (strcmp(dd_skills[dd_heroes[c].skill_first + sk * 5].id, start[i].skills[k]) == 0)
                            h->equipped[n++] = (int8_t)sk;
                h->hp = (int16_t)hero_max_hp(h);
            }
    for (int i = 0; i < 4; i++) g_camp.party[i] = i < g_camp.nroster ? i : -1;
    dungeon_tutorial(&g_camp.dungeon, &r);
    g_camp.dungeon.items[ITEM_FOOD] = 2;
    g_camp.in_dungeon = 1;
    campaign_refresh_recruits(&r);
    wagon_restock(&r);
}

/* Move recruit i into the roster (if there is room). 0 = ok. */
int campaign_hire(int i) {
    if (g_camp.nroster >= campaign_roster_size() || !g_camp.recruits[i].alive) return -1;
    g_camp.roster[g_camp.nroster++] = g_camp.recruits[i];
    g_camp.recruits[i].alive = 0;
    return 0;
}

/* Remove a roster hero (releasing party slots and shifting indices). */
void campaign_dismiss(int idx) {
    if (idx < 0 || idx >= g_camp.nroster) return;
    for (int s = 0; s < HERO_TRINKETS; s++) trinket_unequip(&g_camp.roster[idx], s); /* back to the estate */
    for (int p = 0; p < 4; p++) {
        if (g_camp.party[p] == idx) g_camp.party[p] = -1;
        else if (g_camp.party[p] > idx) g_camp.party[p]--;
    }
    memmove(&g_camp.roster[idx], &g_camp.roster[idx + 1], (g_camp.nroster - idx - 1) * sizeof(Hero));
    g_camp.nroster--;
}

/* A hero died: record in the graveyard, then dismiss from the roster. */
void campaign_bury(int idx) {
    if (idx < 0 || idx >= g_camp.nroster) return;
    const Hero *h = &g_camp.roster[idx];
    g_camp.deaths++;
    campaign_log("%s the %s has died.", h->name, dd_heroes[h->cls].name);
    if (g_camp.nfallen == GRAVEYARD_MAX) {
        memmove(&g_camp.graveyard[0], &g_camp.graveyard[1], (GRAVEYARD_MAX - 1) * sizeof(Fallen));
        g_camp.nfallen--;
    }
    Fallen *f = &g_camp.graveyard[g_camp.nfallen++];
    memcpy(f->name, h->name, sizeof(f->name));
    f->cls = h->cls;
    f->level = h->level;
    f->week = (int16_t)g_camp.week;
    campaign_dismiss(idx);
}

/* Stygian mode ends the campaign after a week limit or too many deaths. */
int campaign_check_game_over(void) {
    if (g_camp.mode == GM_STYGIAN && !g_camp.game_won && !g_camp.game_over) {
        if (g_rules->week_limit && g_camp.week > g_rules->week_limit) g_camp.game_over = 1;
        else if (g_rules->death_limit && g_camp.deaths >= g_rules->death_limit) g_camp.game_over = 2;
    }
    return g_camp.game_over;
}

/* Activity log line from a building story: $B activity, $H hero, $N number, $Q quirk, $T trinket,
 * $D description, $A affliction (export_game_data.py log_story). */
/* Format a tavern/abbey story line, substituting $H hero, $B building, etc. */
static void act_log(const char *t, int act, const Hero *h, int num, const char *q, const char *tr, const char *desc) {
    if (!t || !*t) return;
    char out[200];
    int n = 0;
    for (; *t && n < (int)sizeof(out) - 1; t++) {
        if (*t != '$' || !t[1]) { out[n++] = *t; continue; }
        const char *s = "";
        char num_s[12];
        switch (*++t) {
        case 'B': s = dd_activities[act].name; break;
        case 'H': s = h->name; break;
        case 'N': snprintf(num_s, sizeof(num_s), "%d", num); s = num_s; break;
        case 'Q': s = q ? q : ""; break;
        case 'T': s = tr ? tr : ""; break;
        case 'D': s = desc ? desc : ""; break;
        case 'A': s = h->act_cured ? dd_traits[h->act_cured - 1].name : ""; break;
        }
        n += snprintf(out + n, sizeof(out) - n, "%s", s);
        if (n >= (int)sizeof(out)) n = sizeof(out) - 1;
    }
    out[n] = 0;
    campaign_log("%s", out);
}

/* Give a hero an activity-granted expedition buff. */
static void act_buff(Hero *h, int bi) {
    if (bi < 0 || h->nebuffs >= HERO_BUFFS) return;
    const DDBuff *b = &dd_buffs[bi];
    HeroBuff *e = &h->ebuffs[h->nebuffs++];
    e->stat = b->stat; e->sub = b->sub; e->mode = b->mode;
    e->battles = -1; /* town buffs last the next expedition */
    e->amount = b->amount;
}

/* Week end for a hero who spent it in the Tavern or Abbey (busy 1..6): the stay's log line, then
 * the building's side effects (side_effects.chance, one weighted result). */
/* Resolve one hero's stay at the tavern or abbey: stress relief, cures, and
 * random side effects (quirks, lost gold, going missing...). */
static void activity_week_end(Hero *h, Rng *r) {
    int act = h->busy - 1;
    const DDActivity *a = &dd_activities[act];
    act_log(h->act_cured ? a->cured : a->relief, act, h, h->act_relief, NULL, NULL, NULL);
    h->act_relief = h->act_cured = 0;
    if (!a->count || !rng_chance(r, a->chance)) return;
    float tot = 0;
    for (int k = 0; k < a->count; k++) tot += dd_act_sides[a->first + k].weight;
    float x = rng_float(r) * tot;
    const DDActSide *s = &dd_act_sides[a->first];
    for (int k = 0; k < a->count; k++) {
        s = &dd_act_sides[a->first + k];
        if ((x -= s->weight) < 0) break;
    }
    switch (s->type) {
    case AS_LOCK: h->committed = h->busy; act_log(s->story, act, h, 0, NULL, NULL, NULL); break;
    case AS_MISSING:
        h->missing = (uint8_t)s->a;
        h->missing_act = h->busy;
        act_log(s->story, act, h, 0, NULL, NULL, NULL);
        break;
    case AS_QUIRK:
        if (quirk_add(h, s->a, r)) act_log(s->story, act, h, 0, dd_quirks[s->a].name, NULL, NULL);
        break;
    case AS_BUFF: act_buff(h, s->a); act_buff(h, s->b); act_log(s->story, act, h, 0, NULL, NULL, s->desc); break;
    case AS_GOLD_ADD: g_camp.gold += s->a; act_log(s->story, act, h, s->a, NULL, NULL, NULL); break;
    case AS_GOLD_LOSE: {
        int lost = g_camp.gold < s->a ? g_camp.gold : s->a;
        g_camp.gold -= lost;
        if (lost) act_log(s->story, act, h, lost, NULL, NULL, NULL);
        break;
    }
    case AS_TRINKET_ADD: {
        int t = trinket_roll(r, s->a, -1);
        if (t >= 0 && stash_add(t) == 0) act_log(s->story, act, h, 0, NULL, dd_trinkets[t].name, NULL);
        break;
    }
    case AS_TRINKET_LOSE: { /* a random trinket of that rarity from the estate chest */
        int idx[STASH_MAX], n = 0;
        for (int i = 0; i < g_camp.nstash; i++)
            if (dd_trinkets[g_camp.stash[i]].rarity == s->a) idx[n++] = i;
        if (!n) break;
        int i = idx[rng_range(r, 0, n - 1)], t = g_camp.stash[i];
        stash_remove(i);
        act_log(s->story, act, h, 0, NULL, dd_trinkets[t].name, NULL);
        break;
    }
    }
}

/* End of week in town: resolve activities, heal idle heroes, tick missing
 * heroes, bring new recruits/wagon stock/quest board, roll a town event, and
 * apply Stygian limits. */
void campaign_next_week(void) {
    g_camp.week++;
    Rng r;
    rng_seed(&r, g_camp.seed + (uint32_t)g_camp.week * 977u);
    campaign_refresh_recruits(&r);
    for (int i = 0; i < g_camp.nboard; i++) { /* upgrade_tags_to_remove_on_ignore: the brigands burn what was built */
        const QuestOffer *q = &g_camp.board[i];
        if (q->plot < PLOT_DD) continue;
        /* Upgrade::RemoveUpgradesFromPlotQuest (decomp16): walks the purchase records, no random
         * pick; we take back the most recent purchases */
        for (int k = 0; k < dd_plot_quests[q->plot - PLOT_DD].ignore_upgrades && g_camp.nupgrade_log; k++) {
            int t = g_camp.upgrade_log[--g_camp.nupgrade_log];
            if (t < TREES_MAX && g_camp.upgrades[t]) g_camp.upgrades[t]--;
        }
    }
    campaign_make_board(&r);
    wagon_restock(&r);
    town_event_roll(&r);
    for (int i = 0; i < g_camp.nroster; i++) {
        Hero *h = &g_camp.roster[i];
        int out = 0; /* idle: stayed in the hamlet, neither on the last expedition nor busy or missing */
        for (int r = 0; r < 4; r++) out |= g_camp.party[r] == i;
        if (!out && !h->busy) h->stress = (int16_t)(h->stress > g_rules->idle_heal ? h->stress - g_rules->idle_heal : 0);
        h->hp = (int16_t)hero_max_hp(h); /* a week in the hamlet heals wounds */
        if (h->missing) { /* away from the hamlet: back once the weeks run out */
            if (--h->missing == 0 && h->missing_act >= 1 && h->missing_act <= 6)
                act_log(dd_activities[h->missing_act - 1].found, h->missing_act - 1, h, 0, NULL, NULL, NULL);
            h->busy = h->missing ? MISSING_BUSY : 0;
            continue;
        }
        if (h->busy >= 1 && h->busy <= 6 && !h->committed) activity_week_end(h, &r);
        else if (h->committed) { /* activity_lock: the extra week is spent, free */
            h->committed = 0;
        }
        h->busy = h->missing ? MISSING_BUSY : h->committed; /* committed heroes stay in the building this week */
    }
}

/* ---------------------------------------------------------------- trinkets
 * The estate chest ("stash") holds unequipped trinkets; heroes wear 2. */

/* Put a trinket in the chest. -1 if full. */
int stash_add(int t) {
    if (t < 0 || g_camp.nstash >= STASH_MAX) return -1;
    g_camp.stash[g_camp.nstash++] = (int16_t)t;
    return 0;
}

/* Take entry i out (compacts the array). */
void stash_remove(int i) {
    if (i < 0 || i >= g_camp.nstash) return;
    memmove(&g_camp.stash[i], &g_camp.stash[i + 1], (g_camp.nstash - i - 1) * sizeof(g_camp.stash[0]));
    g_camp.nstash--;
}

/* Equip chest item i onto hero slot s; a currently worn one swaps back into
 * the chest. Returns NULL on success or a human-readable refusal reason. */
const char *trinket_equip(Hero *h, int s, int i) {
    if (i < 0 || i >= g_camp.nstash) return "Nothing to equip.";
    int t = g_camp.stash[i];
    const char *why = trinket_equip_error(h, t, s);
    if (why) return why;
    stash_remove(i);
    if (h->trinkets[s]) stash_add(h->trinkets[s] - 1); /* a slot just freed: always fits */
    h->trinkets[s] = (int16_t)(t + 1);
    h->hp = (int16_t)(h->hp > hero_max_hp(h) ? hero_max_hp(h) : h->hp);
    return NULL;
}

/* Move a worn trinket back to the chest. */
int trinket_unequip(Hero *h, int s) {
    if (!h->trinkets[s]) return 0;
    if (stash_add(h->trinkets[s] - 1) != 0) return -1;
    h->trinkets[s] = 0;
    if (h->hp > hero_max_hp(h)) h->hp = (int16_t)hero_max_hp(h);
    return 0;
}

/* nomad_wagon.building.json trinket_supply: stock size by upgrade, rarity_generation_table */
/* Nomad Wagon: weekly stock of trinkets for sale, chosen by rarity weights. */
void wagon_restock(Rng *r) {
    static const int weight[5] = {6, 5, 4, 2, 1}; /* very common .. very rare */
    memset(g_camp.wagon, 0, sizeof(g_camp.wagon));
    for (int i = 0; i < wagon_count(); i++) {
        int x = rng_range(r, 0, 17), rar = 0;
        while (rar < 4 && x >= weight[rar]) x -= weight[rar++];
        int t = trinket_roll(r, rar, -1);
        g_camp.wagon[i] = (int16_t)(t + 1);
    }
}

/* How many items are still unsold. */
int wagon_count(void) {
    int n = (int)tree_value("nomad_wagon.numitems", TV_AMOUNT, 2);
    return n > WAGON_MAX ? WAGON_MAX : n;
}

/* Price of a trinket after the building's cost upgrades. */
int wagon_price(int t) {
    float d = tree_value("nomad_wagon.cost", TV_DISCOUNT, 0);
    return town_event_upgrade_cost(3, (int)(dd_trinkets[t].price * (1 - d) + 0.5f));
}

/* trinket_sell_value_discount_upgrades: 0.85 off the price */
int trinket_sell_value(int t) { return (int)(dd_trinkets[t].price * 0.15f + 0.5f); }

/* ---------------------------------------------------------------- building upgrades
 * Each building has an upgrade "tree" (dd_trees). g_camp.upgrades[tree] =
 * levels bought. The numbers a building uses (slots, discounts, costs) come
 * from dd_tree_values via tree_value(). */

/* Index of an upgrade tree by string id, or -1. */
int tree_find(const char *id) {
    for (int i = 0; i < dd_tree_count && i < TREES_MAX; i++)
        if (strcmp(dd_trees[i].id, id) == 0) return i;
    return -1;
}

/* Levels bought in a tree. */
int tree_level(const char *id) {
    int t = tree_find(id);
    return t < 0 ? 0 : g_camp.upgrades[t];
}

/* The effective value of a tree stat: base value, overridden by the highest
 * bought level that defines it (discounts stack from all levels instead). */
float tree_value(const char *id, int key, float base) {
    int t = tree_find(id);
    if (t < 0) return base;
    int lvl = g_camp.upgrades[t], best = -1;
    float v = base, sum = 0;
    for (int i = 0; i < dd_tree_value_count; i++) {
        const DDTreeValue *e = &dd_tree_values[i];
        if (e->tree != t || e->key != key || e->level > lvl || e->mode != g_camp.mode) continue;
        if (key == TV_DISCOUNT) sum += e->value;
        else if (e->level > best) { best = e->level; v = e->value; }
    }
    return key == TV_DISCOUNT ? sum : v;
}

/* Pay for the next level of a tree (gold + heirlooms). */
int tree_buy(int t) {
    if (t < 0 || t >= dd_tree_count || t >= TREES_MAX) return -1;
    int lvl = g_camp.upgrades[t];
    if (lvl >= dd_trees[t].levels) return -1;
    const uint16_t *c = dd_trees[t].cost[lvl];
    if (g_camp.nupgrade_log < sizeof(g_camp.upgrade_log)) g_camp.upgrade_log[g_camp.nupgrade_log++] = (uint8_t)t;
    if (town_event_upgrade_cost(2, 1) == 0) { /* town event: one building upgrade free */
        town_event_paid(2);
        g_camp.upgrades[t]++;
        return 0;
    }
    for (int k = 0; k < HEIR_COUNT; k++)
        if (g_camp.heirlooms[k] < c[k]) return -2;
    if (g_camp.gold < c[4]) return -2;
    for (int k = 0; k < HEIR_COUNT; k++) g_camp.heirlooms[k] -= c[k];
    g_camp.gold -= c[4];
    g_camp.upgrades[t]++;
    return 0;
}

/* Max roster size, grown by the stage-coach upgrades. */
int campaign_roster_size(void) {
    int n = (int)tree_value("stage_coach.rostersize", TV_AMOUNT, 9);
    return n > ROSTER_MAX ? ROSTER_MAX : n;
}

/* Cost after the guild/blacksmith discount upgrades. */
static int discounted(int cost, const char *tree) {
    float d = tree_value(tree, TV_DISCOUNT, 0);
    return (int)(cost * (1 - (d > 0.9f ? 0.9f : d)) + 0.5f);
}

/* Equipment tier n needs blacksmith.<weapon|armour> level n; skill rank n needs guild.skill_levels n. */
int upgrade_weapon_cost(const Hero *h) {
    if (h->weapon_lvl >= 4 || tree_level("blacksmith.weapon") <= h->weapon_lvl) return -1;
    const DDUpgrades *u = &dd_upgrades[g_camp.mode * dd_hero_count + h->cls];
    if (h->level < u->weapon_level[h->weapon_lvl]) return -1;
    return town_event_upgrade_cost(1, discounted(u->weapon_cost[h->weapon_lvl], "blacksmith.cost"));
}

int upgrade_armour_cost(const Hero *h) {
    if (h->armour_lvl >= 4 || tree_level("blacksmith.armour") <= h->armour_lvl) return -1;
    const DDUpgrades *u = &dd_upgrades[g_camp.mode * dd_hero_count + h->cls];
    if (h->level < u->armour_level[h->armour_lvl]) return -1;
    return town_event_upgrade_cost(0, discounted(u->armour_cost[h->armour_lvl], "blacksmith.cost"));
}

int upgrade_skill_cost(const Hero *h, int skill) {
    if (skill < 0 || skill >= 8 || h->skill_lvl[skill] >= 4) return -1;
    const DDUpgrades *u = &dd_upgrades[g_camp.mode * dd_hero_count + h->cls];
    int code = h->skill_lvl[skill] + 1; /* code 0 = learning it, which starting heroes already have */
    if (h->level < u->skill_level[skill][code] || tree_level("guild.skill_levels") < code) return -1;
    return discounted(u->skill_cost[skill][code], "guild.cost");
}

/* Dump the whole campaign struct to save.bin (see campaign.h for the rules). */
int campaign_save(void) { return plat_write_file(SAVE_PATH, &g_camp, sizeof(g_camp)); }

/* Read it back; accept only if size, magic and version all match. */
int campaign_load(void) {
    int size = 0;
    Campaign *c = plat_read_file(SAVE_PATH, &size);
    if (!c) return -1;
    int ok = size == (int)sizeof(Campaign) && c->magic == SAVE_MAGIC && c->version == SAVE_VERSION;
    if (ok) {
        g_camp = *c;
        g_rules = &dd_rules[g_camp.mode < GM_COUNT ? g_camp.mode : GM_DARKEST];
    }
    free(c);
    return ok ? 0 : -2;
}
