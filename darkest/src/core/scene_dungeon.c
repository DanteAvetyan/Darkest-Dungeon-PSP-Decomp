/*
 * scene_dungeon.c - exploring an expedition: corridors, rooms, curios, traps,
 * camping, hunger, the map and the results screens.
 *
 * STATE MACHINE (DState): the scene is always in exactly one state:
 *   DS_INTRO       loading card (quest title, art, tip) before starting
 *   DS_ROOM        standing in a room       DS_CORRIDOR  walking a hall
 *   DS_MAP         choosing the next room   DS_MENU      start menu (camp, leave)
 *   DS_INVENTORY   backpack                 DS_CURIO/LOOT  using a curio / loot found
 *   DS_MEAL/CAMP   eating / camping         DS_QUESTDONE   goal reached, continue?
 *   DS_RESULTS     reward pages             DS_DONE        returning to town
 * dungeon_update() switches on `st` and calls that state's update function.
 *
 * WALKING MODEL: in a corridor the party's position is a pixel coordinate
 * d->xpos along the hall. Each hall square is TILE pixels wide; walking onto
 * a new square runs enter_square() which triggers its content (battle, trap,
 * hunger...). Reaching either end enters the neighbouring room.
 *
 * Battles are launched by start_fight() -> scene_switch(SCENE_BATTLE); when the
 * battle scene finishes, dungeon_enter() runs again and reads the result.
 *
 * Dungeon exploration. Rooms are joined by corridors you walk through by hand.
 * Corridor: hold Left/Right to walk, X at a curio, Square inventory, Triangle torch,
 * Select map, Start menu (camp, leave). Room: X curio / map, same other buttons. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "audio.h"
#include "campaign.h"
#include "hud.h"
#include "quirk.h"
#include "town_event.h"
#include "trinket.h"
#include "curio.h"
#include "platform.h"
#include "scenes.h"
#include "stress.h"
#include "text.h"
#include "ui.h"

/* Tunables: geometry in pixels, walking speed, and rules.json numbers for
 * hunger, food healing, camp points and ambush chance. */
#define TILE 272.0f        /* one corridor square = one wall panel */
#define DOOR 150.0f        /* space before the first / after the last square */
#define WALK_SPEED 80.0f   /* px/s at full stride */
#define WALK_ACCEL 220.0f
#define HUNGER_STEPS 14    /* new squares between hunger checks */
#define STARVE_HP 0.2f     /* rules.json hallway_hunger_starve_HPdmg */
#define STARVE_STRESS 15   /* rules.json meals_table "none" */
#define FOOD_HEAL 0.05f    /* rules.json provision_hp_heal */
#define RESPITE 12         /* rules.json camp_start_camping_points */
#define AMBUSH_CHANCE 0.33f /* rules.json ambush_camping_base_chance */
#define WALLS_MAX 10
#define VIEW_Y (-52.0f)    /* backgrounds keep their scale; the HUD covers the floor edge */
#define FLOOR_Y 170.0f

/* Scene states, see the file comment. */
typedef enum { DS_INTRO, DS_QUESTDONE, DS_RESULTS, DS_ROOM, DS_CORRIDOR, DS_MAP, DS_MENU, DS_INVENTORY, DS_CURIO, DS_MEAL, DS_CAMP, DS_LOOT, DS_DONE } DState;

/* Scene globals: `st` the state, art images for the current surroundings,
 * menu cursors, walk velocity and message timers. */
static DState st;
static Image bg_far, bg_mid, bg_room, bg_door, walls[WALLS_MAX];
static int nwalls;
static char loaded_room[96], fight_bg[96];
static int map_cursor, menu_sel, inv_sel, curio_sel, curio_items[ITEM_COUNT], ncurio_items;
static float vel, anim_t, msg_t;
static Image intro_img; /* the loading card art */
/* raid_results: what the two results pages show (filled by end_expedition) */
static struct {
    int kind;                  /* 0 completed, 1 escaped, 2 defeat */
    int quest_gold, quest_heir, heir_type, gold;
    int heirlooms[HEIR_COUNT];
    int ntrinkets;
    int page;
    struct { char name[16]; int16_t cls; uint8_t alive, level, xp, level_up; char qpos[32], qneg[32]; } hero[4];
    int nhero;
} res;
#define RES_ANIM 1.6f
static float res_t; /* seconds on the current results page (animation) */
static Image res_bg, res_frames, res_seal;
static const char *quest_title(const Dungeon *d);
static int facing = 1, last_seg = -1, travel_dir = 1;
static char msg[112];
static int returning_battle;
static int hud_rank; /* party member shown in the HUD (L/R) */
/* camp */
static Image camp_icons[64];
static uint8_t camp_icon_tried[64];

/* Camping-skill icon (lazy loaded). */
static Image *camp_icon(int skill) {
    if (skill < 0 || skill >= 64) return NULL;
    if (!camp_icon_tried[skill]) {
        char p[80];
        snprintf(p, sizeof(p), "assets/camp/%s.ddi", dd_camp_skills[skill].id);
        image_load(&camp_icons[skill], p);
        camp_icon_tried[skill] = 1;
    }
    return camp_icons[skill].tiles ? &camp_icons[skill] : NULL;
}

/* Free the camp icons. */
static void camp_icons_free(void) {
    for (int i = 0; i < 64; i++) image_free(&camp_icons[i]);
    memset(camp_icon_tried, 0, sizeof(camp_icon_tried));
}

static int ambushed;     /* fighting a night ambush: light returns after */
static int teleport_to = -1; /* room the party was teleported into (entered once the scene is up) */
static float ambush_cut; /* reduce_ambush_chance from tonight's camp skills */
static int camp_points, camp_hero, camp_skill, camp_target, camp_phase, camp_uses[64];

static Dungeon *D(void) { return &g_camp.dungeon; }
/* Plot art set (export_ui.py renames walls to corridor_wall.NN / room_wall.rN): 0..3 dd1..dd4,
 * PQ_INVASION the town; -1 = the dungeon's regular art (also the Shrieker's weald perch). */
/* Which fixed-map art set (Old Road / Darkest Dungeon n) applies, if any. */
static int plot_art(void) {
    int qn = DUNGEON_DD_QUEST(D());
    return qn >= 0 && qn <= PQ_INVASION ? qn : -1;
}
/* Asset folder for this dungeon's backgrounds. */
static const char *dir_name(void) {
    static char dd[16];
    int a = plot_art();
    if (a < 0 || a == PQ_INVASION) return dd_dungeon_dirs[D()->dungeon];
    snprintf(dd, sizeof(dd), "dd%d", a + 1); /* export_ui.py: one art set per quest */
    return dd;
}

/* Colour grade set of this expedition: dungeons/<dir>/grade_N.lut (the Shrieker's perch has its own). */
/* Exposes the colour-grade folder to other code (see assets.c grade_scene). */
const char *dungeon_grade_dir(void) {
    static char g[24];
    int qn = DUNGEON_DD_QUEST(D());
    if (qn == PQ_CROW) snprintf(g, sizeof(g), "%s/crow", dd_dungeon_dirs[D()->dungeon]);
    else if (qn >= PQ_PERCH && qn < PQ_PERCH + 3) snprintf(g, sizeof(g), "%s/perch%d", dd_dungeon_dirs[D()->dungeon], qn - PQ_PERCH);
    else if (plot_art() >= 0 && plot_art() < DD_PLOT_QUESTS) snprintf(g, sizeof(g), "dd%d", plot_art() + 1);
    else snprintf(g, sizeof(g), "%s", dd_dungeon_dirs[D()->dungeon]);
    return g;
}

/* Show a short message in the status line for a couple of seconds. */
static void say(const char *s) {
    snprintf(msg, sizeof(msg), "%s", s);
    msg_t = 3.0f;
}

/* ---------------------------------------------------------------- party */

/* The roster hero in party slot r (0..3) or NULL. */
static Hero *party_hero(int r) {
    int h = g_camp.party[r];
    return h >= 0 && g_camp.roster[h].alive ? &g_camp.roster[h] : NULL;
}

/* Compact the party into a list of Hero pointers; returns the count. */
static int party_list(Hero *out[4]) {
    int n = 0;
    for (int r = 0; r < 4; r++)
        if (party_hero(r)) out[n++] = party_hero(r);
    return n;
}

/* ---------------------------------------------------------------- art */

static const char *const room_walls[DG_COUNT][6] = {
    {"altar", "arch", "barrels", "drain", "library", "torture"},
    {"clearing", "corruptedcabin", "crypt", "gate", "handtree", "shroomland"},
    {"brickton", "duct", "effigy", "meatlocker", "shrine", "sluice"},
    {"city", "coral", "grotto", "shipwreck", "temple", "whale"},
};
static const int wall_count[DG_COUNT] = {7, 10, 6, 7}, wall_first[DG_COUNT] = {0, 1, 1, 1};
/* Darkest Dungeon quests 1-4: corridor walls and random room walls in each art set */
static const int dd_walls[PQ_INVASION + 1] = {5, 6, 5, 1, 5}, dd_rooms[PQ_INVASION + 1] = {4, 3, 3, 0, 2};

/* Load the far/mid/door/wall backgrounds for this dungeon type. */
static void load_dungeon_art(void) {
    char p[96];
    image_free(&bg_far);
    image_free(&bg_mid);
    image_free(&bg_door);
    for (int i = 0; i < nwalls; i++) image_free(&walls[i]);
    snprintf(p, sizeof(p), "assets/dungeons/%s/corridor_bg.ddi", dir_name());
    image_load(&bg_far, p);
    snprintf(p, sizeof(p), "assets/dungeons/%s/corridor_mid.ddi", dir_name());
    image_load(&bg_mid, p);
    snprintf(p, sizeof(p), "assets/dungeons/%s/corridor_door.basic.ddi", dir_name());
    image_load(&bg_door, p);
    int qn = plot_art();
    nwalls = qn >= 0 ? dd_walls[qn] : wall_count[D()->dungeon];
    for (int i = 0; i < nwalls; i++) {
        snprintf(p, sizeof(p), "assets/dungeons/%s/corridor_wall.%02d.ddi", dir_name(), (qn >= 0 ? 0 : wall_first[D()->dungeon]) + i);
        image_load(&walls[i], p);
    }
}

/* Pick and load the room's wall art (the stored random `wall` value chooses
 * among the variants so a room always looks the same). */
static void room_background(int room) {
    char p[96];
    const Room *r = &D()->rooms[room];
    int pq = DUNGEON_DD_QUEST(D());
    if (pq == PQ_CROW || (pq >= PQ_PERCH && pq < PQ_PERCH + 3)) { /* the Shrieker's nest: final_room_wall.plot_* */
        if (pq == PQ_CROW) snprintf(p, sizeof(p), "assets/dungeons/weald/plot_crow.ddi");
        else snprintf(p, sizeof(p), "assets/dungeons/weald/plot_perch%d.ddi", pq - PQ_PERCH);
        if (strcmp(p, loaded_room) == 0 && bg_room.tiles) return;
        image_free(&bg_room);
        image_load(&bg_room, p);
        snprintf(loaded_room, sizeof(loaded_room), "%s", p);
        return;
    }
    if (D()->plot == PLOT_OLD_ROAD) { /* weald.tutorial.room_wall.effigy_0/1: the wrecked coach, then the road */
        snprintf(p, sizeof(p), "assets/dungeons/weald/oldroad_%s.ddi", r->content == SPOT_ENTRANCE ? "entrance" : "final");
        if (strcmp(p, loaded_room) == 0 && bg_room.tiles) return;
        image_free(&bg_room);
        image_load(&bg_room, p);
        snprintf(loaded_room, sizeof(loaded_room), "%s", p);
        return;
    }
    int qn = plot_art();
    if (r->content == SPOT_ENTRANCE)
        snprintf(p, sizeof(p), "assets/dungeons/%s/entrance_room_wall.ddi", dir_name());
    else if (qn >= 0 && (r->content == SPOT_BOSS || !dd_rooms[qn]))
        snprintf(p, sizeof(p), "assets/dungeons/%s/final_room_wall.ddi", dir_name());
    else if (qn >= 0)
        snprintf(p, sizeof(p), "assets/dungeons/%s/room_wall.r%d.ddi", dir_name(), r->wall % dd_rooms[qn]);
    else
        snprintf(p, sizeof(p), "assets/dungeons/%s/room_wall.%s.ddi", dir_name(), room_walls[D()->dungeon][r->wall % 6]);
    if (strcmp(p, loaded_room) == 0 && bg_room.tiles) return;
    image_free(&bg_room);
    if (image_load(&bg_room, p) != 0) {
        snprintf(p, sizeof(p), "assets/dungeons/%s/entrance_room_wall.ddi", dir_name());
        image_load(&bg_room, p);
    }
    snprintf(loaded_room, sizeof(loaded_room), "%s", p);
}

