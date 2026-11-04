/*
 * dd_types.h - the SHAPE of all static game data.
 *
 * Everything the original game defines in text/JSON files - heroes, monsters,
 * skills, status effects, loot, quests, town buildings, quirks... - is
 * converted by tools/export_game_data.py into big constant C arrays in
 * extracted/gen/dd_data.c (local only, never shared).  This header declares
 * the struct for each kind of row and the `extern` arrays holding them.
 *
 * HOW TO READ THIS FILE
 *  - A "table" is a const array of structs (e.g. dd_skills[]).  A reference
 *    from one row to another is an INDEX into some other table.
 *  - Variable-length lists are stored FLATTENED: a row has `x_first` and
 *    `x_count`, meaning "my entries are dd_x[x_first .. x_first+x_count-1]".
 *    This avoids pointers/allocations and keeps the data in read-only memory.
 *  - Flags are bit masks (1 << n) so several can be OR'ed into one byte.
 *  - Trailing comments like DDxxx after a field name the enum that field holds.
 *
 * The game's data model in one paragraph: a HERO CLASS has SKILLS; a skill has
 * EFFECTS (stun, bleed, buff...); an effect is a list of OPS that change the
 * target; BUFFs are timed/conditional stat modifiers.  MONSTER CLASSES have
 * skills plus a BRAIN (AI weights).  ENCOUNTERS list which monsters appear in
 * which dungeon.  Adding a field means editing BOTH this file and the exporter.
 */
#ifndef DD_TYPES_H
#define DD_TYPES_H

/* Game data tables are filled at run time from assets/gen/dd_data.bin (see dd_load.h); only dd_load_gen.c writes them. */
#ifdef DD_LOADER
#define DD_C
#else
#define DD_C const
#endif

#include <stdint.h>

/* A party/monster line has 4 positions ("ranks"), 1 = front .. 4 = back. */
#define DD_RANKS 4

/* ---- ranks ---------------------------------------------------------------- */

enum {
    RK_ALL      = 1 << 0, /* '~' hits every listed rank at once */
    RK_FRIENDLY = 1 << 1, /* '@' targets own side */
    RK_SELF     = 1 << 2, /* empty target string: performer only */
};

/* Which ranks a skill can be launched from / aimed at, e.g. "1,2" or "~3,4". */
typedef struct {
    uint8_t mask;  /* bit r-1 set => rank r valid */
    uint8_t flags; /* RK_* */
} DDRanks;

/* ---- resistances / enemy types -------------------------------------------
 * Resistances are % chances to shrug off a status. Monster types (man, beast,
 * unholy...) let skills/trinkets/quirks do extra vs a type ("+15% vs Beast"). */

typedef enum {
    RES_STUN, RES_BLIGHT, RES_BLEED, RES_DISEASE, RES_MOVE, RES_DEBUFF, RES_DEATHBLOW, RES_TRAP,
    RES_COUNT
} DDResist;

typedef enum {
    MT_MAN, MT_BEAST, MT_UNHOLY, MT_ELDRITCH, MT_CORPSE, MT_CAULDRON, MT_COSMIC, MT_STRUCTURE,
    MT_COUNT
} DDMonsterType;

/* ---- buffs -----------------------------------------------------------------
 * A buff = "change STAT by AMOUNT (added or multiplied) for DURATION", and
 * optionally only while a RULE holds (e.g. only in torchlight above 50). Quirks,
 * trinkets, camping, afflictions and skills are all built from buffs. */

typedef enum {
    STAT_NONE,
    STAT_ATK, STAT_DEF, STAT_PROT, STAT_SPD, STAT_CRIT,
    STAT_DMG_LO, STAT_DMG_HI, STAT_MAX_HP,
    STAT_RES,               /* sub = DDResist */
    STAT_HEAL_RECEIVED,     /* fraction */
    STAT_CRIT_RECEIVED,     /* fraction */
    STAT_STRESS_DMG_RECEIVED, /* fraction; used by the stress module */
    STAT_STRESS_HEAL_RECEIVED, /* fraction */
    STAT_FOOD,              /* food_consumption_percent */
    STAT_RESOLVE,           /* resolve_check_percent: lowers the affliction chance */
    STAT_HEAL_DEALT,        /* hp_heal_percent: heals this actor casts */
    STAT_EFFECT_CHANCE,     /* sub = DDResist the effect fights (stun_chance, bleed_chance...) */
    STAT_SURPRISE,          /* sub 0 = party_surprise_chance, 1 = monsters_surprise_chance */
    STAT_DMG_RECEIVED,      /* damage_received_percent: fraction */
    STAT_SCOUTING,          /* scouting_chance (out of battle only) */
    STAT_COUNT
} DDStat;

enum { BUFF_ADD = 0, BUFF_MULTIPLY = 1 };

typedef enum { DUR_ROUNDS, DUR_COMBAT_END, DUR_QUEST_END, DUR_ACTIVITY_END } DDDurationType;

typedef struct {
    const char *id;
    uint8_t stat;     /* DDStat */
    uint8_t sub;      /* DDResist for STAT_RES */
    uint8_t mode;     /* BUFF_ADD / BUFF_MULTIPLY */
    uint8_t duration_type;
    int8_t duration;
    float amount;
    uint8_t rule;     /* DDBuffRule: when the buff counts (quirks, trinkets) */
    float rule_f;     /* light / HP fraction / stress / monster count threshold */
    int8_t rule_type; /* monster type for RULE_MONSTER_TYPE*, -1 none */
    uint8_t rule_not; /* is_false_rule */
    const char *rule_s; /* RULE_SKILL: the skill id it reacts to (NULL otherwise) */
} DDBuff;

