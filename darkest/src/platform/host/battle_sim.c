/*
 * battle_sim.c - a headless COMBAT BALANCE SIMULATOR. It builds a fixed
 * 4-hero party, plays many battles using only the engine in src/core (no
 * graphics), with a simple automatic player, and prints win rates and round
 * counts. Used to tune and sanity-check combat rules.
 * Host test for the combat engine: plays battles with simple hero AI and prints a log plus
 * statistics. Usage: battle_sim [seed] [battles] [dungeon 0-3] [level 1/3/5] */
#include "dd_load.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "battle.h"
#include "dungeon.h"

/* Class index by string id, or -1. */
static int find_hero(const char *id) {
    for (int i = 0; i < dd_hero_count; i++)
        if (strcmp(dd_heroes[i].id, id) == 0) return i;
    return -1;
}

/* Build a fresh hero of a class at a resolve level with sensible defaults. */
static void make_hero(Hero *h, const char *cls, int level) {
    memset(h, 0, sizeof(*h));
    h->cls = (int16_t)find_hero(cls);
    snprintf(h->name, sizeof(h->name), "%s", dd_heroes[h->cls].name);
    h->level = (uint8_t)level;
    h->weapon_lvl = h->armour_lvl = (uint8_t)(level > 4 ? 4 : level);
    for (int i = 0; i < 8; i++) h->skill_lvl[i] = (uint8_t)(level > 4 ? 4 : level);
    for (int i = 0; i < HERO_SKILL_SLOTS; i++) h->equipped[i] = (int8_t)i;
    h->alive = 1;
}

/* Event type -> short name for the log. */
static const char *ev_name(int t) {
    static const char *n[] = {"TURN", "SKILL", "MISS", "DODGE", "HIT", "CRIT", "HEAL", "STUN", "RESIST",
                              "BLEED", "BLIGHT", "DOT", "DEATHS_DOOR", "DEATHBLOW_RESISTED", "DEATH",
                              "CORPSE", "MOVE", "BUFF", "DEBUFF", "MARK", "GUARD", "RIPOSTE", "STUN_SKIP",
                              "STRESS", "STRESS_HEAL", "SUMMON", "TORCH", "WON", "LOST",
                              "AFFLICTION", "VIRTUE", "HEART_ATTACK", "ACTOUT", "REFUSE",
                              "CAPTURED", "RELEASED", "CONTROLLED", "SHAPESHIFT", "BONUS_TURN", "TELEPORT", "STALL"};
    return t < (int)(sizeof(n) / sizeof(n[0])) ? n[t] : "?";
}

/* Dump the recorded events of the last action. */
static void print_events(Battle *b) {
    for (int i = 0; i < b->nevents; i++) {
        BattleEvent *e = &b->events[i];
        const char *an = e->actor >= 0 ? actor_name(&b->actors[e->actor]) : "-";
        const char *tn = e->target >= 0 ? actor_name(&b->actors[e->target]) : "-";
        if (e->type == EV_TURN) printf("\n[round %d] %s's turn (HP %d/%d)\n", e->value, an,
                                       b->actors[e->actor].hp, b->actors[e->actor].max_hp);
        else if (e->type == EV_SKILL) printf("  uses %s\n", dd_skills[e->skill].name);
        else printf("  %-18s %-22s <- %-22s %d\n", ev_name(e->type), an, tn, e->value);
    }
    b->nevents = 0;
}

/* The sim's "player": lists every (usable skill, valid target rank) pair and
 * picks one uniformly at random - a deliberately dumb baseline player. */
static int hero_choose(Battle *b, int actor, int *skill, int *rank) {
    int options[8][2], n = 0;
    for (int s = 0; s < HERO_SKILL_SLOTS; s++) {
        int sk = battle_hero_skill(b, actor, s);
        if (sk < 0 || !battle_skill_usable(b, actor, sk)) continue;
        int mask = battle_targets(b, actor, sk, NULL);
        for (int r = 1; r <= DD_RANKS && n < 8; r++)
            if (mask & (1 << (r - 1))) { options[n][0] = sk; options[n][1] = r; n++; }
    }
    if (!n) return 0;
    int pick = rng_range(&b->rng, 0, n - 1);
    *skill = options[pick][0];
    *rank = options[pick][1];
    return 1;
}