/* ---------------------------------------------------------------- corridor geometry */

static Corridor *cur_corr(void) { return D()->corridor >= 0 ? &D()->corr[D()->corridor] : NULL; }
static float corr_len(const Corridor *c) { return 2 * DOOR + c->nseg * TILE; }
static float seg_center(int s) { return DOOR + (s + 0.5f) * TILE; }
/* Which hall square the x position is over (or -1 in the door areas). */
static int seg_at(const Corridor *c, float x) {
    int s = (int)floorf((x - DOOR) / TILE);
    return s < 0 || s >= c->nseg ? -1 : s;
}

/* ---------------------------------------------------------------- expedition end */

/* The expedition is over (success / escape / defeat). Settle everything:
 * pay gold and heirlooms, move loot into the estate, award XP, roll new
 * quirks, handle deaths (graveyard), stress and affliction cleanup, register
 * quest completion and prepare the results screen data in `res`. */
static void end_expedition(int success) {
    Dungeon *d = D();
    int xp = 0, gold = d->gold;
    memset(&res, 0, sizeof(res));
    res.gold = d->gold;
    res.ntrinkets = d->ntrinkets;
    for (int k = 0; k < HEIR_COUNT; k++) res.heirlooms[k] = d->heirlooms[k];
    if (success) {
        res.quest_gold = dungeon_reward(d);
        res.quest_heir = dungeon_heirloom_reward(d);
        res.heir_type = d->heir % HEIR_COUNT;
    }
    if (success) {
        gold += dungeon_reward(d);
        g_camp.heirlooms[d->heir % HEIR_COUNT] += dungeon_heirloom_reward(d);
        xp = (d->level >= 5 ? 6 : d->level >= 3 ? 4 : 2) + (d->length == LEN_MEDIUM);
        /* town event embark buff resolve_xp_bonus_percent (for its dungeon) */
        int8_t ev = d->town_event;
        for (int k = 0; ev >= 0 && ev < dd_town_event_count && k < dd_town_events[ev].op_count; k++) {
            const DDTownEventOp *o = &dd_town_event_ops[dd_town_events[ev].op_first + k];
            if (o->type == TE_EMBARK_BUFF && (o->arg & 15) == TB_RESOLVE_XP && ((o->arg >> 4) == 0 || (o->arg >> 4) - 1 == d->dungeon))
                xp = (int)(xp * (1 + o->num) + 0.5f);
        }
        if (!d->plot) town_event_quest_done(d->dungeon, d->quest);
        int b = dungeon_boss_room(d); /* caretaker goals: which boss fell, at which level */
        if (b >= 0 && d->rooms[b].encounter >= 0 && !d->plot) {
            static const char *const keys[8] = {"necromancer", "prophet", "hag", "cannon", "swine_prince", "formless", "siren", "drowned"};
            const char *id = dd_monsters[dd_encounters[d->rooms[b].encounter].monster[0]].id;
            int lv = d->level >= 5 ? 2 : d->level >= 3 ? 1 : 0;
            for (int k = 0; k < 8; k++)
                if (strstr(id, keys[k])) g_camp.boss_done |= 1u << (k * 3 + lv);
        }
    }
    int qn = DUNGEON_DD_QUEST(d);
    if (qn >= PQ_INVASION) { /* the town-event quests: trinket rewards and the Shrieker's quirks */
        const DDPlotQuest *pq = &dd_plot_quests[qn];
        Rng tr;
        rng_seed(&tr, plat_ticks_ms() ^ 0xc0f);
        if (success) {
            xp = pq->xp;
            if (pq->trinket >= 0) stash_add(pq->trinket);
            if (pq->trinket_rarity >= 0) {
                int t = trinket_roll(&tr, pq->trinket_rarity, -1);
                if (t >= 0) stash_add(t);
            }
            if (qn == PQ_INVASION) g_camp.plot_done |= 1;
            if (qn >= PQ_PERCH && qn < PQ_PERCH + 3) { /* the Perch gives back 8 lost trinkets */
                /* order unknown (wiki, binary): rarest first, oldest first within a rarity (our choice) */
                int n = g_camp.nhoard < HOARD_RETURN ? g_camp.nhoard : HOARD_RETURN;
                for (int k = 0; k < n; k++) {
                    int best = 0;
                    for (int i = 1; i < g_camp.nhoard; i++)
                        if (dd_trinkets[g_camp.hoard[i]].rarity > dd_trinkets[g_camp.hoard[best]].rarity) best = i;
                    stash_add(g_camp.hoard[best]);
                    memmove(&g_camp.hoard[best], &g_camp.hoard[best + 1], sizeof(g_camp.hoard[0]) * (g_camp.nhoard - best - 1));
                    g_camp.nhoard--;
                }
                campaign_log("%d lost trinkets were recovered from the Shrieker's nest.", n);
            }
            if (qn == PQ_TUTORIAL) g_camp.plot_done |= 2;
        }
        int n = success ? pq->nwin : pq->nlose, tot = 0;
        const int16_t *qs = success ? pq->win_quirk : pq->lose_quirk;
        const uint8_t *ws = success ? pq->win_w : pq->lose_w;
        for (int i = 0; i < n; i++) tot += ws[i];
        /* party_quirks_to_apply_*: one weighted quirk to one random hero of the party (wiki Shrieker:
         * "a quirk will be randomly chosen ... and given to a hero in the party") */
        Hero *cand[4];
        int nc = 0;
        for (int r = 0; r < 4; r++)
            if (party_hero(r)) cand[nc++] = party_hero(r);
        if (tot > 0 && nc) {
            int x = rng_range(&tr, 0, tot - 1), k = 0;
            while (k < n - 1 && x >= ws[k]) x -= ws[k++];
            quirk_add(cand[rng_range(&tr, 0, nc - 1)], qs[k], &tr);
        }
        qn = -1; /* nothing below is about the Darkest Dungeon */
    }
    if (success && qn >= 0) {
        /* plot_darkest_dungeon_N: resolve_xp, roster stress cleared, the party never returns */
        xp = dd_plot_quests[qn].xp;
        if (qn + 1 > g_camp.dd_done) g_camp.dd_done = (uint8_t)(qn + 1);
        if (qn == DD_PLOT_QUESTS - 1) g_camp.game_won = 1;
        for (int i = 0; i < g_camp.nroster; i++) g_camp.roster[i].stress = 0;
        for (int r = 0; r < 4; r++)
            if (party_hero(r)) party_hero(r)->dd_veteran = 1;
        /* additional_trinket_completion_rewards: one very rare trinket; the first quest also hands
         * out three Talismans of the Flame (trinket_unlock dd_trinket 3) for the Templars' Revelation */
        Rng tr;
        rng_seed(&tr, plat_ticks_ms() ^ 0x7a1);
        int t = trinket_roll(&tr, TR_VERY_RARE, -1);
        if (t >= 0) stash_add(t);
        for (int i = 0; qn == 0 && i < dd_trinket_count; i++)
            if (strcmp(dd_trinkets[i].id, "dd_trinket") == 0)
                for (int k = 0; k < 3; k++) stash_add(i);
    }
    g_camp.gold += gold;
    for (int k = 0; k < HEIR_COUNT; k++) g_camp.heirlooms[k] += d->heirlooms[k];
    /* trinkets found reach the estate unless the whole party fell; those the fallen wore go to the
     * Shrieker's hoard (Raid::GenerateRaidFinishResults 0xf42b20: per dead hero, wipe or not) */
    int survivors = 0;
    for (int r = 0; r < 4; r++) survivors += party_hero(r) && party_hero(r)->alive;
    for (int i = 0; i < g_camp.nroster; i++)
        if (!g_camp.roster[i].alive) {
            for (int k = 0; k < HERO_TRINKETS; k++)
                if (g_camp.roster[i].trinkets[k]) hoard_add(g_camp.roster[i].trinkets[k] - 1);
            memset(g_camp.roster[i].trinkets, 0, sizeof(g_camp.roster[i].trinkets));
        }
    for (int i = 0; survivors && i < d->ntrinkets; i++) stash_add(d->trinkets[i]);
    d->ntrinkets = 0;
    if (survivors) g_camp.journal |= d->journal; /* pages found on the way */
    for (int r = 0; r < 4; r++) {
        int hi = g_camp.party[r];
        if (hi >= 0 && hi < g_camp.nroster) { /* the results page lists the fallen too */
            const Hero *x = &g_camp.roster[hi];
            memcpy(res.hero[res.nhero].name, x->name, sizeof(x->name));
            res.hero[res.nhero].cls = x->cls;
            res.hero[res.nhero].alive = x->alive;
            res.nhero++;
        }
        Hero *h = party_hero(r);
        if (h && success) {
            res.hero[res.nhero - 1].level_up = (uint8_t)(hero_add_xp(h, xp) > 0);
            res.hero[res.nhero - 1].xp = (uint8_t)xp;
        }
        if (h) {
            Rng qr;
            rng_seed(&qr, plat_ticks_ms() ^ (uint32_t)(r * 7919));
            char *qp = res.hero[res.nhero - 1].qpos, *qn = res.hero[res.nhero - 1].qneg;
            quirks_after_expedition(h, success, &qr, qp, qn, sizeof(res.hero[0].qpos));
            if (qp[0]) campaign_log("%s: +%s", h->name, qp);
            if (qn[0]) campaign_log("%s: -%s", h->name, qn);
            res.hero[res.nhero - 1].level = h->level;
            stress_on_expedition_end(h);
        }
    }
    for (int i = g_camp.nroster - 1; i >= 0; i--)
        if (!g_camp.roster[i].alive) campaign_bury(i);
    if (success || d->plot) { /* the Old Road counts as cleared even if everyone falls (wiki) */
        for (int k = 0; k < DG_DARKEST; k++)
            if (dd_dungeon_unlock[k] == g_camp.quests_done + 1)
                campaign_log("%s: %s has been unlocked.", dd_dungeon_names[k], dd_dungeon_names[k]);
        g_camp.quests_done++;
        if (d->level > g_camp.highest_level) g_camp.highest_level = d->level;
    }
    g_camp.in_dungeon = 0;
    int was_old_road = d->plot == PLOT_OLD_ROAD;
    campaign_next_week();
    if (was_old_road) g_camp.week = 1; /* the hamlet opens on week 1 after the Old Road */
    campaign_save();
    snprintf(msg, sizeof(msg), success ? "Quest complete! +%d gold" : "Expedition over. +%d gold", gold);
    if (success && qn == DD_PLOT_QUESTS - 1) snprintf(msg, sizeof(msg), "The Heart of Darkness is slain!");
    msg_t = 0;
    int alive = 0;
    for (int i = 0; i < res.nhero; i++) alive += res.hero[i].alive;
    res.kind = success ? 0 : alive ? 1 : 2;
    res.page = 0;
    res_t = 0;
    st = DS_RESULTS;
}

/* ---------------------------------------------------------------- events on the way */

/* Battle background for corridor square s of the current corridor. */
/* Background used for a battle fought on hall square s. */
static const char *corridor_wall_bg(int s) {
    static char wall[96];
    Dungeon *d = D();
    snprintf(wall, sizeof(wall), "assets/dungeons/%s/corridor_wall.%02d.ddi", dir_name(),
             (plot_art() >= 0 ? 0 : wall_first[d->dungeon]) + (nwalls ? cur_corr()->wall[s] % nwalls : 0));
    return wall;
}

/* Remember where we are and hand over to the battle scene. */
static void start_fight(int encounter, const char *bg) {
    snprintf(fight_bg, sizeof(fight_bg), "%s", bg);
    returning_battle = 1;
    campaign_save();
    battle_scene_start(encounter, fight_bg);
}

/* Arrive in a room: mark visited, scout neighbours, and trigger a battle or
 * curio if one is waiting. */
