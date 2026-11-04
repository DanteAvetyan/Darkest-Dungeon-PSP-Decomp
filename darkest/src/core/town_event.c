/*
 * town_event.c - weekly random hamlet events. State is in g_camp (event,
 * event_last, event_missed...). An event is a list of OPs (see
 * DDTownEventOp): cost changes, extra recruits, buffs, posting a plot quest.
 * The other town_event_* functions are small adapters that scene_town calls to
 * ask "what does this price / relief become under the active event?".
 */
#include "town_event.h"

#include <string.h>

#include "dd_types.h"

/* The active event or NULL. */
const DDTownEvent *town_event(void) {
    int e = g_camp.event;
    return e >= 0 && e < dd_town_event_count ? &dd_town_events[e] : NULL;
}

/* First op of the active event of a given type (and arg, if arg >= 0). */
const DDTownEventOp *town_event_op(int type, int arg) {
    const DDTownEvent *e = town_event();
    for (int i = 0; e && i < e->op_count; i++) {
        const DDTownEventOp *o = &dd_town_event_ops[e->op_first + i];
        if (o->type == type && (arg < 0 || o->arg == arg)) return o;
    }
    return NULL;
}

/* Deaths so far (some events need enough deaths to trigger). */
static int dead_count(void) { return g_camp.deaths; }

/* Add a scripted plot quest to the quest board. */
void town_event_post_quest(int pq_index) {
    if (g_camp.nboard >= BOARD_MAX || pq_index < 0 || pq_index >= DD_PLOT_ALL) return;
    const DDPlotQuest *pq = &dd_plot_quests[pq_index];
    QuestOffer *q = &g_camp.board[g_camp.nboard++];
    memset(q, 0, sizeof(*q));
    q->dungeon = pq->dungeon;
    q->level = pq->level;
    q->length = pq->length;
    q->quest = pq->quest <= QT_BOSS ? pq->quest : QT_BOSS;
    q->heir = HEIR_CREST;
    q->plot = (uint8_t)(PLOT_DD + pq_index);
}

/* Count living heroes at or above a resolve level. */
static int heroes_at_level(int lvl) {
    int n = 0;
    for (int i = 0; i < g_camp.nroster; i++) n += g_camp.roster[i].alive && g_camp.roster[i].level >= lvl;
    return n;
}

/* An event that posts a plot quest can't fire while it can't be posted. */
static int plot_ok(const DDTownEvent *e) {
    for (int k = 0; k < e->op_count; k++) {
        const DDTownEventOp *o = &dd_town_event_ops[e->op_first + k];
        if (o->type != TE_PLOT_QUEST) continue;
        if (g_camp.nboard >= BOARD_MAX || (o->arg == PQ_INVASION && (g_camp.plot_done & 1))) return 0;
        for (int i = 0; i < g_camp.nboard; i++)
            if (g_camp.board[i].plot == PLOT_DD + o->arg) return 0;
    }
    return 1;
}

/* settings "normal": chance by town visits since the last event; then a weighted pick of the
 * eligible events, base_chance + per_not_rolled_additional_chance per visit it was eligible but not picked. */
/* Pick this week's event. Two stages: (1) roll whether ANY event happens - the
 * chance grows with each quiet visit; (2) among eligible events (cooldowns,
 * min week, requirements) choose by weight, where an event ignored before
 * gains weight each visit. A quest can also FORCE the next event. */
