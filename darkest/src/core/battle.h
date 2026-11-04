/*
 * battle.h - the combat engine's public interface and data model.
 *
 * Pure game logic: no graphics, no input.  The same code runs in the real game
 * (scene_battle.c), the PC battle simulator (battle_sim.c) and tests.
 *
 * HOW A BATTLE RUNS (the "driver loop" every caller follows):
 *   battle_init -> battle_add_hero/monster (place everyone) ->
 *   battle_roll_surprise ->
 *   repeat { actor = battle_begin_turn();            // who acts now?
 *            choose a skill (UI for heroes, battle_ai_choose for monsters);
 *            battle_perform / battle_move / battle_pass; }   // ends the turn
 *   until b->over != 0.
 *
 * EVENTS: the engine never draws.  Every happening (hit, miss, heal, stun,
 * death...) is appended to b->events as a small BattleEvent.  The presentation
 * layer plays them back later with animation and timing.  This keeps rules
 * and visuals strictly separate - visuals must never change engine state.
 *
 * ROUNDS: each round every actor gets its turn in initiative (speed) order.
 * Ranks: 1 = front line, 4 = back.  Big monsters occupy several ranks.
 */
#ifndef DD_BATTLE_H
#define DD_BATTLE_H

#include <stdint.h>
#include "dd_types.h"
#include "rng.h"

/* Side index of an actor: 0 = player party, 1 = monsters. */
#define SIDE_HEROES 0
#define SIDE_MONSTERS 1
#define MAX_PER_SIDE 4
/* Fixed array sizes keep Battle a plain struct (no malloc, trivially copyable). */
#define MAX_ACTORS (MAX_PER_SIDE * 2 + 4) /* room for summons / corpses */
#define DEATH_DAMAGE (-2) /* battle_damage attacker: another monster's death_damage */
#define MAX_DOTS 6
#define MAX_BUFFS 40
#define MAX_EVENTS 64
#define HERO_SKILL_SLOTS 4

/* Stat change a hero carries between fights (curios, camping). */
typedef struct {
    uint8_t stat, sub, mode;
    int8_t battles;            /* fights left, -1 = until the expedition ends, HB_UNTIL_CAMP */
    float amount;
} HeroBuff;

typedef struct { int8_t amount, rounds; } Dot;

#define HERO_BUFFS 6
#define HERO_DOTS 3                /* bleed / blight carried between fights and ticking per hall tile */
#define HB_UNTIL_CAMP (-2)     /* death's door recovery: cleared by camping */
#define HERO_QUIRKS 13       /* 5 positive + 5 negative + 3 diseases (rules.json) */
#define HERO_TRINKETS 2

/* Persistent hero: the long-lived record kept in the roster and SAVED to disk
 * (so changing this struct means bumping SAVE_VERSION). It holds progression
 * (level, gear, quirks, trinkets) and current HP/stress, but NOT the temporary
 * in-fight state - that is in Actor below, which points back here. */
typedef struct {
    int16_t cls;               /* index into dd_heroes */
    char name[16];
    uint8_t level;             /* resolve level 0..6 */
    uint16_t xp;
    uint8_t weapon_lvl, armour_lvl; /* 0..4 */
    uint8_t skill_lvl[8];      /* per class skill 0..4 */
    int8_t equipped[HERO_SKILL_SLOTS]; /* class skill numbers, -1 empty */
    int16_t hp;
    int16_t stress;            /* 0..200; see stress.c */
    uint8_t affliction;        /* 0 none, else dd_traits index + 1 (affliction or virtue) */
    uint8_t alive;
    uint8_t trait_history[16]; /* times each trait was rolled (raises its weight) */
    uint8_t deaths_door_visits;/* this expedition; later visits stress less */
    uint8_t busy;              /* in a town activity this week */
    HeroBuff ebuffs[HERO_BUFFS];
    uint8_t nebuffs;
    uint8_t quirks[HERO_QUIRKS];  /* dd_quirks index + 1, 0 = empty */
    uint16_t quirk_locked;     /* bit per slot */
    int16_t trinkets[HERO_TRINKETS]; /* dd_trinkets index + 1, 0 = empty */
    uint8_t dd_veteran;        /* finished a Darkest Dungeon quest: never goes back (never_again "strict") */
    uint8_t missing;           /* weeks left away (activity go_missing side effect) */
    uint8_t missing_act;       /* activity (Hero.busy code) they went missing from */
    uint8_t committed;         /* activity_lock: stays in this activity (busy code) next week */
    uint8_t act_relief;        /* stress the activity relieved this week (activity log) */
    uint8_t act_cured;         /* affliction (dd_traits + 1) the activity ended, 0 none */
    Dot bleed[HERO_DOTS], blight[HERO_DOTS]; /* out of battle (Raid::ApplyDot on each new hall tile) */
} Hero;