static void enter_room(int room) {
    Dungeon *d = D();
    audio_sfx("gen_map_roomtransition");
    audio_dungeon_ambience(d->dungeon, d->torch);
    if (d->torch <= 15) audio_narrate_once("bad_torchlight", 55);
    d->room = room;
    d->corridor = -1;
    if (!d->rooms[room].visited) { /* scouting: rolled on the first visit to a room */
        Hero *p[4];
        int n = party_list(p);
        float bonus = 0;
        for (int i = 0; i < n; i++) bonus += quirk_stat(p[i], STAT_SCOUTING, 0) + trinket_stat(p[i], STAT_SCOUTING, 0);
        Rng r;
        rng_seed(&r, plat_ticks_ms() ^ (uint32_t)room * 2246822519u);
        int s = dungeon_scout(d, &r, bonus);
        if (s) say(s > 1 ? "Scouting! The way far ahead is revealed." : "Scouting! The way ahead is revealed.");
    }
    d->rooms[room].visited = 1;
    room_background(room);
    st = DS_ROOM;
    Room *rm = &d->rooms[room];
    if (rm->encounter >= 0 && !rm->done) {
        battle_scene_surprise(rm->scouted, d->plot >= PLOT_DD && DUNGEON_DD_QUEST(d) != PQ_TUTORIAL ? -1 : 0); /* is_surprise_enabled */
        start_fight(rm->encounter, loaded_room);
    }
    if (dungeon_goal_met(d)) say("Quest goal met! " BG_START " Return to town.");
}

/* Hunger check (a hall square or a step counter): if there is one food per
 * hero, everyone eats and heals 5% max HP; otherwise ALL food is lost and
 * each hero takes starvation damage (20% max HP, never below 1) and stress. */
static void hunger_check(void) {
    Hero *p[4];
    int n = party_list(p);
    Dungeon *d = D();
    if (d->items[ITEM_FOOD] >= n) {
        d->items[ITEM_FOOD] = (uint8_t)(d->items[ITEM_FOOD] - n);
        for (int i = 0; i < n; i++) {
            int heal = (int)(hero_max_hp(p[i]) * FOOD_HEAL + 0.5f);
            p[i]->hp = (int16_t)(p[i]->hp + heal > hero_max_hp(p[i]) ? hero_max_hp(p[i]) : p[i]->hp + heal);
        }
        say("The party is hungry and eats.");
    } else {
        d->items[ITEM_FOOD] = 0;
        for (int i = 0; i < n; i++) {
            int dmg = (int)(hero_max_hp(p[i]) * STARVE_HP + 0.5f);
            p[i]->hp = (int16_t)(p[i]->hp - dmg > 1 ? p[i]->hp - dmg : 1);
            stress_add(p[i], STARVE_STRESS, d->level, plat_ticks_ms() + (uint32_t)i);
        }
        say("No food! The party starves.");
        audio_narrate_important("bad_starve", 75);
    }
}

/* ---- traps (Overlay_Event::ShowTrap 0x109aa20, Hero::GetTrapDodgeChance 0xefbe70) ----
 * chance = trap resist (+ trap_scout_disarm_bonus when disarming a seen trap) - difficulty_trap_base[level].
 * A seen trap stops the party and the selected hero disarms it (success_effects: 8 stress healed);
 * an unseen one is walked into and a random hero rolls to dodge it (no stress heal). Either way a
 * failed roll sets it off on that hero: fail_effects plus a fraction of max HP. */
/* Trap definition for this dungeon type. */
static const DDTrap *trap_def(void) {
    int g = D()->dungeon;
    return g >= 0 && g < DG_COUNT && dd_traps[g].id ? &dd_traps[g] : NULL;
}
static int trap_row(void) { int l = D()->level; return l >= 6 ? 3 : l >= 5 ? 2 : l >= 3 ? 1 : 0; }
/* Chance this hero avoids/disarms a trap = hero's trap resistance (class +
 * quirks + trinkets) [+ scout disarm bonus when deliberately disarming] -
 * the trap difficulty for the dungeon level. */
static float trap_chance(const Hero *h, int disarm) {
    int l = D()->level;
    float c = dd_heroes[h->cls].res[RES_TRAP] + quirk_stat(h, STAT_RES, RES_TRAP) + trinket_stat(h, STAT_RES, RES_TRAP);
    return c + (disarm ? dd_trap_scout_bonus : 0) - dd_trap_base[l >= 0 && l < 7 ? l : 6];
}
/* A trap fires: failing heroes lose HP and take its fail effects. */
static void trap_set_off(Hero *party[], int n, int who, Rng *r) {
    const DDTrap *t = trap_def();
    Hero *h = party[who];
    char b[96];
    if (!t) return;
    audio_sfx_like("prop_trap_", t->id);
    audio_narrate("bad_trap", 25);
    int row = trap_row(), dmg = (int)(t->hp_loss[row] * hero_max_hp(h) + 0.5f);
    if (dmg > 0) h->hp = (int16_t)(h->hp - dmg > 1 ? h->hp - dmg : 1); /* outside battle nobody drops below 1 */
    for (int k = 0; k < 2; k++)
        if (t->fail[row][k] >= 0) hero_apply_effect_offline(D(), party, n, who, t->fail[row][k], r);
    if (dmg > 0) snprintf(b, sizeof(b), "%s %s -%d HP", dd_ui_str[US_TRAP], h->name, dmg);
    else snprintf(b, sizeof(b), "%s %s", dd_ui_str[US_TRAP], h->name);
    say(b);
}
/* Walking into an unseen trap: a random hero gets one dodge attempt. */
static void trap_walk_in(int s) {
    Corridor *c = cur_corr();
    Hero *p[4];
    int n = party_list(p);
    c->seg_done[s] |= 1 | 4; /* resolved; bit 2: shows sprung */
    if (!n || !trap_def()) return;
    Rng r;
    rng_seed(&r, plat_ticks_ms() * 31u + (uint32_t)s);
    int who = rng_range(&r, 0, n - 1);
    if (rng_float(&r) < trap_chance(p[who], 0)) {
        char b[96];
        snprintf(b, sizeof(b), "%s %s avoided it.", dd_ui_str[US_TRAP], p[who]->name);
        say(b);
    } else trap_set_off(p, n, who, &r);
}
/* Seen trap right in front of the party, or -1. */
/* A seen, unresolved trap directly ahead (to disarm with X), else -1. */
static int trap_here(void) {
    Corridor *c = cur_corr();
    if (!c || !c->scouted) return -1;
    for (int s = 0; s < c->nseg; s++)
        if (c->seg[s] == SPOT_TRAP && !(c->seg_done[s] & 1) && fabsf(D()->xpos - seg_center(s)) <= 48) return s;
    return -1;
}
/* Deliberate disarm attempt by the lead hero. */
static void trap_disarm(int s) {
    audio_sfx("prop_trap_disarm");
    Corridor *c = cur_corr();
    Hero *p[4];
    int n = party_list(p);
    if (!n) return;
    int who = hud_rank < n ? hud_rank : 0; /* the selected hero */
    Rng r;
    rng_seed(&r, plat_ticks_ms() * 77u + (uint32_t)s);
    c->seg_done[s] |= 1 | 2;
    const DDTrap *t = trap_def();
    if (rng_float(&r) < trap_chance(p[who], 1)) {
        if (t && t->success >= 0) hero_apply_effect_offline(D(), p, n, who, t->success, &r);
        char b[96];
        snprintf(b, sizeof(b), "%s disarmed the trap.", p[who]->name);
        say(b);
    } else {
        c->seg_done[s] |= 4;
        trap_set_off(p, n, who, &r);
    }
}

/* The party stepped into square s of the current corridor (moving in direction dir). */
/* THE core hall-walking hook. Called once per square entered:
 *   1. torch drops (less when re-walking a square)
 *   2. stress_on_corridor_step (darkness stress)
 *   3. bleed/blight tick (unless this square starts a fight)
 *   4. resolve content: battle (with surprise setup), hunger, ghost, trap */
static void enter_square(int s, int dir) {
    Dungeon *d = D();
    Corridor *c = cur_corr();
    int fresh = !(c->seg_done[s] & 2); /* bit 1: walked through before */
    if (DUNGEON_DD_QUEST(d) != 3) d->torch -= fresh ? g_rules->light_new : g_rules->light_old; /* tile_light_loss */ /* dd4: torch_setting */
    if (d->torch < 0) d->torch = 0;
    if (fresh) {
        c->seg_done[s] |= 2; /* bit 1: walked through */
        d->steps++;
    }
    Hero *p[4];
    int n = party_list(p);
    if (stress_on_corridor_step(p, n, d->torch, d->level, dir != travel_dir, plat_ticks_ms() ^ (uint32_t)s * 2654435761u))
        say("The darkness weighs on the party...");
    /* RaidDisplay::HandleStepOnNewTile (decomp26): a new tile that is not a fight ticks every hero's
     * bleed / blight once (Raid::ApplyDot) */
    if (!(c->seg[s] == SPOT_BATTLE && !(c->seg_done[s] & 1))) {
        int any = 0;
        for (int i = 0; i < n; i++) any |= hero_dot_tick(p[i]) > 0;
        if (any) say("Wounds bleed and fester on the march.");
    }
    if (c->seg_done[s] & 1) return; /* bit 0: event resolved */
    if (c->seg[s] == SPOT_BATTLE) {
        c->seg_done[s] |= 1;
        /* Mash::System::FillOutBattleFromMashGuide (decomp9): a hall battle first rolls the
         * conditional mash (Shambler in darkness, Collector with a full bag) against "nothing" */
        Rng r;
        rng_seed(&r, plat_ticks_ms() ^ (uint32_t)s * 40503u);
        int cond = dungeon_roll_conditional(&r, d);
        battle_scene_surprise(cond >= 0 ? 0 : d->rooms[c->a].scouted && d->rooms[c->b].scouted,
                              cond < 0 && d->plot >= PLOT_DD && DUNGEON_DD_QUEST(d) != PQ_TUTORIAL ? -1 : 0);
        start_fight(cond >= 0 ? cond : c->seg_enc[s], corridor_wall_bg(s));
    } else if (c->seg[s] == SPOT_HUNGER) {
        c->seg_done[s] |= 1;
        hunger_check(); /* map_generator.darkest hallway_hunger: placed hunger checks */
    } else if (c->seg[s] == SPOT_GHOST) {
        c->seg_done[s] |= 1; /* obstacle "ancestor": no health, no penalty, only his voice */
        say("A shade of the Ancestor stands in the way, then fades.");
    } else if (c->seg[s] == SPOT_TRAP && !c->scouted) {
        trap_walk_in(s); /* an unseen trap: a random hero tries to dodge it (a seen one blocks the way, X disarms) */
    }
}

/* Obstacle square right in front of the party, or -1. */
/* Obstacle square right ahead, or -1. */
static int obstacle_here(void) {
    Corridor *c = cur_corr();
    if (!c) return -1;
    for (int s = 0; s < c->nseg; s++)
        if (c->seg[s] == SPOT_OBSTACLE && !(c->seg_done[s] & 1) && fabsf(D()->xpos - seg_center(s)) <= 48) return s;
    return -1;
}

/* A shovel clears it cleanly; by hand the party is hurt and strained (DD's no-shovel clearing). */
/* Clear an obstacle: use a shovel if available, otherwise everyone pays
 * HP/stress and the torch dims. */
static void clear_obstacle(int s) {
    Dungeon *d = D();
    { static const char *const on[] = {"bad_obstacle_crypts", "bad_obstacle_weald", "bad_obstacle_warrens", "bad_obstacle_cove"}; if (d->dungeon < 4) audio_narrate(on[d->dungeon], 100); }
    audio_sfx_like("prop_obstacle_", dd_obstacles[d->dungeon < DG_COUNT ? d->dungeon : 0].id);
    cur_corr()->seg_done[s] |= 1;
    if (d->items[ITEM_SHOVEL]) {
        d->items[ITEM_SHOVEL]--;
        say("The shovel clears the way.");
        return;
    }
    /* prop type "obstacle" (prop_definitions.json): without a shovel every hero loses 5% max HP and
     * takes fail_effects (Stress 2), the torch drops 20; clear_on_failure clears it anyway */
    const DDObstacle *o = d->dungeon < DG_COUNT && dd_obstacles[d->dungeon].id ? &dd_obstacles[d->dungeon] : NULL;
    Hero *p[4];
    int n = party_list(p);
    Rng r;
    rng_seed(&r, plat_ticks_ms() * 13u + (uint32_t)s);
    for (int i = 0; o && i < n; i++) {
        int dmg = (int)(hero_max_hp(p[i]) * o->hp_loss + 0.5f);
        p[i]->hp = (int16_t)(p[i]->hp - dmg > 1 ? p[i]->hp - dmg : 1);
        if (o->fail >= 0) hero_apply_effect_offline(d, p, n, i, o->fail, &r);
    }
    if (o) d->torch = d->torch - o->torch > 0 ? d->torch - o->torch : 0;
    say("No shovel: the party clears it by hand.");
}