static Hero party[4];
static int chain; /* keep the party (and its stress) between battles */
static int boss;  /* fight the dungeon's boss encounters */
static int oldroad; /* the Old Road's final room: Reynauld + Dismas vs Bloodletter + Fusilier */
static int stuck;   /* hero turns with nothing to attack while monsters live */
static int force_enc = -1;
static int passive; /* "stall": heroes only pass (stall penalty test) */ /* argv[6]: fight this dd_encounters index */
static int surprised[3]; /* fights by SURPRISE_* outcome */

/* Play one whole battle with the given seed; returns the result code and
 * writes the number of rounds. This is the canonical example of the driver
 * loop documented in battle.h. */
static int run(uint32_t seed, int dungeon, int level, int verbose, int *rounds) {
    static const char *cls[4] = {"crusader", "highwayman", "vestal", "occultist"};
    int lv = level >= 5 ? 4 : level >= 3 ? 2 : 0;
    for (int i = 0; i < 4; i++) {
        if (!chain || !party[i].alive) {
            make_hero(&party[i], cls[i], lv);
        } else {
            party[i].hp = dd_heroes[party[i].cls].armour[party[i].armour_lvl].hp;
        }
    }
    /* vestal in rank 3, occultist in rank 4 suit their skills */
    Battle b;
    battle_init(&b, seed, chain ? 40 : 100);
    b.level = level;
    b.dungeon = oldroad ? -1 : dungeon;
    if (oldroad) { /* scripts/starting_save persist.roster.json selected_combat_skills */
        static const char *const ids[2][4] = {{"smite", "zealous_accusation", "stunning_blow", "bulwark_of_faith"},
                                              {"opened_vein", "pistol_shot", "grape_shot_blast", "take_aim"}};
        for (int h = 0; h < 2; h++) {
            make_hero(&party[h], h ? "highwayman" : "crusader", 0);
            const DDHeroClass *c = &dd_heroes[party[h].cls];
            for (int k = 0; k < 4; k++)
                for (int s = 0; s < c->skill_count; s++)
                    if (strstr(dd_skills[c->skill_first + s * 5].id, ids[h][k])) party[h].equipped[k] = (int8_t)s;
        }
    }
    for (int i = 0; i < (oldroad ? 2 : 4); i++) battle_add_hero(&b, &party[i], i + 1);
    Rng r;
    rng_seed(&r, seed * 7 + 1);
    int enc = force_enc >= 0 ? force_enc : oldroad ? dd_enc_tutorial[1] : dungeon_pick_encounter(&r, dungeon, level, boss ? ENC_BOSS : ENC_ROOM);
    const DDEncounter *e = &dd_encounters[enc];
    int rank = 1;
    for (int i = 0; i < e->count; i++) {
        battle_add_monster(&b, e->monster[i], rank);
        rank += dd_monsters[e->monster[i]].size ? dd_monsters[e->monster[i]].size : 1;
    }
    surprised[battle_roll_surprise(&b, 0, 0)]++;
    if (verbose) {
        printf("Encounter:");
        for (int i = 0; i < e->count; i++) printf(" %s", dd_monsters[e->monster[i]].name);
        printf("\n");
    }
    for (int turn = 0; turn < 300; turn++) {
        int a = battle_begin_turn(&b);
        if (a < 0) break;
        int skill, target;
        if (b.actors[a].is_hero && b.actors[a].controlled) {
            battle_hero_auto(&b, a);
            if (verbose) print_events(&b);
            else b.nevents = 0;
            continue;
        }
        int ok = b.actors[a].is_hero ? (!passive && hero_choose(&b, a, &skill, &target)) : battle_ai_choose(&b, a, &skill, &target);
        if (!ok && b.actors[a].is_hero && !b.actors[a].stunned) {
            stuck++;
            if (stuck <= 3) {
                printf("STUCK %s rank %d:", actor_name(&b.actors[a]), b.actors[a].rank);
                for (int i = 0; i < MAX_ACTORS; i++)
                    if (b.actors[i].used && b.actors[i].alive)
                        printf(" %s@%d%s", actor_name(&b.actors[i]), b.actors[i].rank, b.actors[i].is_corpse ? "(corpse)" : "");
                printf("\n");
            }
        }
        if (ok) battle_perform(&b, a, skill, target);
        else battle_pass(&b, a);
        if (verbose) print_events(&b);
        else b.nevents = 0;
    }
    *rounds = b.round;
    if (!b.over && !verbose) {
        printf("unfinished:");
        for (int i = 0; i < MAX_ACTORS; i++)
            if (b.actors[i].used && b.actors[i].alive && !b.actors[i].is_hero)
                printf(" %s(hp %d)", actor_name(&b.actors[i]), b.actors[i].hp);
        printf("\n");
    }
    return b.over;
}

