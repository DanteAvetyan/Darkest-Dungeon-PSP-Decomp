/*
 * scene_battle.c - the battle SCREEN (presentation + player input).
 *
 * This file contains no combat rules. It owns one Battle (the engine in
 * battle.c) and runs a small state machine:
 *   BS_NEXT   ask the engine whose turn it is (battle_begin_turn)
 *   BS_PLAY   play back the events the engine recorded, one at a time, with
 *             popups, animations and delays (show_event)
 *   BS_CHOOSE / BS_TARGET  a hero's turn: pick a skill, then a target
 *   BS_PAUSE  the pause menu (retreat etc.)
 *   BS_END / BS_LOOT  results and loot window
 * Monster turns are decided by battle_ai_choose; heroes by the player.
 *
 * KEY IDEA: event playback. When battle_perform() runs, the engine instantly
 * computes everything and appends BattleEvents. We then walk that list over
 * time. A "Visual" per actor stores what to DRAW (pose, x position, flash,
 * displayed HP) - separate from the engine's real values - so HP bars can
 * count down in step with the animation. Visuals never change engine state.
 *
 * Battle screen: draws the fight and plays back the combat engine's events.
 * Controls: Left/Right pick a skill, X confirm, then Left/Right pick a target, X attack,
 * O back, Square pass, Start retreat. */
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "audio.h"
#include "battle.h"
#include "campaign.h"
#include "hud.h"
#include "loot.h"
#include "platform.h"
#include "scenes.h"
#include "stress.h"
#include "text.h"
#include "ui.h"

/* Layout constants: y of the floor line, pixel distance between ranks, and
 * the hero panel's slot numbers (0-3 skills, 4 move, 5 pass). */
#define GROUND_Y 160
#define RANK_SPACING 52
#define SLOT_MOVE 4
#define SLOT_PASS 5
#define NSLOTS 6
#define RETREAT_CHANCE 0.70f /* rules.json combat_retreat_chance */

/* The scene's state machine states (see file comment). */
typedef enum { BS_NEXT, BS_PLAY, BS_CHOOSE, BS_TARGET, BS_PAUSE, BS_END, BS_LOOT } State;

/* Per-actor drawing state, parallel to Battle.actors[]. */
typedef struct {
    const char *anim; /* current pose */
    float t;          /* pose time */
    float x;          /* drawn x (eases toward the rank position) */
    float lunge;      /* attack slide */
    float flash;      /* hit flash */
    int hp;           /* HP shown (catches up with events) */
    int visible;
} Visual;

/* Scene globals. B = the live fight. Everything else is UI state: current
 * actor, selected skill/target, timers, banner text, the loot list... */
static Battle B;
static Visual vis[MAX_ACTORS];
static Image bg;
static State state, after_play;
static int encounter_idx, result, actor;
static int sel, tgt_rank, tgt_side, tgt_mask, chosen_skill, pause_sel;
static float timer;
static int ev_i;
static char banner[48];
static float banner_t;
static char bg_path[96];
static Loot loot[LOOT_MAX];
static int nloot, loot_sel;
static int panel_actor = -1; /* hero shown in the HUD (the last one to act) */
/* action close-up (the original's attack camera, simplified): attacker and first target drawn
 * larger toward the centre over a dimmed scene while the skill plays */
static int zoom_a = -1, zoom_b = -1;
static float zoom_time;
#define ZOOM_LEN 1.1f

/* ---------------------------------------------------------------- layout */

/* Screen x of an actor's rank: heroes stand left of centre, monsters right,
 * rank 1 nearest the middle. Big monsters are centred over their ranks. */
static float rank_x(const Actor *a) {
    if (a->is_hero && a->captured_by >= 0) return -100; /* held by a captor: off the line */
    float off = (a->rank - 1) * RANK_SPACING + (a->size - 1) * RANK_SPACING * 0.5f;
    return a->side == SIDE_HEROES ? 196 - off : 284 + off;
}

/* Sprite folder name for an actor (hero class sprite or monster look). */
static const char *sprite_of(const Actor *a) {
    return a->is_hero ? dd_heroes[a->cls].sprite : dd_monsters[a->look].sprite;
}

/* Switch an actor's animation and restart its clock. */
static void pose(int i, const char *anim) {
    vis[i].anim = anim;
    vis[i].t = 0;
}

/* Initialise the Visual of actor i from the engine state. */
static void reset_visual(int i) {
    Actor *a = &B.actors[i];
    memset(&vis[i], 0, sizeof(Visual));
    vis[i].x = rank_x(a);
    vis[i].hp = a->hp;
    vis[i].visible = a->used && a->alive;
    pose(i, a->is_corpse ? "dead" : "combat");
}

/* ---------------------------------------------------------------- start / end */

static int surprise_scouted, surprise_forced;

/* The dungeon scene calls this BEFORE starting a fight to describe how it
 * began (scouted room? ambush?). Consumed once by battle_enter. */
void battle_scene_surprise(int scouted, int forced) {
    surprise_scouted = scouted;
    surprise_forced = forced;
}

/* Request a fight: encounter index into dd_encounters and a background image.
 * The dungeon then switches to SCENE_BATTLE. */
void battle_scene_start(int encounter, const char *background) {
    encounter_idx = encounter;
    snprintf(bg_path, sizeof(bg_path), "%s", background ? background : "");
    scene_switch(SCENE_BATTLE);
}

int battle_scene_result(void) { return result; }

float battle_corr_frac; /* set by the dungeon screen for the HUD map */

/* Scene enter: build the Battle from the party and encounter, roll surprise,
 * load the background, init visuals. */