/* Curio within reach: returns its index (dd_curios), or -1. *seg_out = corridor square or -1 (room). */
/* Is a usable curio at the party's position? Gives its segment via seg_out. */
static int curio_here(int *seg_out) {
    Dungeon *d = D();
    *seg_out = -1;
    if (d->corridor < 0) {
        Room *rm = &d->rooms[d->room];
        return rm->curio != -1 && !rm->curio_done && (rm->encounter < 0 || rm->done) ? rm->curio : -1;
    }
    Corridor *c = cur_corr();
    for (int s = 0; s < c->nseg; s++)
        if (c->seg[s] == SPOT_CURIO && !(c->seg_done[s] & 1) && fabsf(d->xpos + facing * 30 - seg_center(s)) < 70) {
            *seg_out = s;
            return c->seg_enc[s];
        }
    return -1;
}

/* Open the curio menu (investigate or use an item). */
static void open_curio(void) {
    int seg, cu = curio_here(&seg);
    if (cu == CURIO_QUEST) { /* the Darkest Dungeon's goal (iron crown / locus beacon): activate it */
        Dungeon *d = D();
        d->rooms[d->room].curio_done = 1;
        d->goals_done++;
        int need = dd_plot_quests[DUNGEON_DD_QUEST(d)].goals;
        snprintf(msg, sizeof(msg), d->goals_done >= need ? "Activated. The way is lit: Start to leave." : "Activated (%d of %d).",
                 d->goals_done, need);
        msg_t = 3.5f;
        return;
    }
    if (cu < 0) return;
    ncurio_items = curio_usable_items(D(), cu, curio_items);
    curio_sel = 0;
    st = DS_CURIO;
}

/* Do it: run curio_use and react (loot window, teleport, summon). */
static void use_curio(int item) {
    Dungeon *d = D();
    int seg, cu = curio_here(&seg);
    if (cu < 0) { st = d->corridor >= 0 ? DS_CORRIDOR : DS_ROOM; return; }
    Hero *p[4];
    int n = party_list(p);
    curio_nloot = 0;
    menu_sel = 0;
    if (cu != CURIO_QUEST) audio_sfx_like("prop_curio_", dd_curios[cu].sprite);
    int res = curio_use(d, p, n, 0, cu, item, plat_ticks_ms() ^ g_camp.seed, msg, sizeof(msg));
    msg_t = 3.5f;
    if (seg >= 0) cur_corr()->seg_done[seg] |= 1;
    else d->rooms[d->room].curio_done = 1;
    st = curio_nloot ? DS_LOOT : d->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
    if (curio_nloot) { audio_sfx("ui_dun_loot_popup_chest"); audio_narrate("good_lootfirst", 8); }
    if (curio_nloot) msg_t = 0;
    if (res == CURIO_TELEPORT) {
        Rng r;
        rng_seed(&r, plat_ticks_ms());
        enter_room(rng_range(&r, 0, d->nrooms - 1));
    } else if (res == CURIO_SUMMON) {
        Rng r;
        rng_seed(&r, plat_ticks_ms());
        int e = dungeon_pick_encounter(&r, d->dungeon, d->level, ENC_ALTAR);
        if (e >= 0) {
            battle_scene_surprise(0, 0);
            start_fight(e, seg >= 0 ? corridor_wall_bg(seg) : loaded_room);
        }
    }
}

/* ---------------------------------------------------------------- scene
 * Per-frame logic: one update function per state, dispatched below. */

/* Scene enter. Two cases: (a) fresh arrival from town - set up art and the
 * intro card; (b) returning from a battle - read battle_scene_result() and
 * continue (victory, retreat, or defeat ends the expedition). */
void dungeon_enter(void) {
    audio_scene_music(2, D()->dungeon, 0);
    audio_dungeon_ambience(D()->dungeon, D()->torch);
    if (D()->dungeon < 4) { static const char *const ln[] = {"load_crypts", "load_weald", "load_warrens", "load_cove"}; audio_narrate(ln[D()->dungeon], 100); }
    Dungeon *d = D();
    sprite_cache_release_all();
    image_free(&intro_img); /* the loading card belongs to this expedition */
    loaded_room[0] = 0;
    load_dungeon_art();
    msg_t = 0;
    vel = 0;
    if (returning_battle) {
        returning_battle = 0;
        int res = battle_scene_result();
        Hero *p[4];
        if (!party_list(p)) {
            end_expedition(0);
            return;
        }
        if (res == 1 && d->corridor < 0) {
            d->rooms[d->room].done = 1;
            if (d->rooms[d->room].content == SPOT_BOSS) say("The boss is slain!");
        }
        if (res == 3) say("The party retreats.");
        if (res == 4) { /* a White Cell Stalk's teleport: thrown into a random dead-end room (wiki) */
            int cand[DG_MAX_ROOMS], n = 0, ex[4];
            for (int i = 0; i < d->nrooms; i++)
                if (i != d->room && d->rooms[i].content != SPOT_ENTRANCE && dungeon_room_exits(d, i, ex) == 1) cand[n++] = i;
            if (n) {
                Rng r;
                rng_seed(&r, plat_ticks_ms() ^ 0x7e1e);
                teleport_to = cand[rng_range(&r, 0, n - 1)];
                d->corridor = -1;
                d->room = teleport_to;
            }
        }
        if (ambushed) {
            ambushed = 0;
            d->torch = 100; /* the light meter resets when exploring resumes */
        }
        campaign_save();
    }
    if (d->corridor >= 0) {
        st = DS_CORRIDOR;
        last_seg = seg_at(cur_corr(), d->xpos);
        if (d->room >= 0) room_background(d->room);
    } else {
        room_background(d->room);
        st = DS_ROOM;
        if (!d->intro_done) { st = DS_INTRO; audio_narrate_reset(); } /* the loading card first; a new raid may repeat narrator lines */
        else if (!d->rooms[d->room].visited) enter_room(d->room); /* starting in a fight room */
        else if (dungeon_goal_met(d)) say("Quest goal met! " BG_START " Return to town.");
    }
    if (teleport_to >= 0) {
        int r = teleport_to;
        teleport_to = -1;
        enter_room(r);
        say("The flesh closes around you... and spits you out elsewhere.");
    }
}

/* Camping rules: must be in a room (not a hall) and have firewood; not at
 * the entrance, and not in a room whose battle is still unfinished. */
static int can_camp(void) {
    Dungeon *d = D();
    if (d->corridor >= 0 || !d->items[ITEM_FIREWOOD]) return 0;
    const Room *r = &d->rooms[d->room];
    return r->content != SPOT_ENTRANCE && !(r->encounter >= 0 && !r->done);
}

static int map_view;
static float corr_frac(void);
/* Buttons handled in both rooms and halls: inventory, torch, map, menu. */
static void common_buttons(void) {
    Dungeon *d = D();
    if (input_pressed(BTN_START)) { menu_sel = 0; st = DS_MENU; }
    if (input_pressed(BTN_SQUARE)) { inv_sel = 0; st = DS_INVENTORY; }
    if (input_pressed(BTN_SELECT)) { /* the full map, anywhere; from a hall it is look-only */
        map_view = d->corridor >= 0;
        if (map_view) {
            const Corridor *c = &d->corr[d->corridor];
            map_cursor = corr_frac() < 0.5f ? c->a : c->b; /* the nearer end */
        } else map_cursor = d->room;
        st = DS_MAP;
    }
    if (input_pressed(BTN_TRIANGLE)) {
        if (!d->items[ITEM_TORCH]) say("No torches left.");
        else if (d->torch >= 100) say("The torch already burns bright.");
        else {
            d->items[ITEM_TORCH]--;
            d->torch = d->torch + 25 > 100 ? 100 : d->torch + 25;
            say("A fresh torch is lit.");
        }
    }
}

/* Hall walking: Left/Right accelerate (velocity eases for a heavy, careful
 * feel), obstacles/seen traps block movement, then square-entry and ends. */
static void update_corridor(float dt) {
    Dungeon *d = D();
    Corridor *c = cur_corr();
    int dir = input_held(BTN_RIGHT) - input_held(BTN_LEFT);
    if (dir) {
        facing = dir;
        vel += dir * WALK_ACCEL * dt;
        if (vel > WALK_SPEED) vel = WALK_SPEED;
        if (vel < -WALK_SPEED) vel = -WALK_SPEED;
    } else {
        float slow = WALK_ACCEL * 1.5f * dt;
        vel = fabsf(vel) <= slow ? 0 : vel - (vel > 0 ? slow : -slow);
    }
    float nx = d->xpos + vel * dt;
    for (int s = 0; s < c->nseg; s++) { /* an uncleared obstacle blocks the way */
        int blocks = c->seg[s] == SPOT_OBSTACLE || (c->seg[s] == SPOT_TRAP && c->scouted); /* seen traps stop the party */
        if (!blocks || (c->seg_done[s] & 1)) continue;
        float cx = seg_center(s);
        if (d->xpos <= cx - 40 && nx > cx - 40) { nx = cx - 40; vel = 0; }
        if (d->xpos >= cx + 40 && nx < cx + 40) { nx = cx + 40; vel = 0; }
    }
    d->xpos = nx;
    if (d->xpos <= 0) { enter_room(c->a); return; }
    if (d->xpos >= corr_len(c)) { enter_room(c->b); return; }
    int s = seg_at(c, d->xpos);
    if (s >= 0 && s != last_seg) {
        last_seg = s;
        enter_square(s, vel >= 0 ? 1 : -1);
        if (st != DS_CORRIDOR) return;
    } else if (s < 0) {
        last_seg = -1;
    }
    if (input_pressed(BTN_CROSS)) {
        int ob = obstacle_here(), tr = trap_here();
        if (ob >= 0) clear_obstacle(ob);
        else if (tr >= 0) trap_disarm(tr);
        else open_curio();
    }
    common_buttons();
}

/* Room input: X uses a curio or opens the map to pick an exit. */
static void update_room(void) {
    int seg;
    if (input_pressed(BTN_CROSS)) {
        if (curio_here(&seg) >= 0) open_curio();
        else { map_cursor = D()->room; st = DS_MAP; }
        return;
    }
    if (input_pressed(BTN_RIGHT)) { map_cursor = D()->room; st = DS_MAP; return; }
    common_buttons();
}

/* Map cursor moves over rooms. An adjacent room is reached by walking its corridor; a visited
 * room further away is reached at once through explored rooms (quick travel). */
/* Map state: move the cursor between rooms; X travels there. */
static void update_map(void) {
    Dungeon *d = D();
    if (map_cursor < 0 || map_cursor >= d->nrooms) map_cursor = d->room;
    static const int ddx[4] = {-1, 1, 0, 0}, ddy[4] = {0, 0, -1, 1};
    static const uint32_t keys[4] = {BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN};
    for (int k = 0; k < 4; k++) {
        if (!input_pressed(keys[k])) continue;
        /* nearest room in that direction */
        int best = -1, bd = 999;
        for (int i = 0; i < d->nrooms; i++) {
            int dx = d->rooms[i].gx - d->rooms[map_cursor].gx, dy = d->rooms[i].gy - d->rooms[map_cursor].gy;
            if (dx * ddx[k] + dy * ddy[k] <= 0) continue;
            int dist = dx * dx + dy * dy;
            if (dist < bd) { bd = dist; best = i; }
        }
        if (best >= 0) map_cursor = best;
    }
    if (input_pressed(BTN_CIRCLE) || input_pressed(BTN_SELECT)) { st = map_view ? DS_CORRIDOR : DS_ROOM; map_view = 0; return; }
    if (map_view || !input_pressed(BTN_CROSS) || map_cursor == d->room) return;
    int exits[4], n = dungeon_room_exits(d, d->room, exits);
    for (int k = 0; k < n; k++) {
        Corridor *c = &d->corr[exits[k]];
        if (dungeon_other_end(c, d->room) != map_cursor) continue;
        int from_a = c->a == d->room;
        d->corridor = exits[k];
        if (c->explored && !(d->plot >= PLOT_DD && DUNGEON_DD_QUEST(d) < 4)) { /* corridor_return_content, not in the Darkest Dungeon */
            Rng rr;
            rng_seed(&rr, plat_ticks_ms() * 2246822519u + (uint32_t)exits[k]);
            dungeon_return_content(d, c, &rr);
        }
        c->explored = 1;
        d->xpos = from_a ? 10 : corr_len(c) - 10;
        travel_dir = facing = from_a ? 1 : -1;
        vel = 0;
        last_seg = -1;
        st = DS_CORRIDOR;
        return;
    }
    int hops = d->rooms[map_cursor].visited ? dungeon_reachable(d, d->room, map_cursor) : -1;
    if (hops > 0) {
        d->torch = d->torch - hops * 2 < 0 ? 0 : d->torch - hops * 2;
        enter_room(map_cursor);
    } else {
        say("Unexplored. Reach it through a neighbouring room.");
    }
}