/* Parse arguments, run N battles, print aggregate statistics. */
int main(int argc, char **argv) {
    {   /* game data blob (extractor output) next to the assets */
        const char *path = getenv("DD_DATA") ? getenv("DD_DATA") : "assets/gen/dd_data.bin";
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        void *buf = malloc(n);
        if (fread(buf, 1, n, f) != (size_t)n || dd_data_load(buf, (int)n) != 0) { fprintf(stderr, "bad %s\n", path); return 1; }
        fclose(f);
        free(buf);
        g_rules = &dd_rules[GM_DARKEST];
    }
    uint32_t seed = argc > 1 ? (uint32_t)atoi(argv[1]) : 1;
    int battles = argc > 2 ? atoi(argv[2]) : 1000;
    int dungeon = argc > 3 ? atoi(argv[3]) : DG_WEALD;
    int level = argc > 4 ? atoi(argv[4]) : 1;
    int rounds;
    chain = argc > 5 && strcmp(argv[5], "chain") == 0;
    boss = argc > 5 && strcmp(argv[5], "boss") == 0;
    oldroad = argc > 5 && strcmp(argv[5], "oldroad") == 0;
    passive = argc > 5 && strcmp(argv[5], "stall") == 0;
    if (argc > 6) force_enc = atoi(argv[6]);
    printf("=== one battle, seed %u ===\n", seed);
    run(seed, dungeon, level, 1, &rounds);
    int won = 0, lost = 0, timeout = 0, total_rounds = 0;
    int afflicted = 0, virtuous = 0, deaths = 0;
    for (int i = 0; i < battles; i++) {
        int r = run(seed + 1000 + i, dungeon, level, 0, &rounds);
        won += r == 1;
        lost += r == 2;
        timeout += r == 0;
        total_rounds += rounds;
        for (int h = 0; h < 4; h++) {
            if (!party[h].alive) { deaths++; continue; }
            if (party[h].affliction) {
                if (dd_traits[party[h].affliction - 1].type == TRAIT_VIRTUE) virtuous++;
                else afflicted++;
                if (chain) { party[h].affliction = 0; party[h].stress = 0; } /* as if they rested */
            }
        }
    }
    printf("surprise: none %d, monsters %d, heroes %d\n", surprised[0], surprised[1], surprised[2]);
    printf("hero-battles ending afflicted %d, virtuous %d, hero deaths %d\n", afflicted, virtuous, deaths);
    if (oldroad) printf("stuck hero turns: %d\n", stuck);
    printf("\n=== %d battles in %s level %d: won %d, lost %d, unfinished %d, avg rounds %.1f ===\n",
           battles, dd_dungeon_names[dungeon], level, won, lost, timeout, (float)total_rounds / battles);
    return 0;
}