/* Decode this fight's sounds ahead of time (skills, voices, impacts) so nothing is late when it matters. */
static void preload_sounds(void) {
    audio_preload_begin();
    static const char *const common[] = {"char_share_imp_sword", "char_share_imp_knife", "char_share_imp_hammer", "char_share_imp_axe",
        "char_share_imp_gun", "char_share_imp_arrow", "char_share_imp_shield", "char_share_imp_whip", "char_share_imp_liquid",
        "char_share_imp_spear", "char_share_imp_magic_dark", "char_share_imp_staff", "char_share_imp_general", "camp_skill_bandage",
        "sfx_org_imp_01_heavy_sweetener", "sfx_org_imp_02_heavy_sweetener", "sfx_org_imp_03_heavy_sweetener", "sfx_org_imp_04_heavy_sweetener",
        "sfx_org_imp_06_heavy_sweetener", "sfx_org_imp_01_light_sweetener", "sfx_org_imp_02_light_sweetener", "sfx_org_imp_03_light_sweetener",
        "sfx_org_imp_04_light_sweetener", "sfx_org_imp_05_light_sweetener", "gen_combat_deathsdoor", "gen_raid_bodycount",
        "gen_raid_success", "gen_raid_fail", "gen_char_resolve_virtue", "gen_char_resolve_test", "sfx_bleed_01_light_sweetener",
        "char_share_imp_gas"};
    for (unsigned i = 0; i < sizeof common / sizeof *common; i++) audio_sfx(common[i]);
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &B.actors[i];
        if (!a->used) continue;
        if (a->is_hero) {
            const DDHeroClass *hc = &dd_heroes[a->cls];
            for (int k = 0; k < hc->skill_count; k++) {
                audio_hero_skill(hc->id, dd_skills[hc->skill_first + k * 5].id, 0);
                audio_hero_skill(hc->id, dd_skills[hc->skill_first + k * 5].id, 1);
            }
        } else {
            const DDMonsterClass *mc = &dd_monsters[a->cls];
            for (int k = 0; k < mc->skill_count; k++) {
                audio_monster_skill(mc->id, dd_skills[mc->skill_first + k].id, 0);
                audio_monster_skill(mc->id, dd_skills[mc->skill_first + k].id, 1);
            }
            audio_monster(mc->id, "hurt"); audio_monster(mc->id, "death"); audio_monster(mc->id, "aggro");
        }
    }
    audio_preload_end();
}

void battle_enter(void) {
    audio_scene_music(2, g_camp.dungeon.dungeon, 1);
    hud_show_inventory(-1);
    battle_init(&B, g_camp.seed ^ (uint32_t)plat_ticks_ms(), g_camp.dungeon.torch);
    B.level = g_camp.dungeon.level;
    /* stall penalty: off for the Old Road and the first Ruins quest (wiki Stalling) */
    B.dungeon = g_camp.dungeon.plot == PLOT_OLD_ROAD || DUNGEON_DD_QUEST(&g_camp.dungeon) == PQ_TUTORIAL ? -1 : g_camp.dungeon.dungeon;
    for (int r = 0; r < 4; r++) {
        int h = g_camp.party[r];
        if (h >= 0 && g_camp.roster[h].alive) battle_add_hero(&B, &g_camp.roster[h], r + 1);
    }
    /* close gaps if someone in the party died earlier */
    int next = 1;
    for (int r = 1; r <= 4; r++)
        for (int i = 0; i < MAX_ACTORS; i++)
            if (B.actors[i].used && B.actors[i].is_hero && B.actors[i].rank == r) B.actors[i].rank = (uint8_t)next++;
    if (encounter_idx >= 0) {
        const DDEncounter *e = &dd_encounters[encounter_idx];
        int rank = 1;
        for (int i = 0; i < e->count && rank <= 4; i++) {
            battle_add_monster(&B, e->monster[i], rank);
            rank += dd_monsters[e->monster[i]].size ? dd_monsters[e->monster[i]].size : 1;
        }
    }
    int surprised = battle_roll_surprise(&B, surprise_scouted, surprise_forced);
    surprise_scouted = surprise_forced = 0;
    for (int i = 0; i < MAX_ACTORS; i++) reset_visual(i);
    image_free(&bg);
    if (bg_path[0]) image_load(&bg, bg_path);
    ui_popups_clear();
    result = 0;
    preload_sounds();
    state = BS_NEXT;
    timer = 0;
    banner[0] = 0;
    if (surprised) {
        snprintf(banner, sizeof(banner), surprised == SURPRISE_HEROES ? "Surprised!" : "The enemy is surprised!");
        banner_t = 2.0f;
    }
    panel_actor = -1;
}

/* Scene leave: free the background image. */
void battle_leave(void) {
    image_free(&bg);
    sprite_cache_release_all();
}

/* The fight ended with result r (1 won / 2 lost / 3 retreated): copy results
 * back to roster heroes, roll loot, XP and quirks, set battle_scene_result. */
static void finish(int r) {
    result = r;
    audio_sfx(r == 1 ? "gen_raid_success" : r == 2 ? "gen_raid_fail" : "gen_raid_town_click");
    if (r == 1) audio_narrate_once("good_victoryfirst", 85);
    else if (r == 3) audio_narrate("good_retreat_success", 85);
    else audio_narrate_important("bad_questfail", 100);
    for (int i = 0; i < MAX_ACTORS; i++) {
        Actor *a = &B.actors[i];
        if (!a->used || !a->is_hero) continue;
        a->hero->hp = (int16_t)(a->alive ? (a->hp > 0 ? a->hp : 1) : 0);
        int nb = 0, np = 0; /* DoTs still running go back out into the halls */
        memset(a->hero->bleed, 0, sizeof(a->hero->bleed));
        memset(a->hero->blight, 0, sizeof(a->hero->blight));
        for (int k = 0; a->alive && k < MAX_DOTS; k++) {
            if (a->bleed[k].rounds > 0 && nb < HERO_DOTS) a->hero->bleed[nb++] = a->bleed[k];
            if (a->blight[k].rounds > 0 && np < HERO_DOTS) a->hero->blight[np++] = a->blight[k];
        }
    }
    if (r == 1) {
        nloot = loot_battle(&B.rng, &B, g_camp.dungeon.level, g_camp.dungeon.dungeon, B.torch, loot);
        loot_sel = 0;
        snprintf(banner, sizeof(banner), "Victory!");
    } else {
        snprintf(banner, sizeof(banner), r == 3 ? "Retreated" : r == 4 ? "Teleported" : "Defeat");
    }
    g_camp.dungeon.torch = B.torch;
    banner_t = 2.0f;
    state = BS_END;
    timer = 2.0f;
}

/* ---------------------------------------------------------------- event playback */