/* The inventory tab of the right HUD panel (panel_inventory, 8 x 2 stacks): d-pad picks a stack,
 * X uses a provision, Square / O go back to the map tab. */
/* Backpack: move the selection over the 8x2 grid; X uses the stack - a torch
 * relights (+25), food heals 5%, other supplies (bandage, antivenom, herbs,
 * holy water, laudanum) apply their effect to the selected hero. Treasure and
 * trinkets can't be used here. */
static void update_inventory(void) {
    Dungeon *d = D();
    Loot slots[16];
    int n = dungeon_inventory_slots(d, slots, 16);
    if (input_pressed(BTN_LEFT) && inv_sel % 8 > 0) inv_sel--;
    if (input_pressed(BTN_RIGHT) && inv_sel % 8 < 7) inv_sel++;
    if (input_pressed(BTN_UP) && inv_sel >= 8) inv_sel -= 8;
    if (input_pressed(BTN_DOWN) && inv_sel < 8) inv_sel += 8;
    if (input_pressed(BTN_CIRCLE) || input_pressed(BTN_SQUARE)) st = d->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
    if (input_pressed(BTN_CROSS) && inv_sel < n && slots[inv_sel].kind != LK_ITEM)
        say(slots[inv_sel].kind == LK_TRINKET ? "Trinkets are equipped in the hamlet." : "Treasure for the hamlet.");
    if (input_pressed(BTN_CROSS) && inv_sel < n && slots[inv_sel].kind == LK_ITEM) {
        int it = slots[inv_sel].ref;
        Hero *p[4];
        int np = party_list(p);
        if (it == ITEM_TORCH) {
            d->items[it]--;
            d->torch = d->torch + 25 > 100 ? 100 : d->torch + 25;
            say("A fresh torch is lit.");
        } else if (it == ITEM_FOOD) {
            d->items[it]--;
            for (int i = 0; i < np; i++) {
                int heal = (int)(hero_max_hp(p[i]) * FOOD_HEAL + 0.5f);
                p[i]->hp = (int16_t)(p[i]->hp + heal > hero_max_hp(p[i]) ? hero_max_hp(p[i]) : p[i]->hp + heal);
            }
            say("The party shares a ration.");
        } else if (dd_item_effect[it] >= 0 && np) {
            /* supplies on the selected hero (effects "bandage" cure_bleed, "antivenom" cure_poison,
             * "medicinal_herbs" clear_debuff, "holy_water" resist buffs, "laudanum" clearDotStress) */
            Hero *h = p[hud_rank < np ? hud_rank : 0];
            int need = 0;
            for (int k = 0; k < HERO_DOTS; k++) {
                need |= it == ITEM_BANDAGE && h->bleed[k].rounds > 0;
                need |= it == ITEM_ANTIVENOM && h->blight[k].rounds > 0;
            }
            for (int k = 0; k < h->nebuffs; k++) need |= it == ITEM_HERBS && h->ebuffs[k].amount < 0;
            need |= it == ITEM_HOLY_WATER; /* laudanum only treats horror, which exists in battle */
            if (!need) say("That would do nothing for this hero.");
            else {
                Rng r;
                rng_seed(&r, plat_ticks_ms());
                d->items[it]--;
                hero_apply_effect_offline(d, p, np, hud_rank < np ? hud_rank : 0, dd_item_effect[it], &r);
                char b[64];
                snprintf(b, sizeof(b), "%s is treated.", h->name);
                say(b);
            }
        } else {
            say("Used on curios: walk up to one and press X.");
        }
    }
}

/* Start menu: camp, return to town (abandon), resume. */
static void update_menu(void) {
    Dungeon *d = D();
    int camp = can_camp();
    int n = camp ? 3 : 2;
    menu_sel = ui_menu_input(menu_sel, n);
    if (input_pressed(BTN_CIRCLE) || input_pressed(BTN_START)) st = d->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
    if (!input_pressed(BTN_CROSS)) return;
    if (menu_sel == 0) { st = d->corridor >= 0 ? DS_CORRIDOR : DS_ROOM; return; }
    if (camp && menu_sel == 1) {
        d->items[ITEM_FIREWOOD]--;
        menu_sel = 0;
        st = DS_MEAL;
        return;
    }
    int done = dungeon_goal_met(d);
    int qn = DUNGEON_DD_QUEST(d);
    if (!done && qn >= 0 && qn != PQ_TUTORIAL) { /* retreat_party_kill_count 1 */
        if (!dd_plot_quests[qn].can_retreat) { /* the final expedition (can_retreat false) */
            say("There is no turning back.");
            st = d->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
            return;
        }
        /* retreat_party_kill_count 1: one hero stays behind to cover the escape (a lone survivor
         * gets out alive, wiki) */
        Hero *p[4];
        int n = party_list(p), alive = 0;
        for (int k = 0; k < n; k++) alive += p[k]->alive;
        if (alive > 1) {
            Rng r;
            rng_seed(&r, plat_ticks_ms() ^ 0xdd);
            int pick = rng_range(&r, 0, alive - 1);
            for (int k = 0; k < n; k++)
                if (p[k]->alive && pick-- == 0) { p[k]->alive = 0; p[k]->hp = 0; }
        }
    }
    end_expedition(done);
}

/* rules.json meals_table: rations per hero, healing (fraction of max HP), stress */
static const struct { const char *name; float rations, heal; int stress; } meals[4] = {
    {"No meal", 0, -0.2f, 15}, {"Half meal", 0.5f, 0, 0}, {"Full meal", 1.0f, 0.1f, 0}, {"Feast", 2.0f, 0.25f, -10},
};

/* Food for a meal: rations per hero, scaled by food_consumption quirks and trinkets. */
/* How much food each hero needs for meal choice m. */
static int meal_need(int m) {
    Hero *p[4];
    int n = party_list(p);
    float need = 0;
    for (int k = 0; k < n; k++) {
        float f = 1 + quirk_stat(p[k], STAT_FOOD, 0) + trinket_stat(p[k], STAT_FOOD, 0);
        need += meals[m].rations * (f < 0 ? 0 : f);
    }
    return (int)ceilf(need - 0.001f);
}

/* Camp step 1 - the meal. Choose a meal size (more food = more healing, the
 * 'none' option costs stress). Food is spent, heroes heal/stress per the
 * meals table, then camp respite points are reset and we enter DS_CAMP. */
static void update_meal(void) {
    Dungeon *d = D();
    Hero *p[4];
    int n = party_list(p);
    menu_sel = ui_menu_input(menu_sel, 4);
    if (!input_pressed(BTN_CROSS)) return;
    int need = meal_need(menu_sel);
    if (need > d->items[ITEM_FOOD]) { say("Not enough food for that."); return; }
    d->items[ITEM_FOOD] = (uint8_t)(d->items[ITEM_FOOD] - need);
    for (int i = 0; i < n; i++) {
        int dh = (int)(hero_max_hp(p[i]) * meals[menu_sel].heal);
        int hp = p[i]->hp + dh;
        p[i]->hp = (int16_t)(hp < 1 ? 1 : hp > hero_max_hp(p[i]) ? hero_max_hp(p[i]) : hp);
        if (meals[menu_sel].stress) stress_add(p[i], meals[menu_sel].stress, d->level, plat_ticks_ms() + (uint32_t)i);
    }
    ambush_cut = 0;
    camp_icons_free();
    camp_points = RESPITE;
    camp_hero = camp_skill = camp_target = camp_phase = 0;
    memset(camp_uses, 0, sizeof(camp_uses));
    st = DS_CAMP;
    audio_sfx("gen_map_campstart");
    audio_narrate("good_camp", 100);
}

/* The camping skills this hero can use (class + unlocked). */
static int hero_camp_skills(const Hero *h, int out[16]) {
    int n = 0;
    for (int i = 0; i < dd_camp_skill_count && n < 16; i++)
        if (dd_camp_skills[i].classes & (1 << h->cls)) out[n++] = i;
    return n;
}

/* Resolve a camping skill: heals, stress relief, buffs... by its effects. */
static void apply_camp_skill(Hero *p[], int n, int user, int skill, int target) {
    const DDCampSkill *cs = &dd_camp_skills[skill];
    Rng r;
    audio_sfx_like("camp_skill_", cs->id);
    rng_seed(&r, plat_ticks_ms() ^ (uint32_t)skill);
    for (int e = 0; e < cs->eff_count; e++) {
        const DDCampEffect *ce = &dd_camp_effects[cs->eff_first + e];
        if (!rng_chance(&r, ce->chance)) continue;
        for (int k = 0; k < n; k++) {
            int hit = ce->selection == SEL_SELF ? k == user : ce->selection == SEL_INDIVIDUAL ? k == target
                    : ce->selection == SEL_PARTY ? 1 : k != user;
            if (!hit) continue;
            Hero *h = p[k];
            switch (ce->type) {
            case CE_STRESS_HEAL: stress_add(h, -(int)ce->amount, D()->level, rng_u32(&r)); break;
            case CE_STRESS_DAMAGE: stress_add(h, (int)ce->amount, D()->level, rng_u32(&r)); break;
            case CE_HEAL_PERCENT: {
                int hp = h->hp + (int)(hero_max_hp(h) * ce->amount + 0.5f);
                h->hp = (int16_t)(hp > hero_max_hp(h) ? hero_max_hp(h) : hp);
                break;
            }
            case CE_DAMAGE_PERCENT: {
                int hp = h->hp - (int)(hero_max_hp(h) * ce->amount + 0.5f);
                h->hp = (int16_t)(hp < 1 ? 1 : hp);
                break;
            }
            case CE_BUFF:
                if (ce->buff >= 0 && h->nebuffs < HERO_BUFFS) {
                    const DDBuff *bf = &dd_buffs[ce->buff];
                    HeroBuff *eb = &h->ebuffs[h->nebuffs++];
                    /* the camping skill's own amount overrides the buff's (default.camping_skills.json) */
                    eb->stat = bf->stat; eb->sub = bf->sub; eb->mode = bf->mode;
                    eb->amount = ce->amount != 0 ? ce->amount : bf->amount;
                    eb->battles = bf->duration_type == DUR_COMBAT_END && bf->duration > 0 ? bf->duration : -1;
                }
                break;
            case CE_REDUCE_AMBUSH: ambush_cut += ce->amount; break;
            case CE_TORCH: D()->torch = D()->torch - (int)ce->amount < 0 ? 0 : D()->torch - (int)ce->amount; break;
            case CE_LOOT: D()->gold += 100 * (D()->level >= 3 ? 2 : 1); break;
            default: break;
            }
        }
    }
}

/* Camp: spend limited "respite" points on skills, then maybe an ambush. */
static void update_camp(void) {
    Hero *p[4];
    int n = party_list(p);
    if (input_pressed(BTN_START)) { /* break camp */
        Dungeon *d = D();
        camp_icons_free();
        d->torch = 100; /* rules.json camp_restore_torch */
        d->camped = 1;
        for (int r = 0; r < 4; r++) { /* resting clears death's door recovery debuffs */
            int hi = g_camp.party[r];
            if (hi < 0) continue;
            Hero *h = &g_camp.roster[hi];
            int n = 0;
            for (int k = 0; k < h->nebuffs; k++)
                if (h->ebuffs[k].battles != HB_UNTIL_CAMP) h->ebuffs[n++] = h->ebuffs[k];
            h->nebuffs = (uint8_t)n;
        }
        st = DS_ROOM;
        Rng r;
        rng_seed(&r, plat_ticks_ms() ^ 0x5eed);
        if (rng_chance(&r, AMBUSH_CHANCE - ambush_cut)) {
            /* night ambush: a hall pack strikes in total darkness (ambush_torch_reduction) */
            int e = dungeon_pick_encounter(&r, d->dungeon, d->level, ENC_HALL);
            if (e >= 0) {
                d->torch = 0;
                ambushed = 1;
                say("Ambush!");
                battle_scene_surprise(0, SURPRISE_HEROES); /* night ambushes ignore surprise modifiers */
                start_fight(e, loaded_room);
                return;
            }
        }
        say("The party breaks camp, rested.");
        return;
    }
    int skills[16], ns = hero_camp_skills(p[camp_hero], skills);
    if (camp_phase == 0) { /* pick hero */
        if (input_pressed(BTN_LEFT)) camp_hero = (camp_hero + n - 1) % n;
        if (input_pressed(BTN_RIGHT)) camp_hero = (camp_hero + 1) % n;
        if (input_pressed(BTN_CROSS) && ns) { camp_phase = 1; camp_skill = 0; }
        return;
    }
    if (camp_phase == 1) { /* pick skill */
        camp_skill = ui_menu_input(camp_skill, ns);
        if (input_pressed(BTN_CIRCLE)) camp_phase = 0;
        if (input_pressed(BTN_CROSS)) {
            const DDCampSkill *cs = &dd_camp_skills[skills[camp_skill]];
            if (cs->cost > camp_points) say("Not enough respite left.");
            else if (camp_uses[skills[camp_skill]] >= cs->use_limit) say("Already used tonight.");
            else if (cs->needs_target) { camp_phase = 2; camp_target = 0; }
            else {
                apply_camp_skill(p, n, camp_hero, skills[camp_skill], camp_hero);
                camp_points -= cs->cost;
                camp_uses[skills[camp_skill]]++;
                say(cs->name);
            }
        }
        return;
    }
    if (input_pressed(BTN_LEFT)) camp_target = (camp_target + n - 1) % n; /* pick target */
    if (input_pressed(BTN_RIGHT)) camp_target = (camp_target + 1) % n;
    if (input_pressed(BTN_CIRCLE)) camp_phase = 1;
    if (input_pressed(BTN_CROSS)) {
        const DDCampSkill *cs = &dd_camp_skills[skills[camp_skill]];
        apply_camp_skill(p, n, camp_hero, skills[camp_skill], camp_target);
        camp_points -= cs->cost;
        camp_uses[skills[camp_skill]]++;
        say(cs->name);
        camp_phase = 1;
    }
}