void town_event_roll(Rng *r) {
    g_camp.event = -1;
    g_camp.event_free = 0;
    int pick = g_camp.event_force;
    g_camp.event_force = -1;
    float w[TOWN_EVENTS_MAX], total = 0;
    int n = dd_town_event_count < TOWN_EVENTS_MAX ? dd_town_event_count : TOWN_EVENTS_MAX;
    for (int i = 0; i < n; i++) {
        const DDTownEvent *e = &dd_town_events[i];
        int ok = (e->cooldown < 255 || !g_camp.event_last[i]) && /* is_unique: once per campaign */
                 g_camp.week >= e->min_week && dead_count() >= e->dead_heroes && plot_ok(e) &&
                 (!e->req_count || heroes_at_level(e->req_level) >= e->req_count) &&
                 g_camp.nstash >= e->req_storage &&
                 (!g_camp.event_last[i] || g_camp.week - (g_camp.event_last[i] - 1) > e->cooldown);
        w[i] = ok ? e->chance + e->per_miss * g_camp.event_missed[i] : 0;
        total += w[i];
    }
    if (pick < 0) {
        int v = g_camp.event_visits < 3 ? g_camp.event_visits : 3;
        if (total <= 0 || !rng_chance(r, g_rules->te_chance[v])) {
            g_camp.event_visits++;
            return;
        }
        float x = rng_float(r) * total;
        for (int i = 0; i < n && pick < 0; i++) {
            if (w[i] > 0 && x < w[i]) pick = i;
            x -= w[i];
        }
        if (pick < 0) return;
    }
    for (int i = 0; i < n; i++)
        if (w[i] > 0 && i != pick && g_camp.event_missed[i] < 255) g_camp.event_missed[i]++;
    g_camp.event = (int8_t)pick;
    g_camp.event_visits = 0;
    g_camp.event_missed[pick] = 0;
    g_camp.event_last[pick] = (int16_t)(g_camp.week + 1);
    g_camp.event_seen = 0;
    /* recruit effects happen at once on the stage coach */
    int slot = 0, nrec = (int)tree_value("stage_coach.numrecruits", TV_AMOUNT, 2);
    if (nrec > RECRUITS) nrec = RECRUITS;
    const DDTownEvent *e = town_event();
    for (int k = 0; k < e->op_count; k++) {
        const DDTownEventOp *o = &dd_town_event_ops[e->op_first + k];
        if (o->type == TE_PLOT_QUEST) {
            town_event_post_quest(o->arg);
        } else if (o->type == TE_HOARD) { /* A Thief in the Night: random trinkets from the estate chest */
            for (int k = 0; k < (int)o->num && g_camp.nstash > 0; k++) {
                int i = rng_range(r, 0, g_camp.nstash - 1);
                hoard_add(g_camp.stash[i]);
                stash_remove(i);
            }
        } else if (o->type == TE_BONUS_RECRUIT) {
            for (int c = 0; c < (int)o->num && slot < nrec; c++) hero_generate(&g_camp.recruits[slot++], r, o->arg);
        } else if (o->type == TE_DEAD_RECRUIT) { /* the fallen return, at the level they died */
            for (int c = 0; c < (int)o->num && c < g_camp.nfallen && slot < nrec; c++) {
                const Fallen *f = &g_camp.graveyard[g_camp.nfallen - 1 - c];
                Hero *h = &g_camp.recruits[slot++];
                hero_generate(h, r, f->cls);
                memcpy(h->name, f->name, sizeof(h->name));
                h->level = f->level;
                h->xp = g_rules->level_xp[f->level];
                h->hp = (int16_t)hero_max_hp(h);
            }
        }
    }
}

/* Finishing certain quests guarantees a follow-up event. */
void town_event_quest_done(int dungeon, int quest_type) {
    for (int i = 0; i < dd_town_event_guarantee_count; i++) {
        const DDTownEventGuarantee *g = &dd_town_event_guarantees[i];
        if (g->dungeon == dungeon && g->quest_type == quest_type) g_camp.event_force = (int8_t)g->event;
    }
}

int town_event_locked(int activity) { return town_event_op(TE_ACTIVITY_LOCK, activity) != NULL; }

/* Activity cost under the event (e.g. free or discounted). */
int town_event_cost(int activity, int cost) {
    if (town_event_op(TE_FREE_ACTIVITY, activity)) return 0;
    const DDTownEventOp *o = town_event_op(TE_ACTIVITY_COST, activity);
    return o ? (int)(cost * (1 + o->num) + 0.5f) : cost;
}