/* Condition under which a conditional buff is active (checked in formulas.c). */
typedef enum {
    RULE_ALWAYS, RULE_IN_DUNGEON, RULE_MONSTER_TYPE, RULE_MONSTER_TYPE_COUNT_MIN, RULE_LIGHT_ABOVE,
    RULE_LIGHT_BELOW, RULE_HP_BELOW, RULE_FIRST_ROUND, RULE_RANGED, RULE_MELEE, RULE_IN_CAMP,
    RULE_IN_ACTIVITY, RULE_STRESS_ABOVE, RULE_IN_CORRIDOR, RULE_WALKING_BACKWARDS, RULE_OTHER,
    RULE_HP_ABOVE, RULE_DEATHS_DOOR, RULE_IN_RANK /* rule_f = rank - 1 */,
    RULE_SKILL /* the current attack uses skill rule_s (Talisman of the Flame vs Revelation) */
} DDBuffRule;

/* ---- effects ---------------------------------------------------------------
 * An effect is what a skill DOES besides raw damage: a chance, a duration,
 * who it applies to, and a short list of OPs executed by effects.c. */

typedef enum {
    ET_TARGET, ET_PERFORMER, ET_GLOBAL, ET_PERFORMER_GROUP_OTHER, ET_TARGET_GROUP
} DDEffectTarget;

typedef enum {
    /* status / damage over time */
    OP_STUN, OP_UNSTUN, OP_BLEED, OP_BLIGHT, OP_CURE, OP_CURE_BLEED, OP_CURE_BLIGHT,
    /* hp / stress (stress ops are handed to stress.c) */
    OP_HEAL, OP_STRESS, OP_HEAL_STRESS, OP_KILL,
    /* position */
    OP_PUSH, OP_PULL, OP_SHUFFLE_TARGET, OP_SHUFFLE_PARTY, OP_IMMOBILIZE, OP_UNIMMOBILIZE,
    /* tags */
    OP_MARK, OP_UNMARK, OP_GUARD, OP_CLEAR_GUARDING, OP_CLEAR_GUARDED,
    OP_RIPOSTE, OP_STEALTH, OP_UNSTEALTH, OP_CLEAR_DEBUFF,
    /* environment */
    OP_TORCH_INC, OP_TORCH_DEC,
    /* temporary stat change: arg = DDStat, v = amount; *_MUL ops multiply */
    OP_STAT_ADD, OP_STAT_MUL,
    /* named buff: arg = buff index */
    OP_BUFF,
    /* summon: arg = monster class index, v = chance weight */
    OP_SUMMON,
    /* boss mechanics: pull a hero into a captor (Hag's cauldron, anchor); mind control (Siren) */
    OP_CAPTURE, OP_CONTROL,
    /* disease (hero targets, resisted by RES_DISEASE): arg = quirk index, -1 any */
    OP_DISEASE,
    /* dotStress: stress at the start of each turn ("horror"), v = amount */
    OP_HORROR,
    /* kill_enemy_types corpse: every corpse on the target's side is cleared */
    OP_CLEAR_CORPSES,
    OP_COUNT
} DDEffectOp;

typedef struct {
    uint8_t op;   /* DDEffectOp */
    int16_t arg;
    float v;
} DDEffOp;

enum {
    EF_ON_HIT     = 1 << 0,
    EF_ON_MISS    = 1 << 1,
    EF_QUEUE      = 1 << 2, /* resolved after the attack (stun, dots) */
    EF_ON_DEATH   = 1 << 3, /* can apply to a target that died from the hit */
    EF_APPLY_ONCE = 1 << 4,
    EF_SWAP       = 1 << 5, /* swap source and target */
    EF_DEBUFF     = 1 << 6, /* negative curio_result_type: resisted by RES_DEBUFF if it changes stats */
    EF_COMBAT_END = 1 << 7, /* stat changes last until the battle ends */
};

enum { KS_NONE, KS_MARKED, KS_BLIGHTED, KS_STUNNED, KS_BLEEDING };

enum {
    /* skill-instant stat bonus for the performer: only counts while this skill resolves, and
     * only against targets meeting its conditions (Actor::ApplySkillInstantBuffs) */
    EF2_INSTANT = 1 << 0,
    EF2_HAS_DURATION = 1 << 1,
    EF2_SUMMON_NO_LOOT = 1 << 2, /* summon_can_spawn_loot false: the summoned monsters drop nothing */
    EF2_NO_CRIT_ROLL = 1 << 3,   /* crit_doesnt_apply_to_roll: a crit doesn't raise this effect's chance */
    EF2_HEAL_CAN_CRIT = 1 << 4,  /* source_heal_type hero_skill: the heal rolls a hero_skill heal crit */
    EF2_VIRTUE_BLOCKS = 1 << 5,  /* virtue_blockable_chance: a virtuous hero shrugs it off (Bosun's capture) */
};

typedef struct {
    const char *name;
    uint8_t target;         /* DDEffectTarget */
    uint8_t flags;          /* EF_* */
    uint8_t monster_types;  /* only applies vs these MT_* (bitmask), 0 = any */
    uint8_t key_status;     /* KS_*: only applies if the target has this status */
    uint8_t flags2;         /* EF2_* */
    int8_t duration;        /* rounds; -1 = until cleared */
    float chance;           /* base chance before resistances */
    uint16_t op_first;
    uint8_t op_count;
    uint8_t summon_count;   /* monsters summoned per use (the Ancestor calls three) */
} DDEffect;

/* ---- skills ----------------------------------------------------------------
 * One skill row serves both heroes and monsters (a monster simply has fixed
 * dmg_lo/hi, a hero scales weapon damage by dmg_mod). */

typedef enum { SK_MELEE, SK_RANGED, SK_MOVE, SK_HEAL, SK_SUPPORT, SK_TELEPORT } DDSkillType;