/* Popup colour per event type (red damage, green heal, etc.). */
static uint32_t ev_color(int type) {
    switch (type) {
    case EV_HEAL: return COL_GOOD;
    case EV_CRIT: return RGBA(255, 220, 60, 255);
    case EV_MISS: case EV_RESIST: return COL_DIM;
    case EV_BLIGHT: return RGBA(150, 200, 60, 255);
    case EV_STUN: case EV_STUN_SKIP: return RGBA(240, 230, 120, 255);
    default: return COL_RED;
    }
}

/* Spawn a floating text above actor i. */
static void popup_on(int i, uint32_t col, const char *text) {
    if (i < 0) return;
    ui_popup(vis[i].x, GROUND_Y - 108, col, "%s", text); /* below the torch meter */
}

/* One Buff / Debuff / Resist popup per actor per action, however many effects land. */
static uint8_t shown[MAX_ACTORS];
static int once(int actor, int kind) {
    if (actor < 0) return 1;
    if (shown[actor] & (1 << kind)) return 1;
    shown[actor] |= (uint8_t)(1 << kind);
    return 0;
}

/* Shows one event; returns how long to wait before the next one. */
/* Present ONE recorded event: switch poses, spawn popups, bump the Visual's
 * displayed HP, set the banner. The returned number is how long to wait
 * before showing the next event (so a multi-hit attack reads clearly). */
/* Weapon impact sample from the skill's animation name. */
static const char *impact_for(const char *anim) {
    static const struct { const char *k, *s; } t[] = {
        {"sword", "char_share_imp_sword"}, {"dagger", "char_share_imp_knife"}, {"knife", "char_share_imp_knife"},
        {"mace", "char_share_imp_hammer"}, {"hammer", "char_share_imp_hammer"}, {"axe", "char_share_imp_axe"},
        {"pistol", "char_share_imp_gun"}, {"gun", "char_share_imp_gun"}, {"ranged", "char_share_imp_arrow"},
        {"shield", "char_share_imp_shield"}, {"whip", "char_share_imp_whip"}, {"throw", "char_share_imp_liquid"},
        {"spear", "char_share_imp_spear"}, {"point", "char_share_imp_spear"}, {"magic", "char_share_imp_magic_dark"},
        {"staff", "char_share_imp_staff"},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) if (strstr(anim, t[i].k)) return t[i].s;
    return "char_share_imp_general";
}
static int snd_actor = -1, snd_skill = -1; /* current performer and skill, for hit/miss sounds */

static void skill_sound(int a, int skill, int miss) {
    if (a < 0 || skill < 0) return;
    if (B.actors[a].is_hero) audio_hero_skill(dd_heroes[B.actors[a].cls].id, dd_skills[skill].id, miss);
    else audio_monster_skill(dd_monsters[B.actors[a].cls].id, dd_skills[skill].id, miss);
}