/* Stress relief a hero gets from an activity, after event buffs. */
int town_event_relief(int activity, int roster_index, int relief) {
    float m = 1;
    const DDTownEventOp *o = town_event_op(TE_ACTIVITY_BUFF, TB_STRESS_HEAL | (activity + 1) << 4);
    if (o) m += o->num;
    /* idle_buff: heroes that sat out the last expedition (idle at the start of the visit) */
    o = town_event_op(TE_IDLE_BUFF, -1);
    int idle = 1;
    for (int r = 0; r < 4; r++) idle &= g_camp.party[r] != roster_index;
    if (o && idle) m += o->num;
    int v = (int)(relief * m + 0.5f);
    return v < 0 ? 0 : v;
}

/* Provision price after the event's discount. */
int town_event_item_price(int item, int price) {
    const DDTownEventOp *o = town_event_op(TE_PROVISION_COST, item != ITEM_FOOD);
    return o ? (int)(price * (1 + o->num) + 0.5f) : price;
}

/* Upgrade price after discount (or 0 if a free upgrade remains). */
int town_event_upgrade_cost(int tag, int cost) {
    if (cost <= 0) return cost;
    const DDTownEventOp *o = town_event_op(TE_UPGRADE_FREE, tag);
    if (o && !(g_camp.event_free & (1 << tag))) return 0; /* one free upgrade */
    o = town_event_op(TE_UPGRADE_DISCOUNT, tag);
    return o ? (int)(cost * (1 - o->num) + 0.5f) : cost;
}

/* Spend one free upgrade of that kind. */
void town_event_paid(int tag) {
    if (town_event_op(TE_UPGRADE_FREE, tag)) g_camp.event_free |= (uint8_t)(1 << tag);
}

/* Add a carried buff to a hero from an event. */
static void push_buff(Hero *h, int stat, int sub, int mode, float amount) {
    if (h->nebuffs >= HERO_BUFFS) return;
    HeroBuff *b = &h->ebuffs[h->nebuffs++];
    b->stat = (uint8_t)stat;
    b->sub = (uint8_t)sub;
    b->mode = (uint8_t)mode;
    b->battles = -1; /* duration_type quest_end */
    b->amount = amount;
}

/* When the party sets out: apply event buffs, bonus resolve levels, and note
 * the event on the dungeon for the results screen. */
void town_event_embark(Dungeon *d) {
    d->town_event = g_camp.event;
    d->journal = g_camp.journal;
    const DDTownEvent *e = town_event();
    for (int k = 0; e && k < e->op_count; k++) {
        const DDTownEventOp *o = &dd_town_event_ops[e->op_first + k];
        if (o->type == TE_IDLE_LEVEL) { /* heroes of the class who stay behind gain a resolve level */
            for (int i = 0; i < g_camp.nroster; i++) {
                Hero *h = &g_camp.roster[i];
                int out = 0;
                for (int r = 0; r < 4; r++) out |= g_camp.party[r] == i;
                if (!out && h->alive && h->cls == o->arg && h->level < 6)
                    hero_add_xp(h, g_rules->level_xp[h->level + 1] - h->xp);
            }
            continue;
        }
        if (o->type != TE_EMBARK_BUFF) continue;
        int dg = (o->arg >> 4) - 1, stat = o->arg & 15;
        if (dg >= 0 && dg != d->dungeon) continue; /* the dungeon named by the buff only */
        for (int r = 0; r < 4; r++) {
            if (g_camp.party[r] < 0) continue;
            Hero *h = &g_camp.roster[g_camp.party[r]];
            if (stat == TB_DAMAGE) {
                push_buff(h, STAT_DMG_LO, 0, BUFF_MULTIPLY, o->num);
                push_buff(h, STAT_DMG_HI, 0, BUFF_MULTIPLY, o->num);
            } else if (stat == TB_RESOLVE_CHECK) {
                push_buff(h, STAT_RESOLVE, 0, BUFF_ADD, o->num);
            }
        }
    }
}