enum {
    SKF_CRIT_VALID      = 1 << 0,
    SKF_IGNORE_PROT     = 1 << 1,
    SKF_IGNORE_STEALTH  = 1 << 2,
    SKF_IGNORE_GUARD    = 1 << 3,
    SKF_SELF_TARGET_OK  = 1 << 4,
    SKF_CANNOT_MISS     = 1 << 5,
    SKF_CONTINUE_TURN   = 1 << 6,
    SKF_NO_STALL_RESET  = 1 << 7, /* is_stall_invalidating false: doesn't count against stalling */
};

typedef struct {
    const char *id;
    const char *name;
    const char *anim;         /* sprite animation played by the performer */
    uint8_t type;             /* DDSkillType */
    uint8_t level;            /* hero skill rank 0..4; 0 for monsters */
    uint8_t flags;            /* SKF_* */
    int8_t per_battle_limit;  /* -1 = unlimited */
    float atk;                /* accuracy (hero: added to weapon/class accuracy) */
    float dmg_mod;            /* hero: multiplier on weapon damage (-0.4 = -40%) */
    float crit;               /* added crit chance */
    uint8_t dmg_lo, dmg_hi;   /* monster flat damage */
    uint8_t heal_lo, heal_hi;
    int8_t move_back, move_fwd; /* self move after/for move skills */
    DDRanks launch, target;
    uint8_t extra_targets;    /* monster random extra targets */
    float extra_targets_chance;
    uint16_t eff_first;       /* into dd_skill_effects[] */
    uint8_t eff_count;
} DDSkill;

/* ---- hero / monster classes ----------------------------------------------
 * Hero stats are per upgrade LEVEL (index 0..4 = weapon/armour tier). */

typedef struct { float atk, crit; uint8_t dmg_lo, dmg_hi; int8_t spd; } DDWeapon;
typedef struct { float def, prot; uint8_t hp; int8_t spd; } DDArmour;

typedef struct {
    const char *id;
    const char *name;
    float res[RES_COUNT];
    DDWeapon weapon[5];
    DDArmour armour[5];
    uint16_t skill_first;     /* combat skills, 5 levels each, grouped by skill id */
    uint8_t skill_count;      /* number of distinct combat skills */
    int16_t move_skill;
    int16_t riposte_skill;    /* -1 if none */
    uint16_t crit_eff_first;  /* effects applied to self on a crit */
    uint8_t crit_eff_count;
    uint16_t dd_buff_first;   /* death's door debuffs (buff indices in dd_buff_list) */
    uint8_t dd_buff_count;
    uint16_t dd_recovery_first; /* applied when leaving death's door */
    uint8_t dd_recovery_count;
    uint8_t tags;             /* HT_* */
    const char *sprite;       /* asset folder name */
    uint16_t ha_recovery_first; /* recovery_heart_attack_buffs: leaving death's door after a heart attack */
    uint8_t ha_recovery_count;
} DDHeroClass;

enum { HT_RELIGIOUS = 1 << 0, HT_HEAVY = 1 << 1, HT_OUTSIDER = 1 << 2 };

enum {
    MF_CAN_SURPRISE = 1 << 0,
    MF_CAN_BE_SURPRISED = 1 << 1,
    MF_PREFERS_FRONT = 1 << 2,
    MF_STALL_IGNORED = 1 << 3, /* doesn't count for the stall penalty (e.g. corpses) */
    MF_CANNOT_BE_HIT = 1 << 4,
    MF_NO_CORPSE_ON_CRIT = 1 << 5, /* death_class .is_valid_on_crit False */
    MF_ALWAYS_SURPRISE = 1 << 6,   /* battle_modifier .always_surprise (Shambler) */
};

typedef struct {
    const char *id;           /* class with tier, e.g. brigand_cutthroat_A */
    const char *name;
    uint8_t size;             /* ranks occupied */
    uint8_t types;            /* MT_* bitmask */
    uint8_t flags;            /* MF_* */
    uint8_t turns_per_round;
    int16_t hp;
    float def, prot;
    int8_t spd;
    float res[RES_COUNT];
    uint16_t skill_first;
    uint8_t skill_count;
    int16_t riposte_skill;
    int16_t brain;
    int16_t corpse;           /* monster class left on death (-1 = none) */
    int16_t loot_table;       /* dd_loot_ids index, -1 none */
    uint8_t loot_count;       /* draws on death */
    const char *sprite;       /* asset folder name (shared by tiers) = base class id */
    /* boss mechanics (0 / -1 / NULL when unused) */
    const char *life_link;    /* dies when the last monster of this base class dies */
    int16_t captor_full;      /* captor_empty: class it turns into when holding a hero */
    int16_t captor_empty;     /* captor_full: class it turns back into */
    const char *captor_for;   /* captor_empty: base class whose capture effects use it */
    float captor_damage;      /* captor_full: prisoner damage per round (fraction of max HP) */
    uint8_t captor_release_dd;/* release the prisoner at death's door */
    uint8_t shared_health;    /* shared HP pool id (0 none) */
    uint8_t shift_count, shift_every;
    int16_t shift_class[3];
    float shift_chance[3];
    uint8_t shift_ranks[3];
    const char *companion;    /* base class that, while alive, buffs this monster */
    uint16_t companion_buff_first; uint8_t companion_buff_count;
    float companion_heal;     /* fraction of max HP healed per round while the companion lives */
    uint8_t control_stress;   /* stress per controlled turn (Siren) */
    uint16_t spawn_eff_first; uint8_t spawn_eff_count;
    uint8_t is_boss;
    /* the Ancestor's phases (Darkest Dungeon 4) */
    uint8_t flags2;           /* MF2_* */
    uint16_t death_eff_first; uint8_t death_eff_count; /* death_class .change_class_effects */
    float carry_hp;           /* death_class .carry_over_hp_min_percent: new form's minimum HP fraction */
    const char *death_dmg_base; /* death_damage: base class hurt when this monster dies */
    int8_t death_dmg;
    uint16_t react_eff_first; uint8_t react_eff_count; /* skill_reaction .was_hit_performer_effects */
    int8_t prefskill;         /* personality .prefskill: skill slot of preferred_skill desires, -1 none */
} DDMonsterClass;
enum {
    MF2_NO_DIRECT_DAMAGE = 1 << 0, /* battle_modifier .can_be_damaged_directly false */
    MF2_TRANSFORM = 1 << 1,        /* death_class .can_die_from_damage false: turns into the death class */
    MF2_NO_STALL = 1 << 2,         /* battle_modifier .disable_stall_penalty */
    MF2_STALL_FAST = 1 << 3,       /* .accelerate_stall_penalty */
    MF2_STALL_SKIP = 1 << 4,       /* .does_count_towards_stall_penalty false */
    MF2_NEVER_MISSED = 1 << 5,     /* .can_be_missed false: attacks on it always hit */
    MF2_NO_CRIT_RELIEF = 1 << 6,   /* .can_relieve_stress_from_crit false */
    MF2_NO_KILL_RELIEF = 1 << 7,   /* .can_relieve_stress_from_killing_blow false */
};