/* A live buff on an Actor (see DDBuff in dd_types.h for the static form).
 * Recomputed into actor_stat() whenever a stat is read. */
typedef struct {
    uint8_t stat, sub, mode;   /* DDStat, sub-stat, BUFF_ADD/BUFF_MULTIPLY */
    int8_t rounds;             /* -1: until battle end */
    float amount;
    uint8_t from_effect;       /* removed by clear_debuff only if negative */
    int16_t rule_buff;         /* dd_buffs index whose rule gates it (quirks), -1 always on */
} Buff;

/* One combatant IN THIS FIGHT (hero or monster). Temporary: created by
 * battle_add_*, discarded after the fight. For heroes `hero` points at the
 * persistent roster record; HP is copied back to it when the fight ends.
 * Status fields are mostly "rounds remaining" counters (0 = not active). */
typedef struct {
    uint8_t used;
    uint8_t side;
    uint8_t is_hero;
    int16_t cls;               /* hero or monster class index */
    int16_t look;              /* monster class whose sprite is drawn (corpses use the dead one's) */
    Hero *hero;                /* heroes only */
    uint8_t rank;              /* 1..4, front = 1 */
    uint8_t size;              /* ranks occupied */
    int16_t hp, max_hp;
    uint8_t alive;
    uint8_t deaths_door;       /* hero at 0 HP */
    uint8_t is_corpse;
    uint8_t stunned;
    uint8_t marked;            /* rounds */
    uint8_t stealth;           /* rounds */
    uint8_t riposte;           /* rounds */
    uint8_t immobile;          /* rounds */
    int8_t guarded_by;         /* actor index or -1 */
    uint8_t guard_rounds;
    int8_t turns_left;         /* turns remaining this round */
    int16_t initiative;
    Dot bleed[MAX_DOTS], blight[MAX_DOTS], horror[MAX_DOTS]; /* horror: stress per turn */
    uint8_t no_loot;           /* summoned by an effect with summon_can_spawn_loot false */
    int16_t born_round;        /* round it entered the battle (corpse life_time) */
    uint16_t perf_turn;        /* Actor::GetPerformingTurn: turns acted this battle (AI desires) */
    uint8_t round_turn;        /* turns acted this round (first_initiative_only) */
    uint8_t heart_attacked;    /* on death's door from a heart attack: heart-attack recovery buffs */
    Buff buffs[MAX_BUFFS];
    int nbuffs;
    uint8_t skill_uses[16];    /* per-battle use counts (hero skill slot or monster skill) */
    uint8_t killed_by_crit;
    int8_t prisoner;           /* captor: the hero it holds, -1 none */
    int8_t captured_by;        /* hero: the captor holding them, -1 none */
    uint8_t controlled;        /* hero: rounds left fighting for the enemy (Siren) */
    int8_t pool;               /* shared health pool index, -1 none */
    uint8_t companion_on;      /* companion buffs currently applied */
} Actor;

/* Kinds of recorded happenings. See BattleEvent for what the fields mean. */
typedef enum {
    EV_TURN, EV_SKILL, EV_MISS, EV_DODGE, EV_HIT, EV_CRIT, EV_HEAL, EV_STUN, EV_RESIST,
    EV_BLEED, EV_BLIGHT, EV_DOT_DAMAGE, EV_DEATHS_DOOR, EV_DEATHBLOW_RESISTED, EV_DEATH,
    EV_CORPSE, EV_MOVE, EV_BUFF, EV_DEBUFF, EV_MARK, EV_GUARD, EV_RIPOSTE, EV_STUN_SKIP,
    EV_STRESS, EV_STRESS_HEAL, EV_SUMMON, EV_TORCH, EV_BATTLE_WON, EV_BATTLE_LOST,
    EV_AFFLICTION, EV_VIRTUE, EV_HEART_ATTACK, EV_ACTOUT, EV_REFUSE,
    EV_CAPTURED, EV_RELEASED, EV_CONTROLLED, EV_SHAPESHIFT, EV_BONUS_TURN, EV_TELEPORT,
    EV_STALL /* value: 0 warning, 1 party stress, 2 reinforcements */
} EventType;

/* One log entry for the animation layer: who (actor) did what (type) to whom
 * (target), with a number (damage, heal amount...) and the skill used. */