/* Free results art. */
static void results_free(void) {
    image_free(&res_bg);
    image_free(&res_frames);
    image_free(&res_seal);
}

/* Quest Complete!: return to the hamlet now or keep exploring (the menu offers the way home later) */
/* "Quest Complete": choose between returning to town and exploring on. */
static void update_questdone(void) {
    menu_sel = ui_menu_input(menu_sel, 2);
    if (input_pressed(BTN_CROSS)) {
        image_free(&res_seal);
        if (menu_sel == 0) end_expedition(1);
        else st = D()->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
    }
}

/* Draw that prompt. */
static void draw_questdone(void) {
    if (!res_seal.tiles) image_load(&res_seal, "assets/ui/quest_seal.ddi");
    gfx_rect(0, 0, SCREEN_W, HUD_Y, RGBA(0, 0, 0, 150));
    ui_panel(SCREEN_W / 2 - 110, 20, 220, 22);
    text_draw(FONT_TITLE, SCREEN_W / 2, 22, dd_ui_str[US_QUEST_COMPLETE], COL_GOLD, 1);
    if (res_seal.tiles) image_draw(&res_seal, SCREEN_W / 2 - 30, 50, 60, 76, RGBA(255, 255, 255, 255));
    static const char *const items[2] = {"Return to the Hamlet", "Continue exploring"};
    ui_panel(SCREEN_W / 2 - 90, 134, 180, 40);
    ui_menu(SCREEN_W / 2 - 86, 137, 172, items, 2, menu_sel);
}

/* raid_results: page 0 quest rewards + collected treasure + heirlooms, page 1 the heroes */
/* Results animation: sections fade in one after another, amounts count up (res_t seconds). */
/* Helpers for the results screen's staggered count-up animation: progress,
 * count interpolation, and fade-in colour by delay. */
static float res_k(float delay) {
    float k = (res_t - delay) / 0.4f;
    return k < 0 ? 0 : k > 1 ? 1 : k;
}
static int res_count(int v, float delay) {
    float k = (res_t - delay) / 0.8f;
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    return (int)(v * (1 - (1 - k) * (1 - k)) + 0.5f); /* ease out */
}
static uint32_t res_fade(uint32_t col, float delay) {
    return (col & 0x00FFFFFFu) | ((uint32_t)(((col >> 24) & 255) * res_k(delay)) << 24);
}

/* Two results pages: rewards, then heroes (XP, quirks, deaths). */
static void draw_results(void) {
    if (!res_bg.tiles) image_load(&res_bg, "assets/ui/results_bg.ddi");
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 255));
    float bw = 226, bx = SCREEN_W / 2 - bw / 2;
    if (res_bg.tiles) image_draw(&res_bg, bx, 0, bw, SCREEN_H, RGBA(255, 255, 255, 255));
    text_draw(FONT_TITLE, SCREEN_W / 2, 20, res.kind == 0 ? "Victory!" : res.kind == 2 ? "Defeat" : "Retreat", COL_GOLD, 1);
    text_draw(FONT_BODY, SCREEN_W / 2, 44, quest_title(D()), COL_TEXT, 1);
    float x = bx + 16, y = 66;
    if (res.page == 0) {
        text_draw(FONT_BODY, SCREEN_W / 2, y, dd_ui_str[US_QUEST_REWARDS], COL_GOLD, 1);
        y += 16;
        Image *g = hud_item_icon(LK_GOLD, 0, res.quest_gold);
        if (res.quest_gold && g) image_draw(g, SCREEN_W / 2 - 34, y, 20, 40, res_fade(RGBA(255, 255, 255, 255), 0));
        if (res.quest_heir) {
            Image *hi = hud_item_icon(LK_HEIRLOOM, res.heir_type, res.quest_heir);
            if (hi) image_draw(hi, SCREEN_W / 2 + 6, y, 20, 40, res_fade(RGBA(255, 255, 255, 255), 0));
        }
        if (res.quest_gold) text_printf(FONT_SMALL, SCREEN_W / 2 - 34, y, res_fade(COL_TEXT, 0), "%d", res_count(res.quest_gold, 0));
        if (res.quest_heir) text_printf(FONT_SMALL, SCREEN_W / 2 + 6, y, res_fade(COL_TEXT, 0), "%d", res_count(res.quest_heir, 0));
        if (!res.quest_gold && !res.quest_heir) text_draw(FONT_SMALL, SCREEN_W / 2, y + 14, "None", COL_DIM, 1);
        y += 52;
        text_draw(FONT_BODY, x, y, dd_ui_str[US_COLLECTED_TREASURE], res_fade(COL_GOLD, 0.4f), 0);
        text_printf(FONT_BODY, bx + bw - 16, y, res_fade(COL_GOLD, 0.4f), "%d", res_count(res.gold, 0.4f));
        Image *gi = hud_item_icon(LK_GOLD, 0, res.gold);
        if (gi && res.gold) image_draw(gi, x, y + 16, 20, 40, res_fade(RGBA(255, 255, 255, 255), 0.4f));
        if (res.ntrinkets) text_printf(FONT_SMALL, x + 26, y + 30, res_fade(COL_TEXT, 0.4f), "+%d trinket%s", res.ntrinkets, res.ntrinkets > 1 ? "s" : "");
        y += 62;
        text_draw(FONT_BODY, x, y, dd_ui_str[US_COLLECTED_HEIRLOOMS], res_fade(COL_GOLD, 0.8f), 0);
        for (int k = 0; k < HEIR_COUNT; k++) {
            Image *hi = hud_item_icon(LK_HEIRLOOM, k, 1);
            float hx = x + k * 48;
            if (hi) image_draw(hi, hx, y + 16, 16, 32, res_fade(RGBA(255, 255, 255, 255), 0.8f));
            text_printf(FONT_SMALL, hx + 19, y + 28, res_fade(COL_TEXT, 0.8f), "%d", res_count(res.heirlooms[k], 0.8f));
        }
        hud_hints(BG_CROSS " Next");
        text_draw(FONT_TITLE, SCREEN_W / 2, 250, "Next", RGBA(200, 40, 30, 255), 1);
        return;
    }
    if (!res_frames.tiles) image_load(&res_frames, "assets/ui/results_heroes.ddi");
    if (res_frames.tiles) image_draw(&res_frames, SCREEN_W / 2 - 100, y, 200, 176, RGBA(255, 255, 255, 255));
    for (int i = 0; i < res.nhero && i < 4; i++) {
        float hy = y + i * 44, dl = i * 0.25f;
        if (res_k(dl) <= 0) continue;
        Image *p = ui_portrait(res.hero[i].cls);
        if (p) image_draw(p, SCREEN_W / 2 - 96, hy + 12, 24, 24, res_fade(res.hero[i].alive ? RGBA(255, 255, 255, 255) : RGBA(90, 40, 40, 255), dl));
        text_draw(FONT_BODY, SCREEN_W / 2 - 96, hy, res.hero[i].name, res_fade(COL_GOLD, dl), 0);
        if (!res.hero[i].alive) text_draw(FONT_SMALL, SCREEN_W / 2 - 66, hy + 18, "Fallen", res_fade(RGBA(200, 40, 30, 255), dl), 0);
        else {
            if (res.hero[i].xp) text_printf(FONT_SMALL, SCREEN_W / 2 + 40, hy + 1, res_fade(COL_TEXT, dl), dd_ui_str[US_RESOLVE_XP], res_count(res.hero[i].xp, dl)); /* "+%d Resolve XP" */
            text_printf(FONT_SMALL, SCREEN_W / 2 + 76, hy + 18, res_fade(COL_TEXT, dl), "%d", res.hero[i].level);
            /* new quirks: one line each, gained green, negative red (no wrapping into the next row) */
            if (res.hero[i].qpos[0]) text_printf(FONT_SMALL, SCREEN_W / 2 - 66, hy + 13, res_fade(RGBA(120, 210, 110, 255), dl + 0.3f), "+%s", res.hero[i].qpos);
            if (res.hero[i].qneg[0]) text_printf(FONT_SMALL, SCREEN_W / 2 - 66, hy + (res.hero[i].qpos[0] ? 23 : 13), res_fade(RGBA(230, 90, 80, 255), dl + 0.3f), "-%s", res.hero[i].qneg);
            if (res.hero[i].level_up) /* new resolve level: its title (str_resolve_N) */
                text_draw(FONT_SMALL, SCREEN_W / 2 + 40, hy + 10, dd_ui_str[US_RESOLVE_TITLE + (res.hero[i].level < 7 ? res.hero[i].level : 6)], res_fade(COL_GOLD, dl + 0.5f), 0);
        }
    }
    hud_hints(BG_CROSS " Return to Town");
    text_draw(FONT_TITLE, SCREEN_W / 2, 250, dd_ui_str[US_RETURN_TO_TOWN], RGBA(200, 40, 30, 255), 1);
}

/* Per-frame update entry: global hotkeys, quest-goal detection, then the
 * state dispatch (`switch (st)`). */
void dungeon_update(float dt) {
    if (input_pressed(BTN_L)) hud_rank = (hud_rank + 3) % 4;
    if (input_pressed(BTN_R)) hud_rank = (hud_rank + 1) % 4;
    /* Up / Down: the selected hero swaps places with the one ahead / behind (out of battle) */
    if ((st == DS_ROOM || st == DS_CORRIDOR) && (input_pressed(BTN_UP) || input_pressed(BTN_DOWN))) {
        int o = hud_rank + (input_pressed(BTN_UP) ? -1 : 1);
        if (o >= 0 && o < 4 && party_hero(hud_rank) && g_camp.party[o] >= 0) {
            int t = g_camp.party[o];
            g_camp.party[o] = g_camp.party[hud_rank];
            g_camp.party[hud_rank] = t;
            hud_rank = o;
        }
    }
    anim_t += dt;
    if (msg_t > 0) msg_t -= dt;
    Dungeon *dq = D();
    if ((st == DS_ROOM || st == DS_CORRIDOR) && !dq->goal_shown && dungeon_goal_met(dq)) {
        dq->goal_shown = 1; /* offered once; later the menu's "return to town" ends the quest */
        menu_sel = 0;
        st = DS_QUESTDONE;
    }
    switch (st) {
    case DS_QUESTDONE: update_questdone(); break;
    case DS_RESULTS:
        res_t += dt;
        if (input_pressed(BTN_CROSS) || input_pressed(BTN_START)) {
            if (res_t < RES_ANIM) res_t = RES_ANIM; /* first press skips the count-up */
            else if (res.page == 0) { res.page = 1; res_t = 0; }
            else {
                results_free();
                scene_switch(SCENE_TOWN);
            }
        }
        break;
    case DS_INTRO:
        if (input_pressed(BTN_CROSS) || input_pressed(BTN_START)) {
            Dungeon *d = D();
            d->intro_done = 1;
            image_free(&intro_img);
            st = DS_ROOM;
            if (!d->rooms[d->room].visited) enter_room(d->room); /* a quest that starts in a fight */
        }
        break;
    case DS_DONE:
        if (msg_t <= 0 || input_pressed(BTN_CROSS)) scene_switch(SCENE_TOWN);
        break;
    case DS_CORRIDOR: update_corridor(dt); break;
    case DS_ROOM: update_room(); break;
    case DS_MAP: update_map(); break;
    case DS_INVENTORY: update_inventory(); break;
    case DS_MENU: update_menu(); break;
    case DS_LOOT: /* already in the pack; the window just shows what was found */
        if (input_pressed(BTN_LEFT) && menu_sel > 0) menu_sel--;
        if (input_pressed(BTN_RIGHT) && menu_sel < curio_nloot - 1) menu_sel++;
        if (input_pressed(BTN_CROSS) || input_pressed(BTN_CIRCLE)) st = D()->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
        break;
    case DS_MEAL: update_meal(); break;
    case DS_CAMP: update_camp(); break;
    case DS_CURIO: {
        int n = ncurio_items + 2;
        curio_sel = ui_menu_input(curio_sel, n);
        if (input_pressed(BTN_CIRCLE)) st = D()->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
        if (input_pressed(BTN_CROSS)) {
            if (curio_sel == 0) use_curio(-1);
            else if (curio_sel <= ncurio_items) use_curio(curio_items[curio_sel - 1]);
            else st = D()->corridor >= 0 ? DS_CORRIDOR : DS_ROOM;
        }
        break;
    }
    }
}