/* ---- monster AI ------------------------------------------------------------
 * Monsters have no scripts. A BRAIN lists weighted "desires": which skill to
 * prefer under which conditions (turn number, allies alive, hero marked...)
 * and which hero to target (low HP, stunned, high stress...). ai.c rolls them. */

typedef enum {
    TD_RANDOM, TD_MARKED, TD_HEALTH_LOW, TD_STUNNED, TD_RANK, TD_STRESS,
    TD_RESISTANCE_LOW, TD_DOT, TD_GUARD, TD_UNSUPPORTED
} DDTargetDesireType;

/* monster_brains skill_selection_desires types (AI::MonsterSkillSelectionDesire::Functions) */
enum { SD_SPECIFIC, SD_TURN, SD_STUN, SD_ALLY_ALIVE, SD_ALLY_DEAD, SD_HEAL, SD_KEY_STATUS, SD_RANDOM, SD_FILL_CAPTOR,
       SD_PREFERRED /* the monster's personality .prefskill */ };
/* IsDesireValid count ranges (inclusive) */
enum { DC_MONSTERS, DC_MONSTERS_SIZE, DC_HEROES, DC_MARKED, DC_NON_MARKED, DC_NON_STUNNED, DC_NON_DEATHS_DOOR,
       DC_VIRTUED, DC_GUARDED, DC_CONTROLLED, DC_COUNT };
typedef struct {
    const char *skill; /* combat_skill_id (brains are shared by a monster's A/B/C variants); NULL = random */
    float chance;
    uint8_t type;      /* SD_* */
    uint8_t turn;      /* SD_TURN: performing_turn (exact); others: performing_turn_min */
    uint8_t first_only;/* first_initiative_only: the monster's first turn of the round */
    uint8_t key_status;/* SD_KEY_STATUS: KS_* a hero must have */
    const char *ally;  /* ally_base_class_id (monster id prefix), NULL none */
    float hp_ratio;    /* SD_HEAL: an ally at or below this HP ratio */
    uint8_t lo[DC_COUNT], hi[DC_COUNT];
} DDSkillDesire;

typedef struct {
    uint8_t type;      /* DDTargetDesireType */
    uint8_t param;     /* rank mask for TD_RANK, DDResist for TD_RESISTANCE_LOW */
    const char *skill; /* restrict to this skill id, NULL = any */
    float chance;
} DDTargetDesire;

/* Extra actions outside the normal turn order (AI::MonsterBonusInitiativeDesire). */
typedef enum { BI_GUARANTEED, BI_LAST_SKILL, BI_ALLY_DAMAGED, BI_ALLY_COUNT } DDBonusType;
enum { BW_ROUND_START = 1, BW_POST_TURN = 2, BW_ROUND_FINISH = 4 };

typedef struct {
    uint8_t type, when;       /* DDBonusType, BW_* */
    const char *skill;        /* combat_skill_id_override, NULL = AI chooses */
    const char *trigger;      /* BI_LAST_SKILL: skill id that triggers it */
    const char *ally;         /* BI_ALLY_*: ally base class */
    int8_t ally_min, ally_max, mon_min, mon_max, size_limit;
    uint8_t ignore_if_stun;
} DDBonusDesire;

typedef struct {
    const char *id;
    uint16_t skill_first; uint8_t skill_count;   /* into dd_skill_desires[] */
    uint16_t target_first; uint8_t target_count; /* into dd_target_desires[] */
    uint16_t bonus_first; uint8_t bonus_count;   /* into dd_bonus_desires[] */
} DDBrain;

/* ---- dungeons --------------------------------------------------------------
 * An ENCOUNTER is a fixed group of up to 4 monsters, tagged with where it can
 * appear (dungeon, level, hall/room/boss...) and a spawn weight. */

typedef enum { DG_RUINS, DG_WEALD, DG_WARRENS, DG_COVE, DG_DARKEST, DG_TOWN, DG_COUNT } DDDungeon; /* DG_TOWN: plot only */
#define DG_REGULAR DG_DARKEST /* dungeons that get generated quests; the Darkest Dungeon is plot-only */
/* ENC_COND_*: <dungeon>.conditional.<lvl>.mash hall packs (Shambler, Collector), rolled per hall
 * square while the condition holds; ENC_ALTAR: summon_mash_shambler (torch on the Shambler's altar). */
typedef enum { ENC_HALL, ENC_ROOM, ENC_BOSS, ENC_NAMED, ENC_COND_DARK, ENC_COND_INV, ENC_ALTAR, ENC_STALL } DDEncounterKind;