static float show_event(const BattleEvent *e) {
    char buf[32];
    int t = e->target, a = e->actor;
    switch (e->type) {
    case EV_TURN: return a >= 0 && B.actors[a].is_hero ? 0.1f : 0.5f;
    case EV_SKILL:
        memset(shown, 0, sizeof(shown));
        snprintf(banner, sizeof(banner), "%s", dd_skills[e->skill].name);
        banner_t = 1.2f;
        pose(a, dd_skills[e->skill].type == SK_MOVE ? "walk" : dd_skills[e->skill].anim);
        if (dd_skills[e->skill].type != SK_MOVE && a >= 0) {
            snd_actor = a; snd_skill = e->skill;
            skill_sound(a, e->skill, 0);
            if (!B.actors[a].is_hero) audio_monster(dd_monsters[B.actors[a].cls].id, "aggro");
        }
        vis[a].lunge = dd_skills[e->skill].type == SK_MOVE ? 0 : 1;
        if (dd_skills[e->skill].type != SK_MOVE) {
            zoom_a = a;
            zoom_b = -1; /* set by the first hit or miss */
            zoom_time = ZOOM_LEN;
        }
        return 0.6f;
    case EV_HIT:
    case EV_CRIT:
        if (zoom_time > 0 && zoom_b < 0 && a >= 0 && a != zoom_a) zoom_b = a; /* hit events carry the struck actor */
        audio_sfx(snd_skill >= 0 ? impact_for(dd_skills[snd_skill].anim) : "char_share_imp_general");
        {
            static const char *const hv[] = {"sfx_org_imp_01_heavy_sweetener", "sfx_org_imp_02_heavy_sweetener", "sfx_org_imp_03_heavy_sweetener", "sfx_org_imp_04_heavy_sweetener", "sfx_org_imp_06_heavy_sweetener"};
            static const char *const lt[] = {"sfx_org_imp_01_light_sweetener", "sfx_org_imp_02_light_sweetener", "sfx_org_imp_03_light_sweetener", "sfx_org_imp_04_light_sweetener", "sfx_org_imp_05_light_sweetener"};
            static unsigned sw;
            sw = sw * 1103515245u + 12345u;
            audio_sfx((e->type == EV_CRIT ? hv : lt)[(sw >> 16) % 5]);
        }
        if (a >= 0 && !B.actors[a].is_hero) audio_monster(dd_monsters[B.actors[a].cls].id, "hurt");
        if (e->type == EV_CRIT && a >= 0) (B.actors[a].is_hero ? audio_narrate_important : audio_narrate)(B.actors[a].is_hero ? "bad_crit" : "good_crit", B.actors[a].is_hero ? 75 : 70);
        snprintf(buf, sizeof(buf), e->type == EV_CRIT ? "CRIT %d" : "%d", e->value);
        popup_on(a, ev_color(e->type), buf);
        if (a >= 0) {
            pose(a, "defend");
            vis[a].flash = 0.3f;
            vis[a].hp -= e->value;
        }
        return 0.55f;
    case EV_MISS:
        skill_sound(snd_actor, snd_skill, 1);
        if (zoom_time > 0 && zoom_b < 0 && a >= 0 && a != zoom_a) zoom_b = a;
        popup_on(a, ev_color(e->type), "Miss");
        if (a >= 0) pose(a, "defend");
        return 0.55f;
    case EV_HEAL:
        audio_sfx("camp_skill_bandage");
        snprintf(buf, sizeof(buf), "+%d", e->value);
        popup_on(a, ev_color(e->type), buf);
        if (a >= 0) vis[a].hp += e->value;
        return 0.5f;
    case EV_STUN: popup_on(a, ev_color(e->type), "Stun"); return 0.45f;
    case EV_STUN_SKIP: popup_on(a, ev_color(e->type), "Stunned"); return 0.9f;
    case EV_RESIST:
        if (once(a, 0)) return 0;
        popup_on(a, ev_color(e->type), "Resist");
        return 0.4f;
    case EV_BLEED:
    case EV_BLIGHT:
        if (t < 0) audio_sfx(e->type == EV_BLEED ? "sfx_bleed_01_light_sweetener" : "char_share_imp_gas");
        if (t < 0) { /* tick at turn start */
            snprintf(buf, sizeof(buf), "%d", e->value);
            vis[a].hp -= e->value;
        } else {
            snprintf(buf, sizeof(buf), e->type == EV_BLEED ? "Bleed" : "Blight");
        }
        popup_on(a, ev_color(e->type), buf);
        return 0.5f;
    case EV_DEATHS_DOOR: audio_sfx("gen_combat_deathsdoor"); audio_narrate_important("bad_deathsdoor", 90); popup_on(a, COL_RED, "Death's Door"); return 0.6f;
    case EV_DEATHBLOW_RESISTED: popup_on(a, COL_GOOD, "Deathblow resisted"); return 0.6f;
    case EV_DEATH:
        if (a >= 0 && B.actors[a].is_corpse) { vis[a].visible = 0; return 0.3f; } /* rotted or destroyed */
        if (a >= 0 && !B.actors[a].is_hero) { audio_monster(dd_monsters[B.actors[a].cls].id, "death"); audio_narrate_once("good_killfirst", 20); if (B.actors[a].size > 1) audio_narrate("good_kill_big", 23); else audio_narrate(B.actors[a].max_hp > 20 ? "good_kill_strong" : "good_kill_weak", 33); }
        else if (a >= 0) { audio_sfx("gen_raid_bodycount"); audio_narrate_important("bad_death", 100); }
        popup_on(a, COL_RED, "Death");
        if (a >= 0) vis[a].visible = 0;
        return 0.5f;
    case EV_CORPSE: case EV_SUMMON: reset_visual(a); return 0.2f;
    case EV_STALL: { /* str_combat_wastingtime / str_bark_accelerated_stall */
        static const char *const msg[3] = {"We are wasting time!", "Stalling wears on the party...", "Reinforcements arrive!"};
        snprintf(banner, sizeof(banner), "%s", msg[e->value >= 0 && e->value < 3 ? e->value : 0]);
        banner_t = 1.6f;
        return 1.0f;
    }
    case EV_MARK: popup_on(a, COL_RED, "Marked"); return 0.4f;
    case EV_BUFF:
        if (once(a, 1)) return 0;
        popup_on(a, COL_GOOD, "Buff");
        return 0.35f;
    case EV_DEBUFF:
        if (once(a, 2)) return 0;
        popup_on(a, COL_RED, "Debuff");
        return 0.35f;
    case EV_GUARD: popup_on(t, COL_GOOD, "Guarded"); return 0.2f;
    case EV_RIPOSTE: if (e->skill >= 0) popup_on(a, COL_GOLD, "Riposte"); return 0.2f;
    case EV_STRESS: snprintf(buf, sizeof(buf), "Stress %d", e->value); popup_on(a, RGBA(230, 230, 230, 255), buf); return 0.3f;
    case EV_STRESS_HEAL: snprintf(buf, sizeof(buf), "Relief %d", e->value); popup_on(a, RGBA(230, 230, 230, 255), buf); return 0.3f;
    case EV_TORCH: return 0.1f;
    case EV_AFFLICTION:
    case EV_VIRTUE:
        audio_sfx(e->type == EV_VIRTUE ? "gen_char_resolve_virtue" : "gen_char_resolve_test");
        { char nm[48]; snprintf(nm, sizeof nm, "%s_%s", e->type == EV_VIRTUE ? "good_virtue" : "bad_afflicted", dd_traits[e->value].id);
          audio_narrate_important(nm, 100);
          audio_narrate_important(e->type == EV_VIRTUE ? "good_virtue" : "bad_afflicted", 100); }
        snprintf(banner, sizeof(banner), "%s is %s!", actor_name(&B.actors[a]), dd_traits[e->value].name);
        banner_t = 2.2f;
        popup_on(a, e->type == EV_VIRTUE ? COL_GOLD : RGBA(200, 60, 200, 255), e->type == EV_VIRTUE ? "Virtue" : "Affliction");
        return 2.0f;
    case EV_HEART_ATTACK:
        snprintf(banner, sizeof(banner), "%s suffers a heart attack!", actor_name(&B.actors[a]));
        banner_t = 2.0f;
        return 1.6f;
    case EV_ACTOUT: {
        static const char *const what[ACT_COUNT] = {"", "Lashes out", "Moves on their own", "Ignores orders",
            "Acts on their own", "Tries to flee", "Attacks an ally!", "Self-harm", "Marks self",
            "Steels themselves", "Inspires the party", "Encourages an ally", "Rallies the party", "Recovers"};
        popup_on(a, RGBA(200, 60, 200, 255), e->value < ACT_COUNT ? what[e->value] : "");
        return 0.9f;
    }
    case EV_REFUSE: popup_on(a, RGBA(200, 60, 200, 255), e->value == 1 ? "Refuses to move" : "Refuses help"); return 0.7f;
    case EV_CAPTURED:
        popup_on(t, COL_RED, "Captured!");
        snprintf(banner, sizeof(banner), "%s is captured!", actor_name(&B.actors[a]));
        banner_t = 1.6f;
        vis[a].visible = 0;
        return 1.0f;
    case EV_RELEASED:
        snprintf(banner, sizeof(banner), "%s is free!", actor_name(&B.actors[a]));
        banner_t = 1.4f;
        reset_visual(a);
        return 0.8f;
    case EV_CONTROLLED: popup_on(a, RGBA(80, 160, 255, 255), "Enthralled"); return 0.9f;
    case EV_SHAPESHIFT: popup_on(a, COL_TEXT, "Shifts"); reset_visual(a); return 0.4f;
    case EV_BONUS_TURN: return 0.05f;
    case EV_TELEPORT:
        snprintf(banner, sizeof(banner), "The party is swallowed!");
        banner_t = 1.6f;
        return 1.0f;
    default: return 0.05f;
    }
}