/* ---------------------------------------------------------------- drawing
 * Each state has a matching draw function; dungeon_draw() picks them. */

/* Draw the 4 heroes in a column, walking or idle, facing left or right. */
static void draw_party(float front_x, int walking, int flip) {
    for (int r = 0; r < 4; r++) {
        Hero *h = party_hero(r);
        if (!h) continue;
        SpriteAnim *s = sprite_get(dd_heroes[h->cls].sprite, walking ? "walk" : "idle");
        if (!s) s = sprite_get(dd_heroes[h->cls].sprite, "combat");
        float x = front_x - (flip ? -1 : 1) * r * 50;
        const uint8_t tint[4] = {255, 255, 255, 255};
        sprite_draw(s, anim_t + r * 0.27f, 1, x, FLOOR_Y, 1.0f, flip, tint);
        hud_bars(x, FLOOR_Y + 2, h->hp, hero_max_hp(h), h->stress, 0);
        if (r == hud_rank) hud_bracket(BRACKET_SELECTED, x, FLOOR_Y + 12, 200);
    }
}

/* Draw a curio's sprite at x. */
static void draw_curio_prop(int curio, float x) {
    char spr[48];
    snprintf(spr, sizeof(spr), "curio_%s", curio == CURIO_QUEST ? "beacon" : dd_curios[curio].sprite);
    const uint8_t tint[4] = {255, 255, 255, 255};
    if (!sprite_draw(sprite_get(spr, "idle"), anim_t, 1, x, FLOOR_Y, 1.0f, 0, tint)) {
        gfx_rect(x - 12, FLOOR_Y - 26, 24, 26, RGBA(90, 70, 40, 255));
        ui_frame(x - 12, FLOOR_Y - 26, 24, 26, COL_GOLD);
    }
}

/* Hall view: parallax backgrounds, wall panels per square, props (curios,
 * traps, obstacles), party, darkness vignette. */
static void draw_corridor(void) {
    Dungeon *d = D();
    Corridor *c = cur_corr();
    float front = facing > 0 ? 250 : 230;      /* where the lead hero stands on screen */
    float cam = d->xpos - front;
    if (bg_far.tiles) image_draw(&bg_far, 0, VIEW_Y, SCREEN_W, SCREEN_H, RGBA(255, 255, 255, 255));
    /* middle layer: parallax at half speed */
    if (bg_mid.tiles)
        for (float x = -fmodf(cam * 0.5f, TILE) - TILE; x < SCREEN_W; x += TILE)
            image_draw(&bg_mid, x, VIEW_Y, TILE, SCREEN_H, RGBA(255, 255, 255, 255));
    /* door panels fill both ends edge to edge (DOOR wide each, repeated outward), wall panels
     * per square; every pixel of the corridor is covered so nothing shows through */
    float L = corr_len(c);
    if (bg_door.tiles)
        for (int k = 0; k < 4; k++) {
            float lx = -k * DOOR - cam, rx = L - DOOR + k * DOOR - cam;
            if (lx + DOOR > 0 && lx < SCREEN_W) image_draw(&bg_door, lx, VIEW_Y, DOOR, SCREEN_H, RGBA(255, 255, 255, 255));
            if (rx + DOOR > 0 && rx < SCREEN_W) image_draw(&bg_door, rx, VIEW_Y, DOOR, SCREEN_H, RGBA(255, 255, 255, 255));
        }
    for (int s = 0; s < c->nseg; s++) {
        float sx = DOOR + s * TILE - cam;
        if (sx > SCREEN_W || sx + TILE < 0) continue;
        Image *w = nwalls ? &walls[c->wall[s] % nwalls] : NULL;
        if (w && !w->tiles) w = &walls[0]; /* missing variant: reuse the first */
        if (w && w->tiles) image_draw(w, sx, VIEW_Y, TILE, SCREEN_H, RGBA(255, 255, 255, 255));
        if (c->seg[s] == SPOT_CURIO && !(c->seg_done[s] & 1)) draw_curio_prop(c->seg_enc[s], seg_center(s) - cam);
        if (c->seg[s] == SPOT_TRAP && trap_def() && (c->scouted || (c->seg_done[s] & 4))) {
            int armed = !(c->seg_done[s] & 1);
            char spr[40];
            snprintf(spr, sizeof(spr), "trap_%s", trap_def()->id);
            const uint8_t tint[4] = {255, 255, 255, 255};
            float tx = seg_center(s) - cam;
            if (!sprite_draw(sprite_get(spr, armed ? "idle" : "sprung"), anim_t, 1, tx, FLOOR_Y, 1.0f, 0, tint) && armed)
                gfx_rect(tx - 14, FLOOR_Y - 6, 28, 6, RGBA(150, 60, 200, 255));
            if (armed) { /* pulsing purple marker so it reads on the small screen */
                int a = 150 + (int)(100 * sinf(anim_t * 5));
                text_draw(FONT_BODY, tx, FLOOR_Y - 58, dd_ui_str[US_TRAP], RGBA(190, 90, 255, a), 1);
            }
        }
        if ((c->seg[s] == SPOT_OBSTACLE || c->seg[s] == SPOT_GHOST) && !(c->seg_done[s] & 1)) {
            float ox = seg_center(s) - cam; /* obstacle_definitions graphics (the Ancestor's shade in DD4) */
            const char *id = c->seg[s] == SPOT_GHOST ? "ancestor_ghost"
                           : D()->dungeon < DG_COUNT && dd_obstacles[D()->dungeon].id ? dd_obstacles[D()->dungeon].id : "rubble";
            char spr[48];
            snprintf(spr, sizeof(spr), "obstacle_%s", id);
            const uint8_t tint[4] = {255, 255, 255, 255};
            if (!sprite_draw(sprite_get(spr, "idle"), anim_t, 1, ox, FLOOR_Y, 1.0f, 0, tint)) {
                gfx_rect(ox - 30, FLOOR_Y - 72, 60, 72, RGBA(45, 35, 28, 255));
                ui_frame(ox - 30, FLOOR_Y - 72, 60, 72, RGBA(90, 70, 50, 255));
            }
        }
    }
    draw_party(front, fabsf(vel) > 5, facing < 0);
}

/* Camp: the party sits around the fire, two on each side facing it. */
/* Camp scene art. */
static void draw_campsite(void) {
    static const float xs[4] = {110, 175, 305, 370};
    const uint8_t white[4] = {255, 255, 255, 255}, warm[4] = {255, 200, 160, 255};
    gfx_rect(0, 0, SCREEN_W, HUD_Y, RGBA(30, 10, 0, 120));
    if (!sprite_draw(sprite_get("campfire", "idle"), anim_t, 1, SCREEN_W / 2, FLOOR_Y, 1.0f, 0, white))
        gfx_rect(SCREEN_W / 2 - 10, FLOOR_Y - 14, 20, 14, RGBA(255, 140, 40, 255));
    Hero *p[4];
    int n = party_list(p);
    for (int k = 0; k < n; k++) {
        float x = xs[n <= 2 ? k + 1 : k];
        int right = x > SCREEN_W / 2;
        SpriteAnim *sa = sprite_get(dd_heroes[p[k]->cls].sprite, "camp");
        if (!sa) sa = sprite_get(dd_heroes[p[k]->cls].sprite, "idle");
        sprite_draw(sa, anim_t + k * 0.31f, 1, x, FLOOR_Y, 1.0f, right, warm);
        hud_bars(x, FLOOR_Y + 2, p[k]->hp, hero_max_hp(p[k]), p[k]->stress, 0);
        if (st == DS_CAMP && k == (camp_phase == 2 ? camp_target : camp_hero))
            hud_bracket(camp_phase == 2 ? BRACKET_TARGET_ALLY : BRACKET_SELECTED, x, FLOOR_Y + 12, 255);
    }
}

/* Room view. */
static void draw_room(void) {
    Dungeon *d = D();
    if (bg_room.tiles) {
        float sw = SCREEN_W * bg_room.h / (float)SCREEN_H;
        float sx = (bg_room.w - sw) / 2;
        image_draw_part(&bg_room, sx > 0 ? sx : 0, sw < bg_room.w ? sw : bg_room.w, 0, VIEW_Y, SCREEN_W, SCREEN_H, RGBA(255, 255, 255, 255));
    }
    Room *rm = &d->rooms[d->room];
    if (rm->curio != -1 && !rm->curio_done && (rm->encounter < 0 || rm->done)) draw_curio_prop(rm->curio, 350);
    if (st == DS_CAMP || st == DS_MEAL) draw_campsite();
    else draw_party(250, 0, 0);
}

/* map_view: DS_MAP opened from a hall with SELECT, look only */
/* Party progress along the hall as 0..1 (for the minimap). */
static float corr_frac(void) {
    Corridor *c = cur_corr();
    return c ? D()->xpos / corr_len(c) : 0;
}

/* Full-screen map. */
static void draw_map(void) {
    Dungeon *d = D();
    float x = 16, y = 16, w = SCREEN_W - 32, h = HUD_Y - 30;
    gfx_rect(x - 4, y - 4, w + 8, h + 8, RGBA(6, 5, 4, 245));
    ui_frame(x - 4, y - 4, w + 8, h + 8, COL_EDGE);
    const Room *c = &d->rooms[map_cursor >= 0 ? map_cursor : d->room];
    hud_map(d, x, y, w, h, c->gx, c->gy, map_cursor, corr_frac());
    if (map_view) hud_hints(BG_DPAD " Look  " BG_CIRCLE BG_SELECT " Close");
    else hud_hints(BG_DPAD " Room  " BG_CROSS " Go  " BG_CIRCLE " Close");
}

/* Quest name for the HUD and the loading card. */
/* Loading-card title for the current quest. */
static const char *quest_title(const Dungeon *d) {
    int qn = DUNGEON_DD_QUEST(d);
    if (d->plot == PLOT_OLD_ROAD) return dd_ui_str[US_OLD_ROAD];
    if (qn >= DD_PLOT_QUESTS) return dd_plot_quests[qn].name;
    return dd_dungeon_names[d->dungeon];
}

/* loading_screen.<key>.png (export_ui.py -> assets/ui/loading/<key>.ddi) and its tip */
/* Picks the localized text key and tip for the loading card. */
static const char *loading_key(const Dungeon *d, const char **tip) {
    static char k[40];
    int qn = DUNGEON_DD_QUEST(d);
    static const char *const pq_keys[] = {"plot_town_invasion_0", "plot_crow_trinket", "plot_tutorial_crypts",
                                          "plot_trinket_retention_0", "plot_trinket_retention_1", "plot_trinket_retention_2"};
    *tip = d->dungeon <= DG_DARKEST ? dd_ui_str[US_TIP_RUINS + d->dungeon] : "";
    if (d->plot == PLOT_OLD_ROAD) {
        *tip = dd_ui_str[US_OLD_ROAD_TIP];
        return "old_road";
    }
    if (qn >= 0 && dd_plot_quests[qn].tip[0]) *tip = dd_plot_quests[qn].tip;
    if (qn >= 0 && qn < DD_PLOT_QUESTS) snprintf(k, sizeof(k), "plot_darkest_dungeon_%d", qn + 1);
    else if (qn >= DD_PLOT_QUESTS) return pq_keys[qn - DD_PLOT_QUESTS];
    else snprintf(k, sizeof(k), "%s_0", dd_dungeon_dirs[d->dungeon]);
    return k;
}