typedef struct {
    uint8_t dungeon, level, kind, count;
    float chance;
    int16_t monster[4];
    float cond;        /* ENC_COND_DARK: max light; ENC_COND_INV: min inventory fill (0..1) */
} DDEncounter;

/* ---- afflictions & virtues (shared/trait/trait_library.json) --------------- */

typedef enum {
    ACT_NOTHING, ACT_BARK_STRESS, ACT_CHANGE_POS, ACT_IGNORE_COMMAND, ACT_RANDOM_COMMAND,
    ACT_RETREAT, ACT_ATTACK_FRIENDLY, ACT_ATTACK_SELF, ACT_MARK_SELF, ACT_STRESS_HEAL_SELF,
    ACT_STRESS_HEAL_PARTY, ACT_BUFF_RANDOM_ALLY, ACT_BUFF_PARTY, ACT_HEAL_SELF, ACT_COUNT
} DDActOutType;

typedef struct {
    uint8_t type;      /* DDActOutType */
    int8_t hero_cls;   /* only for this hero class, -1 = any */
    int16_t effect;    /* dd_effects index or -1 */
    float weight;
    float value;       /* number_value (e.g. damage fraction, move ranks) */
} DDActOut;

enum { TRAIT_AFFLICTION, TRAIT_VIRTUE };

typedef struct {
    const char *id, *name, *desc;
    uint8_t type;      /* TRAIT_* */
    uint16_t buff_first; uint8_t buff_count;   /* into dd_buff_list */
    uint16_t act_first; uint8_t act_count;     /* into dd_actouts */
    float block_heal, block_move, block_buff;  /* reaction chances (afflictions) */
} DDTrait;

/* ---- curios (curios/curio_type_library.csv) -------------------------------- */

typedef enum { CR_NOTHING, CR_LOOT, CR_QUIRK, CR_EFFECT, CR_PURGE, CR_SCOUTING, CR_TELEPORT, CR_DISEASE, CR_SUMMON } DDCurioResultType;

typedef struct {
    uint8_t type;      /* DDCurioResultType */
    uint8_t item;      /* item interactions: ItemId (dungeon.h), 255 = none */
    float weight;      /* chance weight among the curio's results */
    int16_t effect;    /* CR_EFFECT: effect index; CR_LOOT: loot table id (dd_loot_ids) */
    int8_t draws;      /* CR_LOOT: number of loot draws; CR_QUIRK: +1 positive / -1 negative */
} DDCurioResult;

typedef struct {
    const char *id, *name, *sprite;
    uint8_t regions;   /* bit per DDDungeon */
    uint8_t good;      /* 0 bad, 1 good, 2 mixed */
    uint16_t res_first; uint8_t res_count;     /* investigate results */
    uint16_t item_first; uint8_t item_count;   /* item interactions (item set) */
} DDCurio;

/* ---- camping (raid/camping/default.camping_skills.json) -------------------- */

typedef enum {
    CE_STRESS_HEAL, CE_STRESS_DAMAGE, CE_HEAL_PERCENT, CE_DAMAGE_PERCENT, CE_BUFF, CE_CURE,
    CE_REDUCE_AMBUSH, CE_REMOVE_DD_RECOVERY, CE_LOOT, CE_TORCH, CE_OTHER
} DDCampEffectType;
enum { SEL_SELF, SEL_INDIVIDUAL, SEL_PARTY, SEL_PARTY_OTHER };

typedef struct {
    uint8_t type, selection;  /* DDCampEffectType, SEL_* */
    int16_t buff;             /* CE_BUFF: buff index */
    float amount, chance;
} DDCampEffect;

typedef struct {
    const char *id, *name;
    uint8_t cost, use_limit;
    uint16_t classes;         /* bit per dd_heroes index */
    uint16_t eff_first; uint8_t eff_count;
    uint8_t needs_target;     /* any effect picks one hero */
} DDCampSkill;

/* ---- map generation (scripts/map_generator.darkest) -----------------------
 * Ranges stored as [min,max] pairs (e.g. hall_battle) are rolled per map. */

enum { QT_EXPLORE, QT_CLEANSE, QT_BOSS, QT_GATHER, QT_ACTIVATE, QT_INVENTORY_ACTIVATE, QT_COUNT };

typedef struct {
    uint8_t length, quest, dungeon;      /* 1 short / 2 medium / 3 long, QT_*, DDDungeon */
    uint8_t rooms, corridors, grid_w, grid_h, spacing, goal_rooms, min_final_distance;
    float connectivity;
    uint8_t hall_battle[2], hall_trap[2], hall_obstacle[2], hall_curio[2], hall_hunger[2];
    uint8_t room_battles[2], guarded_curio[2], guarded_treasure[2], secret_rooms[2];
} DDMapParams;

/* ---- quest board (campaign/quest/quest.generation.json) --------------------- */

typedef struct {
    uint8_t dungeon, row, quest, length; /* row = progression index 0..7 */
    float chance;
} DDQuestOption;

/* ---- town upgrades (upgrades/heroes/<class>.upgrades.json) ---------------------- */

typedef struct {
    uint16_t weapon_cost[4];  uint8_t weapon_level[4];   /* to reach weapon tier 1..4 */
    uint16_t armour_cost[4];  uint8_t armour_level[4];
    uint16_t skill_cost[8][5]; uint8_t skill_level[8][5]; /* [skill][code 0..4]: code 0 = learn */
} DDUpgrades;

/* ---- generated tables (extracted/gen/dd_data.c) -------------------------
 * These `extern` declarations are the whole public data API of the game. Rows
 * are read-only; runtime state lives in Hero/Actor/Dungeon structs elsewhere. */