/* Begin playing back the engine's event list; afterwards go to state `then`. */
static void play(State then) {
    memset(shown, 0, sizeof(shown));
    ev_i = 0;
    timer = 0;
    state = BS_PLAY;
    after_play = then;
}

/* Advance playback by dt: when the wait timer elapses, show the next event. */
static void update_play(float dt) {
    timer -= dt;
    if (timer > 0) return;
    if (ev_i < B.nevents) {
        timer = show_event(&B.events[ev_i++]);
        return;
    }
    /* back to idle poses and true HP */
    for (int i = 0; i < MAX_ACTORS; i++) {
        if (!B.actors[i].used) continue;
        if (B.actors[i].alive) vis[i].visible = 1;
        vis[i].hp = B.actors[i].hp;
        vis[i].lunge = 0;
        if (vis[i].visible) pose(i, B.actors[i].is_corpse ? "dead" : "combat");
    }
    battle_clear_events(&B);
    if (B.over) {
        finish(B.over);
        return;
    }
    state = after_play;
}

/* ---------------------------------------------------------------- hero input
 * A hero's turn has two steps: choose a skill (slots 0-5), then choose a
 * target among the valid ranks (battle_targets). */

/* Absolute skill index for a panel slot. */
static int slot_skill(int slot) {
    if (slot < HERO_SKILL_SLOTS) return battle_hero_skill(&B, actor, slot);
    if (slot == SLOT_MOVE) return dd_heroes[B.actors[actor].cls].move_skill;
    return -1;
}

/* Can the current hero use this slot right now? */
static int slot_usable(int slot) {
    if (slot == SLOT_PASS) return 1;
    int sk = slot_skill(slot);
    if (sk < 0) return 0;
    if (slot == SLOT_MOVE) return !B.actors[actor].immobile;
    return battle_skill_usable(&B, actor, sk);
}

/* Ranks the hero could move to. */
static int move_mask(void) {
    const Actor *a = &B.actors[actor];
    const DDSkill *s = &dd_skills[slot_skill(SLOT_MOVE)];
    int mask = 0;
    for (int r = a->rank - s->move_fwd; r <= a->rank + s->move_back; r++)
        if (r >= 1 && r <= DD_RANKS && r != a->rank && battle_actor_at(&B, SIDE_HEROES, r) >= 0) mask |= 1 << (r - 1);
    return mask;
}

/* Enter target selection for the chosen skill. */
static void begin_targeting(void) {
    chosen_skill = slot_skill(sel);
    if (sel == SLOT_MOVE) {
        tgt_side = SIDE_HEROES;
        tgt_mask = move_mask();
    } else {
        tgt_mask = battle_targets(&B, actor, chosen_skill, &tgt_side);
    }
    if (!tgt_mask) return;
    tgt_rank = 1;
    while (tgt_rank <= DD_RANKS && !(tgt_mask & (1 << (tgt_rank - 1)))) tgt_rank++;
    state = BS_TARGET;
}

/* Input while choosing a skill. */
static void update_choose(void) {
    if (input_pressed(BTN_LEFT)) sel = (sel + NSLOTS - 1) % NSLOTS;
    if (input_pressed(BTN_RIGHT)) sel = (sel + 1) % NSLOTS;
    if (input_pressed(BTN_SQUARE)) {
        battle_pass(&B, actor);
        play(BS_NEXT);
        return;
    }
    if (input_pressed(BTN_START)) {
        pause_sel = 0;
        state = BS_PAUSE;
        return;
    }
    if (input_pressed(BTN_CROSS) && slot_usable(sel)) {
        if (sel == SLOT_PASS) {
            battle_pass(&B, actor);
            play(BS_NEXT);
        } else {
            begin_targeting();
        }
    }
}

/* Input while choosing a target: Left/Right cycles valid ranks; X confirms by
 * calling battle_perform, then we play the events back. */
static void update_target(void) {
    const DDSkill *s = &dd_skills[chosen_skill];
    int all = (s->target.flags & (RK_ALL | RK_SELF)) && sel != SLOT_MOVE;
    if (!all) {
        int dir = input_pressed(BTN_RIGHT) ? 1 : input_pressed(BTN_LEFT) ? -1 : 0;
        /* on screen, hero ranks run right-to-left and monster ranks left-to-right */
        if (tgt_side == SIDE_HEROES) dir = -dir;
        for (int k = 0; dir && k < DD_RANKS; k++) {
            tgt_rank += dir;
            if (tgt_rank < 1) tgt_rank = DD_RANKS;
            if (tgt_rank > DD_RANKS) tgt_rank = 1;
            if (tgt_mask & (1 << (tgt_rank - 1))) break;
        }
    }
    if (input_pressed(BTN_CIRCLE)) state = BS_CHOOSE;
    if (input_pressed(BTN_CROSS)) {
        if (sel == SLOT_MOVE) battle_move(&B, actor, tgt_rank);
        else battle_perform(&B, actor, chosen_skill, tgt_rank);
        play(BS_NEXT);
    }
}

/* Pause menu input (Start): resume or try to retreat. */
static void update_pause(void) {
    pause_sel = ui_menu_input(pause_sel, 2);
    if (input_pressed(BTN_CIRCLE) || input_pressed(BTN_START)) state = BS_CHOOSE;
    if (input_pressed(BTN_CROSS)) {
        if (pause_sel == 0) {
            state = BS_CHOOSE;
        } else if (B.surprise == SURPRISE_HEROES && B.round <= 1) {
            /* a surprised party can't flee on the first round */
            snprintf(banner, sizeof(banner), "Can't retreat yet!");
            banner_t = 1.2f;
            state = BS_CHOOSE;
        } else if (rng_chance(&B.rng, RETREAT_CHANCE)) {
            finish(3);
        } else {
            snprintf(banner, sizeof(banner), "Retreat failed!");
            banner_t = 1.2f;
            battle_pass(&B, actor);
            play(BS_NEXT);
        }
    }
}

/* ---------------------------------------------------------------- update */