/* The loading card. */
static void draw_intro(void) {
    const char *tip, *key = loading_key(D(), &tip);
    if (!intro_img.tiles) {
        char p[80];
        snprintf(p, sizeof(p), "assets/ui/loading/%s.ddi", key);
        image_load(&intro_img, p);
    }
    gfx_rect(0, 0, SCREEN_W, SCREEN_H, RGBA(0, 0, 0, 255));
    if (intro_img.tiles) image_draw(&intro_img, 0, 0, SCREEN_W, SCREEN_H, RGBA(255, 255, 255, 255));
    /* loading_screen.layout: title frame at the top, tip under the art, pulsing "continue" */
    ui_panel(SCREEN_W / 2 - 110, 22, 220, 26);
    text_draw(FONT_TITLE, SCREEN_W / 2, 26, quest_title(D()), COL_GOLD, 1);
    int w = text_width(FONT_BODY, tip);
    int tw = w < 300 ? w : 300;
    text_draw_wrapped(FONT_BODY, SCREEN_W / 2 - tw / 2, 206, 300, tip, COL_TEXT);
    if (fmodf(anim_t, 2.0f) < 1.4f) text_draw(FONT_SMALL, SCREEN_W / 2, 258, "Press " BG_CROSS " to continue", COL_DIM, 1);
}

/* Bottom HUD plus torch meter. */
static void draw_hud(void) {
    Dungeon *d = D();
    hud_torch(d->torch);
    text_printf(FONT_SMALL, 4, 2, COL_TEXT, "%s", quest_title(d));
    int qn = DUNGEON_DD_QUEST(d);
    if (d->plot == PLOT_OLD_ROAD) text_draw(FONT_SMALL, 4, 12, dd_ui_str[US_OLD_ROAD_GOAL], COL_DIM, 0);
    else if (qn >= 0 && dd_plot_quests[qn].goals)
        text_printf(FONT_SMALL, 4, 12, COL_DIM, qn == 1 ? "Iron crowns lit %d/%d" : "Locus beacon %d/%d", d->goals_done, dd_plot_quests[qn].goals);
    else if (d->quest == QUEST_BOSS) text_draw(FONT_SMALL, 4, 12, dungeon_goal_met(d) ? "Boss slain" : "Slay the boss", COL_DIM, 0);
    else if (d->quest == QUEST_EXPLORE) text_printf(FONT_SMALL, 4, 12, COL_DIM, "Explore %d%%/90%%", dungeon_explored_percent(d));
    else text_printf(FONT_SMALL, 4, 12, COL_DIM, "Battles left %d", dungeon_battles_left(d));
    char buf[40];
    snprintf(buf, sizeof(buf), "%d gold", d->gold);
    text_draw(FONT_SMALL, SCREEN_W - 4, 2, buf, COL_GOLD, 2);
    snprintf(buf, sizeof(buf), "Food %d  Torches %d", d->items[ITEM_FOOD], d->items[ITEM_TORCH]);
    text_draw(FONT_SMALL, SCREEN_W - 4, 12, buf, COL_TEXT, 2);
    Hero *h = party_hero(hud_rank);
    if (st == DS_CAMP) {
        Hero *p[4];
        if (camp_hero < party_list(p)) h = p[camp_hero];
    }
    for (int k = 0; k < 4 && !h; k++) h = party_hero(hud_rank = (hud_rank + 1) % 4);
    HudHero v;
    if (h) hud_hero(&v, h, NULL);
    else memset(&v, 0, sizeof(v));
    battle_corr_frac = corr_frac();
    hud_show_inventory(st == DS_INVENTORY ? inv_sel : -1); /* Square: the panel's inventory tab */
    hud_draw(&v, d, battle_corr_frac);
}

/* the stacks themselves are drawn in the HUD panel; here only the selected stack's name */
/* Backpack overlay. */
static void draw_inventory(void) {
    Dungeon *d = D();
    Loot slots[16];
    int n = dungeon_inventory_slots(d, slots, 16);
    char name[64];
    if (inv_sel < n) {
        loot_label(&slots[inv_sel], name, sizeof(name));
        float w = text_width(FONT_BODY, name) + 12;
        ui_panel(SCREEN_W - w - 6, HUD_Y - 34, w, 18);
        text_draw(FONT_BODY, SCREEN_W - w, HUD_Y - 31, name, COL_TEXT, 0);
    }
    hud_hints(BG_DPAD " Item  " BG_CROSS " Use  " BG_SQUARE BG_CIRCLE " Map");
}

/* Curio choice list. */
static void draw_curio_menu(void) {
    int seg, cu = curio_here(&seg);
    if (cu < 0) return;
    char items[ITEM_COUNT + 2][48];
    const char *ptrs[ITEM_COUNT + 2];
    snprintf(items[0], 48, "Investigate");
    for (int i = 0; i < ncurio_items; i++) snprintf(items[i + 1], 48, "Use %s", item_names[curio_items[i]]);
    snprintf(items[ncurio_items + 1], 48, "Leave it");
    for (int i = 0; i < ncurio_items + 2; i++) ptrs[i] = items[i];
    ui_panel(140, 50, 200, 40 + (ncurio_items + 2) * 14);
    text_draw(FONT_TITLE, 150, 54, dd_curios[cu].name, COL_GOLD, 0);
    ui_menu(150, 76, 180, ptrs, ncurio_items + 2, curio_sel);
    ui_hints(BG_CROSS " Choose  " BG_CIRCLE " Back");
}

/* Camp skill menu. */
static void draw_camp(void) {
    Hero *p[4];
    int n = party_list(p);
    if (st == DS_MEAL) {
        char items[4][48];
        const char *ptrs[4];
        for (int i = 0; i < 4; i++) {
            snprintf(items[i], 48, "%s  (%d food)", meals[i].name, meal_need(i));
            ptrs[i] = items[i];
        }
        ui_panel(SCREEN_W / 2 - 100, 48, 200, 88);
        text_draw(FONT_TITLE, SCREEN_W / 2, 51, "The Meal", COL_GOLD, 1);
        ui_menu(SCREEN_W / 2 - 92, 74, 184, ptrs, 4, menu_sel);
        text_printf(FONT_SMALL, SCREEN_W / 2 - 92, 124, COL_DIM, "Food carried: %d", D()->items[ITEM_FOOD]);
        hud_hints(BG_CROSS " Eat");
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "Respite %d", camp_points);
    text_draw(FONT_TITLE, SCREEN_W / 2, 44, buf, COL_GOLD, 1);
    if (camp_phase >= 1) {
        int skills[16], ns = hero_camp_skills(p[camp_hero], skills);
        float w = 170, y = HUD_Y - 14 - ns * 15;
        float x = camp_hero < n / 2 || n == 1 ? SCREEN_W - w - 6 : 6; /* opposite side to the hero */
        gfx_rect(x, y - 3, w, ns * 15 + 4, RGBA(0, 0, 0, 210));
        ui_frame(x, y - 3, w, ns * 15 + 4, COL_EDGE);
        for (int i = 0; i < ns; i++) {
            const DDCampSkill *cs = &dd_camp_skills[skills[i]];
            int used = camp_uses[skills[i]] >= cs->use_limit, afford = cs->cost <= camp_points;
            float ry = y + i * 15;
            if (i == camp_skill) gfx_rect(x + 1, ry - 1, w - 2, 15, RGBA(90, 66, 30, 210));
            Image *ic = camp_icon(skills[i]);
            if (ic) image_draw(ic, x + 3, ry, 13, 13, used || !afford ? RGBA(90, 90, 90, 255) : RGBA(255, 255, 255, 255));
            text_draw(FONT_SMALL, x + 20, ry + 2, cs->name, used || !afford ? COL_DIM : i == camp_skill ? COL_SELECT : COL_TEXT, 0);
            snprintf(buf, sizeof(buf), used ? "used" : "%d", cs->cost);
            text_draw(FONT_SMALL, x + w - 5, ry + 2, buf, COL_GOLD, 2);
        }
    }
    hud_hints(camp_phase == 0 ? BG_LEFTRIGHT " Hero  " BG_CROSS " Skills  " BG_START " Rest" : camp_phase == 1 ? BG_CROSS " Use  " BG_CIRCLE " Back" : BG_LEFTRIGHT " Target  " BG_CROSS " Use  " BG_CIRCLE " Back");
}

/* Draw by state. */
void dungeon_draw(void) {
    Dungeon *d = D();
    grade_scene(dungeon_grade_dir(), d->torch);
    if (st == DS_INTRO) {
        draw_intro();
        return;
    }
    if (st == DS_RESULTS) {
        draw_results();
        return;
    }
    if (d->corridor >= 0) draw_corridor();
    else draw_room();
    int dark = (100 - d->torch) * 150 / 100;
    if (dark > 0 && !grade_active()) gfx_rect(0, 0, SCREEN_W, HUD_Y, RGBA(0, 0, 0, dark)); /* the LUT darkens instead */
    ui_vignette(0, 0, SCREEN_W, HUD_Y, d->torch);
    draw_hud();

    int seg;
    if (st == DS_CORRIDOR && obstacle_here() >= 0) {
        ui_panel(SCREEN_W / 2 - 90, 68, 180, 18);
        text_draw(FONT_BODY, SCREEN_W / 2, 71, d->items[ITEM_SHOVEL] ? BG_CROSS " Dig through (shovel)" : BG_CROSS " Clear by hand", COL_GOLD, 1);
    } else if (st == DS_CORRIDOR && trap_here() >= 0) {
        Hero *p[4];
        int n = party_list(p), who = hud_rank < n ? hud_rank : 0;
        char b[96], pct[40];
        float ch = n ? trap_chance(p[who], 1) : 0;
        snprintf(pct, sizeof(pct), dd_ui_str[US_TRAP_DISARM], (ch < 0 ? 0 : ch > 1 ? 1 : ch) * 100);
        snprintf(b, sizeof(b), BG_CROSS " Disarm: %s (%s)", n ? p[who]->name : "", pct);
        ui_panel(SCREEN_W / 2 - 110, 68, 220, 18);
        text_draw(FONT_BODY, SCREEN_W / 2, 71, b, RGBA(190, 90, 255, 255), 1);
    } else if ((st == DS_CORRIDOR || st == DS_ROOM) && curio_here(&seg) >= 0) {
        ui_panel(SCREEN_W / 2 - 80, 68, 160, 18);
        text_draw(FONT_BODY, SCREEN_W / 2, 71, BG_CROSS " Investigate curio", COL_GOLD, 1);
    }
    switch (st) {
    case DS_QUESTDONE: draw_questdone(); break;
    case DS_MAP: draw_map(); break;
    case DS_LOOT:
        hud_loot("Loot", curio_loot, curio_nloot, menu_sel < curio_nloot ? menu_sel : 0);
        hud_hints(BG_LEFTRIGHT " Inspect  " BG_CROSS " Take all");
        break;
    case DS_INVENTORY: draw_inventory(); break;
    case DS_CURIO: draw_curio_menu(); break;
    case DS_MEAL:
    case DS_CAMP: draw_camp(); break;
    case DS_MENU: {
        const char *items[3];
        int n = 0;
        items[n++] = "Continue";
        if (can_camp()) items[n++] = "Make camp (firewood)";
        items[n++] = dungeon_goal_met(d) ? "Return to town (complete)" : "Abandon expedition";
        ui_panel(SCREEN_W / 2 - 100, 96, 200, 16 + n * 14);
        ui_menu(SCREEN_W / 2 - 96, 102, 192, items, n, menu_sel);
        break;
    }
    case DS_CORRIDOR: hud_hints(BG_LEFTRIGHT " Walk  " BG_SELECT " Map  " BG_L BG_R " Hero  " BG_UPDOWN " Swap  " BG_SQUARE " Items  " BG_TRIANGLE " Torch  " BG_START " Menu"); break;
    case DS_ROOM: hud_hints(BG_CROSS BG_RIGHT " Map  " BG_L BG_R " Hero  " BG_UPDOWN " Swap  " BG_SQUARE " Items  " BG_TRIANGLE " Torch  " BG_START " Menu"); break;
    default: break;
    }
    if (msg_t > 0 && msg[0]) {
        float w = text_width(FONT_BODY, msg) + 16;
        ui_panel(SCREEN_W / 2 - w / 2, 46, w, 18);
        text_draw(FONT_BODY, SCREEN_W / 2, 49, msg, COL_TEXT, 1);
    }
}