extern const DDBuff *dd_buffs;           extern DD_C int dd_buff_count;
extern const int16_t *dd_buff_list;
extern const DDEffOp *dd_eff_ops;
extern const DDEffect *dd_effects;       extern DD_C int dd_effect_count;
extern const uint16_t *dd_skill_effects;
extern const DDSkill *dd_skills;         extern DD_C int dd_skill_count;
extern const int8_t *dd_hero_guaranteed; /* class skill every recruit starts with (generation_guaranteed), -1 none */
extern const DDHeroClass *dd_heroes;     extern DD_C int dd_hero_count;
extern const DDMonsterClass *dd_monsters; extern DD_C int dd_monster_count;
extern const DDSkillDesire *dd_skill_desires;
extern const DDTargetDesire *dd_target_desires;
extern const DDBonusDesire *dd_bonus_desires;
extern const DDBrain *dd_brains;         extern DD_C int dd_brain_count;
extern const DDEncounter *dd_encounters; extern DD_C int dd_encounter_count;
extern const int16_t *dd_enc_tutorial; /* Old Road fights: [0] hall (tutorial_1), [1] room (tutorial_2) */
extern const DDTrait *dd_traits;         extern DD_C int dd_trait_count;
extern const DDActOut *dd_actouts;
extern const DDCurioResult *dd_curio_results;
extern const DDCurio *dd_curios;         extern DD_C int dd_curio_count;
extern const DDCampEffect *dd_camp_effects;
extern const DDCampSkill *dd_camp_skills; extern DD_C int dd_camp_skill_count;
extern const DDMapParams *dd_map_params; extern DD_C int dd_map_param_count;
extern const DDQuestOption *dd_quest_options; extern DD_C int dd_quest_option_count;
extern const uint8_t *dd_quests_per_visit;
extern const uint8_t *dd_dungeon_unlock;   /* quests finished needed */
extern const DDUpgrades *dd_upgrades;    /* [GM_* * hero count + class]: per game mode */
extern const char *const *dd_dungeon_names;

/* ---- Darkest Dungeon plot quests (maps/DD_map1..4.dm, quest.plot_quests.json) ---- */
#define DD_PLOT_QUESTS 4 /* the Darkest Dungeon chain */
#define DD_PLOT_ALL 10    /* + 4 Wolves at the Door (town invasion), 5 Shrieker's Prize (town events),
                          * 6 the first Ruins quest after the Old Road (plot_tutorial_crypts) */
#define PQ_INVASION 4
#define PQ_CROW 5
#define PQ_TUTORIAL 6
#define PQ_PERCH 7       /* 7..9: the Shrieker's Perch (plot_trinket_retention_0..2), levels 1/3/5 */
#define HOARD_RETURN 8   /* lost trinkets returned per Perch completed (wiki Shrieker) */
#define DD_PLOT_SEGS 10
#define CURIO_QUEST (-2)      /* curio slot "custom_map_quest_curio": the quest's goal to activate */
typedef struct { int8_t gx, gy; uint8_t spot; int16_t enc, curio; } DDPlotRoom; /* gx/gy in map tiles */
typedef struct { uint8_t a, b, nseg; uint8_t seg[DD_PLOT_SEGS]; int16_t ref[DD_PLOT_SEGS]; } DDPlotCorr;
typedef struct {
    uint8_t quest, length, can_retreat, goals; /* QT_*, length, retreat allowed, goal curios to activate */
    int16_t boss;                               /* boss encounter in the final room, -1 none */
    int16_t room_first, nrooms, corr_first, ncorr;
    int32_t gold;
    int16_t crests, xp;                         /* completion_reward */
    const char *name, *desc, *tip;              /* town_quest_name/description, str_plot_..._tip */
    uint8_t dungeon, level;                     /* DG_*, quest difficulty */
    int16_t trinket;                            /* completion_reward trinket (dd_trinkets), -1 none */
    int8_t trinket_rarity;                      /* additional_trinket_completion_rewards rarity, -1 none */
    uint8_t ignore_upgrades;                    /* building upgrades lost if the quest is ignored */
    uint8_t nwin; int16_t win_quirk[7]; uint8_t win_w[7];    /* party_quirks_to_apply_on_completion */
    uint8_t nlose; int16_t lose_quirk[7]; uint8_t lose_w[7]; /* ... on failure */
} DDPlotQuest;
extern const DDPlotRoom *dd_plot_rooms;
extern const DDPlotCorr *dd_plot_corrs;
extern const DDPlotQuest *dd_plot_quests;
extern const char *const *dd_coach_idle; extern DD_C int dd_coach_idle_count; /* str_stagecoach_idle (recruit barks) */
extern const char *const *dd_epilog; extern DD_C int dd_epilog_count; /* str_vo_epilog_N */
extern const char *const *dd_dungeon_dirs;

/* ---- loot (loot/<name>.loot.json) ---------------------------------------------- */

typedef enum { LK_NOTHING, LK_TABLE, LK_GOLD, LK_GEM, LK_HEIRLOOM, LK_ITEM, LK_TRINKET, LK_JOURNAL } DDLootKind;
typedef struct { uint8_t kind; uint16_t chances; int16_t ref; int16_t amount; } DDLootEntry; /* ref: table id, gem, HEIR_*, ItemId */
typedef struct { int16_t id; uint8_t difficulty; int8_t dungeon; uint16_t first; uint8_t count; } DDLootTable; /* 0 / -1 = any */
typedef struct { const char *id, *name; int value; } DDGem;
typedef struct { uint8_t darkness; float chance; uint8_t ncodes; int16_t code; } DDDarknessLoot; /* torch <= darkness row */
extern const DDLootTable *dd_loot_tables; extern DD_C int dd_loot_table_count;
extern const DDLootEntry *dd_loot_entries;
extern const DDGem *dd_gems; extern DD_C int dd_gem_count;
extern const DDDarknessLoot *dd_darkness_battle; extern DD_C int dd_darkness_battle_count;
extern const DDDarknessLoot *dd_darkness_curio; extern DD_C int dd_darkness_curio_count;
extern const char *const *dd_loot_ids; extern DD_C int dd_loot_id_count;