/* Per-frame update: animation clocks first, then the state machine. */
void battle_update(float dt) {
    ui_popups_update(dt);
    if (zoom_time > 0) zoom_time -= dt;
    if (banner_t > 0) banner_t -= dt;
    for (int i = 0; i < MAX_ACTORS; i++) {
        if (!B.actors[i].used) continue;
        vis[i].t += dt;
        float goal = rank_x(&B.actors[i]);
        vis[i].x += (goal - vis[i].x) * (dt * 6 > 1 ? 1 : dt * 6);
        if (vis[i].flash > 0) vis[i].flash -= dt;
    }
    switch (state) {
    case BS_NEXT: {
        int a = battle_begin_turn(&B);
        if (a < 0) {
            play(BS_NEXT); /* plays out final events, then finish() via B.over */
            if (!B.over) finish(2);
            break;
        }
        actor = a;
        if (B.actors[a].is_hero) panel_actor = a;
        if (B.actors[a].is_hero && B.actors[a].controlled) {
            battle_hero_auto(&B, a); /* enthralled: fights for the enemy */
            play(BS_NEXT);
        } else if (B.actors[a].is_hero) {
            sel = 0;
            while (sel < NSLOTS - 1 && !slot_usable(sel)) sel++;
            play(BS_CHOOSE);
        } else {
            int skill, rank;
            if (battle_ai_choose(&B, a, &skill, &rank)) battle_perform(&B, a, skill, rank);
            else battle_pass(&B, a);
            play(BS_NEXT);
        }
        break;
    }
    case BS_PLAY: update_play(dt); break;
    case BS_CHOOSE: update_choose(); break;
    case BS_TARGET: update_target(); break;
    case BS_PAUSE: update_pause(); break;
    case BS_END:
        timer -= dt;
        if (timer <= 0 || input_pressed(BTN_CROSS)) {
            if (result == 1) state = BS_LOOT;
            else scene_switch(SCENE_DUNGEON);
        }
        break;
    case BS_LOOT:
        if (input_pressed(BTN_LEFT) && loot_sel > 0) loot_sel--;
        if (input_pressed(BTN_RIGHT) && loot_sel < nloot - 1) loot_sel++;
        if (input_pressed(BTN_CROSS) || input_pressed(BTN_START)) {
            loot_take(&g_camp.dungeon, loot, nloot);
            scene_switch(SCENE_DUNGEON);
        } else if (input_pressed(BTN_CIRCLE)) {
            scene_switch(SCENE_DUNGEON); /* leave it behind */
        }
        break;
    }
}

/* ---------------------------------------------------------------- drawing
 * Back to front: background, actors (sorted), status icons, brackets,
 * popups, banner, HUD, then state-specific overlays. */

static void draw_actor_ex(int i, float xo, float scale);
static void draw_actor(int i) { draw_actor_ex(i, -1, 1.0f); }

/* xo >= 0: draw at this x (close-up) instead of the rank position */
/* Draw actor i with optional x override and scale (used by the close-up). */
static void draw_actor_ex(int i, float xo, float scale) {
    Actor *a = &B.actors[i];
    Visual *v = &vis[i];
    if (!a->used || !v->visible) return;
    SpriteAnim *s = sprite_get(sprite_of(a), v->anim);
    if (!s) s = sprite_get(sprite_of(a), "combat");
    float x = (xo >= 0 ? xo : v->x) + (v->lunge > 0 ? (a->side == SIDE_HEROES ? 26 : -26) * scale : 0);
    uint8_t tint[4] = {255, 255, 255, 255};
    if (v->flash > 0) tint[1] = tint[2] = 150;
    if (a->stunned) tint[2] = 120;
    int loop = strcmp(v->anim, "combat") == 0 || strcmp(v->anim, "walk") == 0 || strcmp(v->anim, "dead") == 0;
    /* DD_MONSTER_FLIP (PSP build): monsters reported facing away from the heroes on hardware while
     * the host renders them right; mirror them there until the cause is found */
#ifndef DD_MONSTER_FLIP
#define DD_MONSTER_FLIP 0
#endif
    sprite_draw(s, v->t, loop, x, GROUND_Y + (scale - 1) * 30, scale, a->side == SIDE_MONSTERS && DD_MONSTER_FLIP, tint);
}

/* The close-up: dim everything, then the two focus actors larger, eased toward the centre. */
/* Close-up camera during an attack. Returns 1 if it drew the scene. */
static int draw_zoom(void) {
    if (zoom_time <= 0 || zoom_a < 0 || !B.actors[zoom_a].used || !vis[zoom_a].visible) return 0;
    float k = (ZOOM_LEN - zoom_time) / 0.15f, out = zoom_time / 0.15f;
    if (k > 1) k = 1;
    if (out < k) k = out; /* ease in, ease out */
    gfx_rect(0, 0, SCREEN_W, HUD_Y, RGBA(0, 0, 0, (int)(170 * k)));
    int focus[2] = {zoom_a, zoom_b};
    for (int f = 0; f < 2; f++) {
        int i = focus[f];
        if (i < 0 || !B.actors[i].used || !vis[i].visible) continue;
        int left = B.actors[i].side == SIDE_HEROES; /* heroes on the left, as in the original */
        float goal = zoom_b < 0 ? SCREEN_W / 2 : left ? 160 : 320;
        draw_actor_ex(i, vis[i].x + (goal - vis[i].x) * k, 1.0f + 0.3f * k);
    }
    return 1;
}