typedef struct {
    uint8_t type;
    int8_t actor, target;
    int16_t value;
    int16_t skill;
} BattleEvent;

/* The whole state of one fight. Small enough to copy, which tests and the
 * simulator rely on. */
typedef struct Battle {
    Rng rng;                   /* seeded: same seed = same fight */
    Actor actors[MAX_ACTORS];
    int round;
    int order[MAX_ACTORS];     /* initiative order for the round */
    int norder, cursor;
    int current;               /* actor whose turn it is, -1 none */
    int torch;                 /* 0..100, shared with the dungeon */
    int level;                 /* dungeon difficulty (1, 3, 5) for stress scaling */
    int over;                  /* 0 running, 1 heroes won, 2 heroes lost/fled, 4 party teleported away */
    int turns_without_damage;  /* stall detection */
    int hero_miss_streak;      /* consecutive hero attacks that hit nothing */
    BattleEvent events[MAX_EVENTS];
    int nevents;
    /* shared health pools (Formless Flesh) */
    int16_t pool_hp[4], pool_max[4];
    uint8_t pool_id[4];
    int npools;
    /* bonus actions outside the turn order (bosses) */
    int8_t bonus_actor[8];
    int16_t bonus_skill[8];
    int nbonus;
    int forced_skill;          /* skill the current actor must use (-1 = free choice) */
    int round_finished;        /* round-finish bonus actions already queued */
    int dungeon;               /* DG_* of the fight (stall reinforcements), -1 none */
    int stall_rounds;          /* consecutive stalling rounds (Battle::UpdateStallPenalty) */
    int active_skills;         /* hero skills this round that count against stalling */
    int last_skill, last_performer;
    uint8_t damaged[MAX_ACTORS];
    int acted;                 /* an action happened since the last post-turn check */
    int summoned;              /* a summon already happened during this action */
    int surprise;              /* SURPRISE_*: who was caught off guard this fight */
} Battle;

enum { SURPRISE_NONE, SURPRISE_MONSTERS, SURPRISE_HEROES };

/* Setup: create the fight, then seat each combatant at a rank (1..4).
 * Each add returns the new actor index, or -1 if it didn't fit. */
void battle_init(Battle *b, uint32_t seed, int torch);
int battle_add_hero(Battle *b, Hero *h, int rank);
int battle_add_monster(Battle *b, int monster_class, int rank);

/* Roll the opening surprise once every actor is in place. scouted: the fight's room or hall was
 * scouted; forced: 0 roll, SURPRISE_HEROES for a camp ambush, -1 surprise disabled. Surprised
 * heroes get their ranks shuffled; surprised monsters act after every hero in round 1. */
int battle_roll_surprise(Battle *b, int scouted, int forced);
/* Turn flow. Returns the actor whose turn it is (after DoTs/stun handling), or -1 if the
 * battle is over. Events from the turn start (bleed ticks, stun skip...) are recorded. */
int battle_begin_turn(Battle *b);

/* Skill helpers for the current actor. `skill` is an absolute dd_skills index. */
int battle_hero_skill(const Battle *b, int actor, int slot);      /* -1 if empty */
int battle_skill_usable(const Battle *b, int actor, int skill);
/* Bitmask of target ranks (bit r-1) that are valid; side via *side_out. */
int battle_targets(const Battle *b, int actor, int skill, int *side_out);

/* Perform a skill on the given target rank (ignored for self/all-target skills). Ends the turn. */
void battle_perform(Battle *b, int actor, int skill, int target_rank);
void battle_pass(Battle *b, int actor);
/* Heroes only: swap with the ally at target_rank. */
void battle_move(Battle *b, int actor, int target_rank);

/* Monster AI picks skill and target. Returns 0 if no usable skill (then pass). */
int battle_ai_choose(Battle *b, int actor, int *skill, int *target_rank);
/* A hero acting on their own (mind control, afflictions): random usable skill and target. */
void battle_hero_auto(Battle *b, int actor);
/* Side an actor's offensive skills target (flips while mind-controlled). */
int battle_enemy_side(const Battle *b, int actor);

/* Queries: who stands at a rank; an actor's stat after all buffs; display name. */
int battle_actor_at(const Battle *b, int side, int rank);
float actor_stat(const Actor *a, DDStat stat);
const char *actor_name(const Actor *a);

/* Event log helpers: the presentation layer clears the log after consuming it. */
void battle_clear_events(Battle *b);
void battle_event(Battle *b, EventType t, int actor, int target, int value, int skill);

#endif