/* ---- building upgrades (upgrades/building, campaign/town/buildings) ------- */

enum { TV_DISCOUNT, TV_AMOUNT, TV_COST, TV_SLOTS, TV_HEAL,
       TV_COST_NEGATIVE, TV_COST_PERMANENT, TV_CHANCE, TV_SLOTS_DISEASE }; /* sanitarium extras */
typedef struct { const char *id; uint8_t levels; uint16_t cost[8][5]; } DDTree; /* cost: bust, portrait, deed, crest, gold */
typedef struct { uint8_t tree, level, key; float value; uint8_t mode; } DDTreeValue; /* level 0 = base value; mode GM_* */
extern const DDTree *dd_trees; extern DD_C int dd_tree_count;

/* ---- town events (campaign/town_events) ------------------------------------ */
enum { TA_BAR, TA_GAMBLING, TA_BROTHEL, TA_MEDITATION, TA_PRAYER, TA_FLAGELLATION, TA_TREATMENT, TA_DISEASE, TA_COUNT };
enum { TE_FREE_ACTIVITY, TE_ACTIVITY_COST, TE_ACTIVITY_LOCK, TE_PROVISION_COST, TE_BONUS_RECRUIT, TE_IDLE_LEVEL,
       TE_DEAD_RECRUIT, TE_UPGRADE_FREE, TE_UPGRADE_DISCOUNT, TE_EMBARK_BUFF, TE_IDLE_BUFF, TE_ACTIVITY_BUFF,
       TE_NO_LEVEL_LIMIT, TE_PLOT_QUEST /* arg: PQ_* put on the quest board */,
       TE_HOARD /* trinket_retention_add_from_storage: the Shrieker steals num trinkets from the estate */ };
enum { TB_DAMAGE, TB_RESOLVE_XP, TB_RESOLVE_CHECK, TB_STRESS_HEAL }; /* buff ops: arg & 15 */
typedef struct {
    uint8_t type;  /* TE_* */
    uint8_t arg;   /* TA_* activity; class; 0 provision / 1 supply; tag 0 armour 1 weapon 2 building 3 trinket;
                    * buffs: TB_* | (dungeon + 1 or activity + 1, 0 = any) << 4 */
    float num;     /* cost change, count, buff amount */
} DDTownEventOp;
typedef struct {
    const char *id, *title, *desc;
    float chance, per_miss;   /* base_chance, per_not_rolled_additional_chance */
    uint8_t cooldown, min_week, dead_heroes;
    uint16_t op_first; uint8_t op_count;
    uint8_t req_level, req_count;  /* hero_level_counts: at least count heroes of resolve level >= level */
    uint8_t req_storage;           /* trinket_storage_count: trinkets in the estate chest */
} DDTownEvent;
typedef struct { uint8_t dungeon, quest_type, event; } DDTownEventGuarantee;
extern const DDTownEvent *dd_town_events; extern DD_C int dd_town_event_count;
extern const DDTownEventOp *dd_town_event_ops;
extern const DDTownEventGuarantee *dd_town_event_guarantees; extern DD_C int dd_town_event_guarantee_count;
extern const float *dd_town_event_chance;
/* ---- game modes (modes/<mode>/: overrides of rules.json, progression, town event settings) */
typedef enum { GM_DARKEST, GM_RADIANT, GM_STYGIAN, GM_COUNT } DDGameMode;
enum { DK_STRESS_CHANCE, DK_STRESS_DMG, DK_M_ACC, DK_M_DMG, DK_M_CRIT, DK_H_CRIT, DK_H_DEF, DK_SCOUT, DK_M_SURP, DK_H_SURP, DK_COUNT };
typedef struct {
    float dark_lower[5];          /* darkness bands, brightest first: torch > lower (in %) */
    float dark[5][DK_COUNT];      /* range_table values, in % */
    float crit_comp_chance, dd_comp_chance, dd_comp_chance_rep;
    float pass_stress, dd_self, dd_self_rep, dd_comp, dd_comp_rep;
    float hall[4];                /* hallway_stress: fwd dmg, fwd chance, back dmg, back chance */
    uint8_t light_new, light_old; /* tile_light_loss dim/dark, clear */
    uint8_t level_xp[8];          /* roster.variables resolve_level_thresholds (hero XP) */
    float te_chance[4];           /* town_events.settings "normal" */
    uint16_t week_limit;          /* new_game_plus_week_limit (game over, Stygian only) */
    uint8_t death_limit;          /* new_game_plus_hero_death_limit */
    uint8_t idle_heal;            /* town_visit_town_progression idle_hero_stress_heal */
    float act_mult[7];            /* estate hero_activity_cost_multiplier_table, by resolve level */
    uint8_t max_level[7];         /* quest.restriction: highest resolve level allowed, by quest level */
} DDRules;
extern const DDRules *dd_rules;
extern const DDRules *g_rules;
extern const char *const *dd_game_over_reason; /* weeks, dead heroes (localization) */
/* localization strings for menus (export_game_data.py UI_STRINGS, same order) */
typedef enum {
    US_SELECT_CAMPAIGN, US_MODE_RADIANT, US_MODE_DARKEST, US_MODE_STYGIAN,
    US_MODE_RADIANT_TIP, US_MODE_DARKEST_TIP, US_MODE_STYGIAN_TIP,
    US_OLD_ROAD, US_OLD_ROAD_TIP, US_OLD_ROAD_GOAL,
    US_TIP_RUINS, US_TIP_WEALD, US_TIP_WARRENS, US_TIP_COVE, US_TIP_DARKEST,
    US_QUEST_COMPLETE, US_QUEST_REWARDS, US_COLLECTED_TREASURE, US_COLLECTED_HEIRLOOMS, US_RETURN_TO_TOWN,
    US_HEROES, US_RESOLVE_XP, US_NEW_QUIRKS, US_RESULT_DONE, US_RESULT_DEFEAT, US_RESULT_ESCAPE,
    US_ACTIVITY_LOG, US_CARETAKER_GOALS, US_QUEST_GOALS, US_GOAL_TUTORIAL, US_GOAL_INVASION,
    US_GOAL_DD1, US_GOAL_BOSS = US_GOAL_DD1 + 4, /* + boss * 3 + level index (BOSS_KEYS order) */
    US_RESOLVE_TITLE = US_GOAL_BOSS + 24, /* + resolve level 0..6: Seeker .. Legend */
    US_COACH_FULL = US_RESOLVE_TITLE + 7, /* str_stagecoach_roster_full_rejection */
    US_HAS_QUIRK,  /* camping_skill_requirement_has_quirk "has quirk: %s" */
    US_TRAP,       /* str_its_a_trap "TRAP!" */
    US_TRAP_DISARM,/* resistance_trap_disarm_format "Trap Disarm: %.0f%%" */
    US_COUNT
} UiStr;
extern const char *const *dd_ui_str;    /* the campaign's mode (formulas.c) */
#define JOURNAL_PAGES 22 /* localization journal_page_title/text_0..21 */
extern const char *const *dd_journal_title; extern const char *const *dd_journal_text;
/* campaign/heirloom_exchange: from_amount of HEIR_* from -> to_amount of to */
typedef struct { uint8_t from, from_n, to, to_n; } DDHeirExchange;
extern const DDHeirExchange *dd_heir_exchange; extern DD_C int dd_heir_exchange_count;