/* HP bar, stress pips and status-effect tray icons under an actor. */
static void draw_status(int i) {
    Actor *a = &B.actors[i];
    Visual *v = &vis[i];
    if (!a->used || !v->visible) return;
    float x = v->x - 19, y = GROUND_Y + 4;
    if (a->is_corpse) { /* corpses have a grey HP bar (they can be hit, and rot away) */
        ui_bar(v->x - 18, y + 2, 36, 3, (float)v->hp / (a->max_hp > 0 ? a->max_hp : 1), RGBA(150, 150, 150, 255), RGBA(30, 30, 30, 220));
        return;
    }
    hud_bars(v->x, y, v->hp, a->max_hp, a->is_hero ? a->hero->stress : -1, a->deaths_door);
    /* status tray: one icon per active state, wrapping upwards after 4 */
    int st[TRAY_COUNT], n = 0, bl = 0, po = 0, ho = 0, up = 0, down = 0, dde = 0, dis = 0, te = 0;
    for (int k = 0; k < MAX_DOTS; k++) {
        bl |= a->bleed[k].rounds > 0;
        po |= a->blight[k].rounds > 0;
        ho |= a->horror[k].rounds > 0;
    }
    for (int k = 0; k < a->nbuffs; k++) {
        if (a->buffs[k].rule_buff >= 0) continue; /* quirk/trinket rules: always there, not a status */
        if (a->buffs[k].amount > 0) up = 1;
        else if (a->buffs[k].amount < 0) down = 1;
    }
    if (a->is_hero) {
        for (int k = 0; k < a->hero->nebuffs; k++) {
            dde |= a->hero->ebuffs[k].battles == HB_UNTIL_CAMP;
            te |= a->hero->ebuffs[k].battles == -1;
        }
        for (int k = 0; k < HERO_QUIRKS; k++) dis |= a->hero->quirks[k] && dd_quirks[a->hero->quirks[k] - 1].disease;
    }
    if (a->stunned) st[n++] = TRAY_STUN;
    if (a->marked) st[n++] = TRAY_TAG;
    if (bl) st[n++] = TRAY_BLEED;
    if (po) st[n++] = TRAY_POISON;
    if (ho) st[n++] = TRAY_HORROR;
    if (a->guarded_by >= 0) st[n++] = TRAY_GUARD;
    if (a->riposte) st[n++] = TRAY_RIPOSTE;
    if (a->stealth) st[n++] = TRAY_STEALTH;
    if (up) st[n++] = TRAY_BUFF;
    if (down) st[n++] = TRAY_DEBUFF;
    if (a->deaths_door) st[n++] = TRAY_DEATHSDOOR;
    if (dde) st[n++] = TRAY_DD_EFFECTS;
    if (dis) st[n++] = TRAY_DISEASE;
    if (te) st[n++] = TRAY_TOWN_EVENT;
    for (int k = 0; k < n; k++) {
        Image *ic = hud_tray(st[k]);
        if (ic) image_draw(ic, x + (k % 4) * 11, y - 11 - (k / 4) * 11, 10, 10, RGBA(255, 255, 255, 255));
    }
    if (a->is_hero && a->controlled) text_draw(FONT_SMALL, v->x, y + 9, "Enthralled", RGBA(80, 160, 255, 255), 1);
    else if (a->is_hero && a->hero->affliction) {
        int virtue = 0;
        const char *nm = stress_affliction_name(a->hero, &virtue);
        text_draw(FONT_SMALL, v->x, y + 9, nm, virtue ? COL_GOLD : RGBA(210, 90, 210, 255), 1);
    }
}

/* Brackets over every legal target / the selected one. */
static void draw_target_markers(void) {
    if (state != BS_TARGET) return;
    const DDSkill *s = &dd_skills[chosen_skill];
    int all = (s->target.flags & (RK_ALL | RK_SELF)) && sel != SLOT_MOVE;
    for (int r = 1; r <= DD_RANKS; r++) {
        if (!(tgt_mask & (1 << (r - 1)))) continue;
        int i = battle_actor_at(&B, tgt_side, r);
        if (i < 0) continue;
        int chosen = all || r == tgt_rank;
        hud_bracket(tgt_side == SIDE_HEROES ? BRACKET_TARGET_ALLY : BRACKET_TARGET, vis[i].x, GROUND_Y + 16,
                    chosen ? 255 : 90);
    }
}

/* Right half while aiming at a monster: its name, HP and defences instead of the map. */
/* Monster stat card shown while aiming at a monster. */
static void draw_monster_info(int i) {
    const Actor *m = &B.actors[i];
    const DDMonsterClass *mc = &dd_monsters[m->cls];
    float x = 248, y = HUD_Y + 6;
    gfx_rect(240, HUD_Y, 180, HUD_H, RGBA(8, 6, 5, 255));
    ui_frame(242, HUD_Y + 2, 176, HUD_H - 4, COL_EDGE);
    /* the original's target panel: red name bar with HP, type + PROT/DODGE/SPD, then resistances */
    gfx_rect(244, HUD_Y + 4, 172, 14, RGBA(70, 12, 10, 255));
    text_draw(FONT_BODY, x, y - 1, actor_name(m), RGBA(220, 70, 50, 255), 0);
    char hp[24];
    snprintf(hp, sizeof(hp), "HP: %d/%d", m->hp, m->max_hp);
    text_draw(FONT_SMALL, 412, y, hp, RGBA(220, 70, 50, 255), 2);
    static const char *const tn[] = {"Human", "Beast", "Unholy", "Eldritch", "Corpse", "Cauldron", "Cosmic", "Structure"};
    const char *type = "";
    for (int k = 0; k < 8 && !type[0]; k++)
        if (mc->types & (1u << k)) type = tn[k];
    text_draw(FONT_SMALL, x, y + 16, type, COL_TEXT, 0);
    text_printf(FONT_SMALL, x + 70, y + 16, COL_TEXT, "PROT: %d%%  DODGE: %d  SPD: %d", (int)(actor_stat(m, STAT_PROT) * 100 + 0.5f),
                (int)(actor_stat(m, STAT_DEF) * 100 + 0.5f), (int)actor_stat(m, STAT_SPD));
    gfx_rect(248, y + 28, 164, 1, RGBA(90, 80, 70, 255));
    static const char *const rn[] = {"Stun", "Blight", "Bleed", "Debuff", "Move"};
    static const int rk[] = {RES_STUN, RES_BLIGHT, RES_BLEED, RES_DEBUFF, RES_MOVE};
    static const uint32_t rc[] = {0xff40c8e8, 0xff40e0a0, 0xff3030e0, 0xff30a0f0, 0xffe8b060}; /* ABGR: yellow, green, red, orange, blue */
    for (int k = 0; k < 5; k++)
        text_printf(FONT_SMALL, x + (k % 3) * 56, y + 32 + (k / 3) * 10, rc[k], "%s %d%%", rn[k], (int)(mc->res[rk[k]] * 100 + 0.5f));
    if (m->marked) text_draw(FONT_SMALL, x, y + 54, "Marked", COL_RED, 0);
}

/* Skill tooltip above the HUD while choosing. */
/* Tooltip describing the highlighted skill. */
static void draw_skill_tip(const Actor *a) {
    char l1[48] = "", l2[64] = "";
    if (sel == SLOT_PASS) {
        snprintf(l1, sizeof(l1), "Pass");
    } else if (sel == SLOT_MOVE) {
        snprintf(l1, sizeof(l1), "Move");
        snprintf(l2, sizeof(l2), "Swap places with an ally");
    } else {
        int sk = slot_skill(sel);
        if (sk < 0) return;
        const DDSkill *s = &dd_skills[sk];
        snprintf(l1, sizeof(l1), "%s", s->name);
        if (s->type == SK_MELEE || s->type == SK_RANGED)
            snprintf(l2, sizeof(l2), "ACC %d  DMG %+d%%  CRIT %+d%%", (int)(s->atk * 100 + 0.5f), (int)(s->dmg_mod * 100),
                     (int)(s->crit * 100 + 0.5f));
        else if (s->heal_hi)
            snprintf(l2, sizeof(l2), "Heal %d-%d", s->heal_lo, s->heal_hi);
        if (s->eff_count) {
            size_t n = strlen(l2);
            snprintf(l2 + n, sizeof(l2) - n, "%s%s", n ? "  " : "", dd_effects[dd_skill_effects[s->eff_first]].name);
        }
    }
    (void)a;
    float w = text_width(FONT_BODY, l1);
    float w2 = text_width(FONT_SMALL, l2);
    if (w2 > w) w = w2;
    w += 10;
    float y = HUD_Y - (l2[0] ? 26 : 16) - 12;
    gfx_rect(62, y, w, l2[0] ? 26 : 16, RGBA(0, 0, 0, 200));
    ui_frame(62, y, w, l2[0] ? 26 : 16, COL_EDGE);
    text_draw(FONT_BODY, 67, y + 2, l1, COL_GOLD, 0);
    if (l2[0]) text_draw(FONT_SMALL, 67, y + 15, l2, COL_TEXT, 0);
}

/* The bottom hero panel for the acting hero. */
static void draw_panel(void) {
    int choosing = state == BS_CHOOSE || state == BS_TARGET || state == BS_PAUSE;
    int pa = choosing && actor >= 0 && B.actors[actor].is_hero ? actor : panel_actor;
    HudHero v;
    if (pa >= 0 && B.actors[pa].used && B.actors[pa].is_hero) {
        hud_hero(&v, B.actors[pa].hero, &B.actors[pa]);
        if (choosing && pa == actor) {
            v.usable = 0;
            for (int s = 0; s < NSLOTS; s++)
                if (slot_usable(s)) v.usable |= (uint8_t)(1 << s);
            v.sel = sel;
        }
    } else {
        memset(&v, 0, sizeof(v));
        v.sel = -1;
    }
    hud_draw(&v, &g_camp.dungeon, battle_corr_frac);
    if (state == BS_TARGET && tgt_side == SIDE_MONSTERS) {
        int i = battle_actor_at(&B, SIDE_MONSTERS, tgt_rank);
        if (i >= 0) draw_monster_info(i);
    }
    if (state == BS_CHOOSE || state == BS_TARGET) draw_skill_tip(&B.actors[actor]);
}

/* Draw the whole battle scene. */
void battle_draw(void) {
    grade_scene(dungeon_grade_dir(), B.torch);
    if (bg.tiles) {
        /* centre a screen-wide window of the room wall; the HUD covers the floor edge */
        float sw = SCREEN_W * bg.h / (float)SCREEN_H;
        float sx = (bg.w - sw) / 2;
        if (sx < 0) sx = 0;
        image_draw_part(&bg, sx, sw < bg.w ? sw : bg.w, 0, GROUND_Y - 206, SCREEN_W, SCREEN_H, RGBA(255, 255, 255, 255));
    }
    /* darkness: the dungeon's colour grade for this light band, or a plain tint without one */
    int dark = (100 - B.torch) * 140 / 100;
    if (dark > 0 && !grade_active()) gfx_rect(0, 0, SCREEN_W, HUD_Y, RGBA(0, 0, 0, dark));
    ui_vignette(0, 0, SCREEN_W, HUD_Y, B.torch);

    for (int i = 0; i < MAX_ACTORS; i++)
        if (B.actors[i].used && !B.actors[i].alive) draw_actor(i);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < MAX_ACTORS; i++)
            if (B.actors[i].used && B.actors[i].alive && (i == actor) == pass && !(zoom_time > 0 && (i == zoom_a || i == zoom_b)))
                draw_actor(i);
    draw_zoom();
    if (actor >= 0 && B.actors[actor].used && B.actors[actor].is_hero && vis[actor].visible && state != BS_END)
        hud_bracket(BRACKET_SELECTED, vis[actor].x, GROUND_Y + 16, 255);
    draw_target_markers();
    for (int i = 0; i < MAX_ACTORS; i++) draw_status(i);
    ui_popups_draw();

    hud_torch(B.torch);
    text_printf(FONT_SMALL, 4, 2, COL_DIM, "Round %d", B.round);
    if (banner_t > 0 && banner[0]) {
        float w = text_width(FONT_TITLE, banner) + 20;
        ui_panel(SCREEN_W / 2 - w / 2, 46, w, 24);
        text_draw(FONT_TITLE, SCREEN_W / 2, 49, banner, COL_TEXT, 1);
    }
    draw_panel();
    if (state == BS_PAUSE) {
        static const char *const items[] = {"Resume", "Retreat"};
        ui_panel(SCREEN_W / 2 - 60, 80, 120, 44);
        ui_menu(SCREEN_W / 2 - 56, 84, 112, items, 2, pause_sel);
    }
    if (state == BS_LOOT) {
        hud_loot("Loot", loot, nloot, loot_sel);
        hud_hints(BG_LEFTRIGHT " Inspect  " BG_CROSS " Take all  " BG_CIRCLE " Leave");
    }
    if (state == BS_CHOOSE) hud_hints(BG_LEFTRIGHT " Skill  " BG_CROSS " Use  " BG_SQUARE " Pass  " BG_START " Menu");
    else if (state == BS_TARGET) hud_hints(BG_LEFTRIGHT " Target  " BG_CROSS " Confirm  " BG_CIRCLE " Back");
}