/* ---- traps (props/trap_definitions.json), one type per dungeon; rows by level 1/3/5/6 ---- */
typedef struct {
    const char *id;      /* NULL: no traps; sprite trap_<id> */
    float hp_loss[4];    /* fraction of max HP lost when set off */
    int16_t fail[4][2];  /* fail_effects (dd_effects, -1 none) */
    int16_t success;     /* success_effects on a disarm (Heal Stress TrapD: 8 stress) */
} DDTrap;
extern const DDTrap *dd_traps;
/* hall obstacles (obstacle_definitions + the prop type "obstacle"): cleared without a shovel each hero
 * loses hp_loss of max HP and takes fail (Stress 2: 15), and the torch drops by torch */
typedef struct { const char *id; float hp_loss; int torch; int16_t fail; } DDObstacle; /* sprite obstacle_<id> */
extern const DDObstacle *dd_obstacles;
extern const int16_t *dd_item_effect; /* supply item -> dd_effects (bandage, antivenom, herbs, holy water, laudanum), -1 none */
extern const float *dd_trap_base;       /* difficulty_trap_base by dungeon level */
extern DD_C float dd_trap_scout_bonus;   /* trap_scout_disarm_bonus */
/* rules.json corridor_return_content: chance a revisited hall gets a new battle / hunger / trap,
 * by torch band (0, 1-25, 26-50, 51-75, 76-100) */
extern const float (*dd_corridor_return)[5];

/* ---- tavern/abbey activities (buildings/<b>/<b>.building.json), Hero.busy 1..6 order ---- */
enum { AS_LOCK, AS_MISSING, AS_QUIRK, AS_BUFF, AS_GOLD_ADD, AS_GOLD_LOSE, AS_TRINKET_ADD, AS_TRINKET_LOSE };
typedef struct {
    uint8_t type;      /* AS_* */
    float weight;
    int16_t a, b;      /* weeks missing / quirk / buff pair / gold / trinket rarity */
    const char *story, *desc; /* activity log text; $B building $H hero $N number $Q quirk $T trinket $D desc $A affliction */
} DDActSide;
typedef struct {
    float chance;                     /* side_effects chance per stay */
    int16_t first, count;             /* dd_act_sides rows */
    int16_t no_quirk[8]; uint8_t nno; /* not_have_quirks */
    const char *relief, *cured, *found, *committed, *name; /* town_activity_name_* */
} DDActivity;
extern const DDActSide *dd_act_sides; extern const DDActivity *dd_activities;

/* ---- quirks and diseases (shared/quirk/quirk_library.json) ---------------- */

typedef struct {
    const char *id, *name;
    uint8_t positive, disease;
    float weight;             /* random_chance */
    uint16_t buff_first; uint8_t buff_count; /* dd_buff_list span */
    int16_t incompatible[2];
    const char *desc;
} DDQuirk;
extern const DDQuirk *dd_quirks; extern DD_C int dd_quirk_count;

/* ---- trinkets (trinkets/base.entries.trinkets.json) ------------------------ */

typedef enum {
    TR_VERY_COMMON, TR_COMMON, TR_UNCOMMON, TR_RARE, TR_VERY_RARE, TR_ANCESTRAL, TR_TROPHY, TR_CROW,
    TR_ANCESTRAL_SHAMBLER, TR_MADMAN, TR_COLLECTOR, TR_COURTIER, TR_DARKEST_DUNGEON, TR_COUNT
} DDTrinketRarity;

typedef struct {
    const char *id, *name;
    uint8_t rarity;    /* DDTrinketRarity */
    int8_t cls;        /* dd_heroes index the trinket is restricted to, -1 any */
    uint8_t limit;     /* 1 = one copy per hero */
    int8_t dungeon;    /* origin_dungeon (drops only there), -1 any */
    int32_t price;     /* Nomad Wagon price before discounts */
    int16_t buff_first, buff_count; /* dd_buff_list span */
} DDTrinket;
extern const DDTrinket *dd_trinkets; extern DD_C int dd_trinket_count;
extern const DDTreeValue *dd_tree_values; extern DD_C int dd_tree_value_count;

#endif
