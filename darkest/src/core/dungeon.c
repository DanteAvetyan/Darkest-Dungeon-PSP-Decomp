/*
 * dungeon.c - map generation and map queries.
 *
 * dungeon_generate() builds a random map in these steps:
 *   1. Pick parameters (room/corridor counts, grid size) for the quest from
 *      the game's map-generator table (dd_map_params).
 *   2. Grow rooms outward from the entrance on a grid: repeatedly choose an
 *      existing room, a random direction, and add a room if that cell is free;
 *      each new room gets a corridor back to its parent (so the map is
 *      always connected).
 *   3. Add extra corridors between neighbouring rooms to make loops.
 *   4. Shuffle every hall square, then hand out exact counts of battles,
 *      traps, obstacles, hunger checks and curios.
 *   5. The room farthest from the entrance becomes the goal / boss lair;
 *      other rooms get battles and guarded curios.
 * Fixed story maps (Old Road, Darkest Dungeon) are built by other functions
 * from stored layouts instead of randomness.
 */
#include "dungeon.h"

#include <string.h>

#include "dd_types.h"
#include "formulas.h"
#include "loot.h"

const char *const item_names[ITEM_COUNT] = {"Food", "Torch", "Shovel", "Bandage", "Antivenom",
                                            "Medicinal Herbs", "Holy Water", "Skeleton Key", "Laudanum", "Firewood"};
const int item_prices[ITEM_COUNT] = {75, 75, 250, 150, 150, 200, 150, 200, 100, 0};

/* Weighted random choice among encounters matching (dungeon, level, kind):
 * add up their chances, roll a number in that sum, and walk the list
 * subtracting until we land inside one. Returns dd_encounters index or -1. */
int dungeon_pick_encounter(Rng *r, int dungeon, int level, int kind) {
    float total = 0;
    for (int i = 0; i < dd_encounter_count; i++) {
        const DDEncounter *e = &dd_encounters[i];
        if (e->dungeon == dungeon && e->level == level && e->kind == kind) total += e->chance;
    }
    if (total <= 0) return -1;
    float x = rng_float(r) * total;
    int last = -1;
    for (int i = 0; i < dd_encounter_count; i++) {
        const DDEncounter *e = &dd_encounters[i];
        if (e->dungeon != dungeon || e->level != level || e->kind != kind) continue;
        last = i;
        if (x < e->chance) return i;
        x -= e->chance;
    }
    return last;
}

/* INVENTORY: the raid backpack has 16 slots. Each item type stacks up to a
 * limit (food 12, torches 8...). Gold stacks to 1750 per slot. Trinkets take
 * a slot each. These tables mirror the original's limits.
 * inventory/base.*.inventory.items.darkest base_stack_limit; raid inventory max_slots 16 */
static const uint8_t item_stack[ITEM_COUNT] = {12, 8, 4, 6, 6, 6, 6, 6, 6, 1};
static const uint8_t heir_stack[HEIR_COUNT] = {6, 3, 6, 12};

/* Expand the carried stuff into display stacks (kind/ref/amount) for the
 * inventory panel. The PUSH macro splits a total into full stacks plus a
 * remainder. */
int dungeon_inventory_slots(const Dungeon *d, Loot *out, int max) {
    int n = 0;
#define PUSH(k, r, total, lim) for (int left = (total); left > 0 && n < max; left -= (lim)) { \
        out[n].kind = (uint8_t)(k); out[n].ref = (int16_t)(r); out[n].amount = (int16_t)(left > (lim) ? (lim) : left); n++; }
    for (int i = 0; i < ITEM_COUNT; i++) PUSH(LK_ITEM, i, d->items[i], item_stack[i]);
    PUSH(LK_GOLD, 0, d->gold, 1750);
    for (int i = 0; i < HEIR_COUNT; i++) PUSH(LK_HEIRLOOM, i, d->heirlooms[i], heir_stack[i]);
    for (int i = 0; i < d->ntrinkets && n < max; i++) {
        out[n].kind = LK_TRINKET; out[n].ref = d->trinkets[i]; out[n].amount = 1; n++;
    }
#undef PUSH
    return n;
}

/* Fraction (0..1) of backpack slots used. A full pack can summon the Collector. */
float dungeon_inventory_fill(const Dungeon *d) {
    int slots = (d->gold + 1749) / 1750 + d->ntrinkets;
    for (int i = 0; i < ITEM_COUNT; i++) slots += (d->items[i] + item_stack[i] - 1) / item_stack[i];
    for (int i = 0; i < HEIR_COUNT; i++) slots += (d->heirlooms[i] + heir_stack[i] - 1) / heir_stack[i];
    return slots >= 16 ? 1.0f : slots / 16.0f;
}

/* Special hall encounters that only occur when a condition holds - e.g. the
 * Shambler if the torch is low, the Collector if the bag is full. Each can
 * happen at most once per expedition (cond_met bit). */
int dungeon_roll_conditional(Rng *r, Dungeon *d) {
    if (d->plot != PLOT_NONE) return -1;
    float fill = dungeon_inventory_fill(d);
    for (int i = 0; i < dd_encounter_count; i++) {
        const DDEncounter *e = &dd_encounters[i];
        if (e->dungeon != d->dungeon || e->level != d->level) continue;
        if (e->kind != ENC_COND_DARK && e->kind != ENC_COND_INV) continue;
        int bit = 1 << (e->kind - ENC_COND_DARK);
        if (d->cond_met & bit) continue;
        if (e->kind == ENC_COND_DARK ? d->torch > e->cond : fill < e->cond) continue;
        if (!rng_chance(r, e->chance)) continue;
        d->cond_met |= (uint8_t)bit;
        return i;
    }
    return -1;
}

/* Pick a random curio valid for this dungeon. Uses RESERVOIR SAMPLING: walk
 * the list once, replace the pick with the i-th candidate with probability
 * 1/i - which yields a uniform choice without knowing the count in advance. */
int dungeon_pick_curio(Rng *r, int dungeon) {
    int n = 0, pick = -1;
    for (int i = 0; i < dd_curio_count; i++) {
        const DDCurio *c = &dd_curios[i];
        if (!(c->regions & (1 << dungeon)) || strstr(c->id, "goal") || strstr(c->id, "heirloom")) continue;
        if (rng_range(r, 0, n++) == 0) pick = i; /* reservoir sampling: uniform among valid */
    }
    return pick;
}

/* Curio lookup by string id. */
static int find_curio(const char *id) {
    for (int i = 0; i < dd_curio_count; i++)
        if (strcmp(dd_curios[i].id, id) == 0) return i;
    return -1;
}

/* A random treasure container for guarded rooms. */
static int pick_treasure(Rng *r) {
    static const char *const ids[] = {"unlocked_strongbox", "locked_strongbox", "heirloom_chest"};
    int c = find_curio(ids[rng_range(r, 0, 2)]);
    return c >= 0 ? c : find_curio("unlocked_strongbox");
}

/* Find the generator parameters for (length, quest type, dungeon), falling
 * back to an explore-type entry if that exact combination doesn't exist. */
static const DDMapParams *find_params(int length, int quest, int dungeon) {
    const DDMapParams *fallback = NULL;
    for (int i = 0; i < dd_map_param_count; i++) {
        const DDMapParams *p = &dd_map_params[i];
        if (p->dungeon != dungeon) continue;
        if (p->length == length && p->quest == quest) return p;
        if (p->length == length && p->quest == QT_EXPLORE) fallback = p;
        if (!fallback && p->quest == QT_EXPLORE) fallback = p;
    }
    return fallback ? fallback : &dd_map_params[0];
}

/* Room index at grid cell (gx,gy), or -1. */
static int room_at(const Dungeon *d, int gx, int gy) {
    for (int i = 0; i < d->nrooms; i++)
        if (d->rooms[i].gx == gx && d->rooms[i].gy == gy) return i;
    return -1;
}

/* Is there already a hall between rooms a and b? */
static int connected(const Dungeon *d, int a, int b) {
    for (int i = 0; i < d->ncorr; i++)
        if ((d->corr[i].a == a && d->corr[i].b == b) || (d->corr[i].a == b && d->corr[i].b == a)) return 1;
    return 0;
}

/* Create a hall of `squares` squares; each gets a random wall-art variant. */
static void add_corridor(Dungeon *d, Rng *r, int a, int b, int squares) {
    if (d->ncorr >= DG_MAX_CORRIDORS || connected(d, a, b)) return;
    Corridor *c = &d->corr[d->ncorr++];
    memset(c, 0, sizeof(*c));
    c->a = (uint8_t)a;
    c->b = (uint8_t)b;
    c->nseg = (uint8_t)(squares < 1 ? 1 : squares > DG_MAX_SEGMENTS ? DG_MAX_SEGMENTS : squares);
    for (int s = 0; s < c->nseg; s++) {
        c->seg[s] = SPOT_EMPTY;
        c->seg_enc[s] = -1;
        c->wall[s] = (uint8_t)rng_range(r, 0, 255);
    }
}

/* Roll a [min,max] pair from the map params. */
static int rand_pair(Rng *r, const uint8_t v[2]) { return rng_range(r, v[0], v[1] > v[0] ? v[1] : v[0]); }

/* Breadth-first search over the room graph: dist[i] = number of halls from
 * `from` to room i (-1 if unreachable). With visited_only, only walks through
 * rooms the party has already been in (used for map fast-travel). */
static void distances(const Dungeon *d, int from, int dist[DG_MAX_ROOMS], int visited_only) {
    int queue[DG_MAX_ROOMS], qh = 0, qt = 0;
    for (int i = 0; i < d->nrooms; i++) dist[i] = -1;
    dist[from] = 0;
    queue[qt++] = from;
    while (qh < qt) {
        int r0 = queue[qh++];
        for (int c = 0; c < d->ncorr; c++) {
            int o = d->corr[c].a == r0 ? d->corr[c].b : d->corr[c].b == r0 ? d->corr[c].a : -1;
            if (o < 0 || dist[o] >= 0) continue;
            if (visited_only && !d->rooms[o].visited) continue;
            dist[o] = dist[r0] + 1;
            queue[qt++] = o;
        }
    }
}

/* Hall-distance from `from` to `to` through visited rooms (-1 if none). */
int dungeon_reachable(const Dungeon *d, int from, int to) {
    int dist[DG_MAX_ROOMS];
    distances(d, from, dist, 1);
    return dist[to];
}

/* Layout and contents follow scripts/map_generator.darkest: room and corridor counts, grid size
 * (grown like MapGenerator::CalculateGridSize), squares per hallway = spacing - 1 (one per grid
 * step, see MapGenerator::CreateCorridorCardinal), and the exact numbers of hallway battles,
 * traps, obstacles, curios and hunger checks and of room battles / guarded rooms. */
void dungeon_generate(Dungeon *d, Rng *r, int dungeon, int level, int length, int quest) {
    /* --- step 0: blank slate + fixed header fields --- */
    memset(d, 0, sizeof(*d));
    d->dungeon = (uint8_t)dungeon;
    d->level = (uint8_t)level;
    d->length = (uint8_t)length;
    d->quest = (uint8_t)(quest > QUEST_BOSS ? QUEST_EXPLORE : quest);
    d->torch = 100;
    const DDMapParams *p = find_params(length, quest, dungeon);
    int want = p->rooms < DG_MAX_ROOMS ? p->rooms : DG_MAX_ROOMS;
    int gw = p->grid_w, gh = p->grid_h;
    while (gw * gh < want) { /* CalculateGridSize: grow the short side first */
        if ((float)gw / gh < (float)p->grid_w / p->grid_h) gw++;
        else gh++;
    }
    if (gw > DG_GRID_W) gw = DG_GRID_W;
    if (gh > DG_GRID_H) gh = DG_GRID_H;
    int squares = p->spacing - 1;  /* squares per hall = grid spacing minus 1 */

    /* --- step 1/2: rooms --- */

    /* rooms grow from the entrance on the grid; each new room joins the room it grew from */
    Room *e = &d->rooms[0];
    e->gx = 0;
    e->gy = (int8_t)(gh / 2);
    e->content = SPOT_ENTRANCE;
    e->visited = e->done = 1;
    e->encounter = e->curio = -1;
    d->nrooms = 1;
    for (int guard = 0; d->nrooms < want && guard < 4000; guard++) {
        int from = rng_range(r, 0, d->nrooms - 1);
        static const int dx[4] = {1, 0, 0, -1}, dy[4] = {0, 1, -1, 0};
        int dir = rng_range(r, 0, 3);
        int gx = d->rooms[from].gx + dx[dir], gy = d->rooms[from].gy + dy[dir];
        if (gx < 0 || gy < 0 || gx >= gw || gy >= gh || room_at(d, gx, gy) >= 0) continue;
        Room *rm = &d->rooms[d->nrooms];
        memset(rm, 0, sizeof(*rm));
        rm->gx = (int8_t)gx;
        rm->gy = (int8_t)gy;
        rm->encounter = rm->curio = -1;
        rm->wall = (uint8_t)rng_range(r, 0, 255);
        add_corridor(d, r, from, d->nrooms, squares);
        d->nrooms++;
    }
    /* --- step 3: extra halls --- */
    /* extra corridors between neighbours up to the corridor budget (loops), by connectivity */
    for (int guard = 0; d->ncorr < p->corridors && guard < 400; guard++) {
        int a = rng_range(r, 0, d->nrooms - 1);
        static const int dx[4] = {1, 0, 0, -1}, dy[4] = {0, 1, -1, 0};
        int dir = rng_range(r, 0, 3);
        int b = room_at(d, d->rooms[a].gx + dx[dir], d->rooms[a].gy + dy[dir]);
        if (b >= 0 && !connected(d, a, b) && rng_chance(r, p->connectivity)) add_corridor(d, r, a, b, squares);
    }

    /* --- step 4: hall contents. Fisher-Yates shuffle of all squares, then the
     * first N become battles, next M traps, and so on (exact counts). --- */
    /* hallway contents: shuffle every square, then hand out the exact counts */
    int tiles[DG_MAX_CORRIDORS * DG_MAX_SEGMENTS], nt = 0;
    for (int c = 0; c < d->ncorr; c++)
        for (int s = 0; s < d->corr[c].nseg; s++) tiles[nt++] = c * DG_MAX_SEGMENTS + s;
    for (int i = nt - 1; i > 0; i--) {
        int j = rng_range(r, 0, i), t = tiles[i];
        tiles[i] = tiles[j];
        tiles[j] = t;
    }
    int ti = 0;
    struct { uint8_t kind; int count; } fills[5] = {
        {SPOT_BATTLE, rand_pair(r, p->hall_battle)}, {SPOT_TRAP, rand_pair(r, p->hall_trap)},
        {SPOT_OBSTACLE, rand_pair(r, p->hall_obstacle)}, {SPOT_HUNGER, rand_pair(r, p->hall_hunger)},
        {SPOT_CURIO, rand_pair(r, p->hall_curio)},
    };
    for (int f = 0; f < 5; f++)
        for (int k = 0; k < fills[f].count && ti < nt; k++, ti++) {
            Corridor *c = &d->corr[tiles[ti] / DG_MAX_SEGMENTS];
            int s = tiles[ti] % DG_MAX_SEGMENTS;
            c->seg[s] = fills[f].kind;
            if (fills[f].kind == SPOT_BATTLE) c->seg_enc[s] = (int16_t)dungeon_pick_encounter(r, dungeon, level, ENC_HALL);
            if (fills[f].kind == SPOT_CURIO) c->seg_enc[s] = (int16_t)dungeon_pick_curio(r, dungeon);
            if (c->seg_enc[s] < 0 && (fills[f].kind == SPOT_BATTLE || fills[f].kind == SPOT_CURIO)) c->seg[s] = SPOT_EMPTY;
        }

    /* --- step 5: rooms. `far` = room with the greatest hall-distance from the
     * entrance; `order` = shuffled list of the other rooms to fill. --- */
    /* rooms: the farthest room is the goal / boss lair; then battles and guarded rooms */
    int dist[DG_MAX_ROOMS], far = 0;
    distances(d, 0, dist, 0);
    for (int i = 1; i < d->nrooms; i++)
        if (dist[i] > dist[far]) far = i;
    int order[DG_MAX_ROOMS], no = 0;
    for (int i = 1; i < d->nrooms; i++)
        if (i != far || quest != QUEST_BOSS) order[no++] = i;
    for (int i = no - 1; i > 0; i--) {
        int j = rng_range(r, 0, i), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    int oi = 0;
    if (quest == QUEST_BOSS && far > 0) {
        int enc = dungeon_pick_encounter(r, dungeon, level, ENC_BOSS);
        if (enc >= 0) {
            d->rooms[far].content = SPOT_BOSS;
            d->rooms[far].encounter = (int16_t)enc;
        }
    }
    int battles = rand_pair(r, p->room_battles), guarded = rand_pair(r, p->guarded_curio);
    int treasure = rand_pair(r, p->guarded_treasure);
    for (int k = 0; k < battles && oi < no; k++, oi++) {
        Room *rm = &d->rooms[order[oi]];
        rm->encounter = (int16_t)dungeon_pick_encounter(r, dungeon, level, ENC_ROOM);
        if (rm->encounter < 0) continue;
        rm->content = SPOT_BATTLE;
        if (guarded > 0) { rm->curio = (int16_t)dungeon_pick_curio(r, dungeon); guarded--; }
        else if (treasure > 0) { rm->curio = (int16_t)pick_treasure(r); treasure--; }
    }
    /* any guarded loot left over sits in quiet rooms */
    for (; (guarded > 0 || treasure > 0) && oi < no; oi++) {
        Room *rm = &d->rooms[order[oi]];
        rm->content = SPOT_CURIO;
        rm->curio = (int16_t)(guarded-- > 0 ? dungeon_pick_curio(r, dungeon) : (treasure--, pick_treasure(r)));
    }
    d->room = 0;
    d->corridor = -1;
    if (d->room < 0 || d->room >= d->nrooms || d->rooms[d->room].content != SPOT_ENTRANCE) d->room = 0; /* no entrance: the Shrieker's perch */
}

/* The Old Road (scripts/starting_save/persist.raid.json + persist.map.json): a fixed Weald map,
 * entrance -> one hallway -> the final room. The hallway holds the lone cutthroat (named mash
 * "tutorial_1") and the Traveller's Tent; the final room holds "tutorial_2" guarding the trapped
 * chest. Completion pays 5000 gold (completion_reward). */
void dungeon_tutorial(Dungeon *d, Rng *r) {
    memset(d, 0, sizeof(*d));
    d->dungeon = DG_WEALD;
    d->level = 1;
    d->length = LEN_SHORT;
    d->quest = QUEST_EXPLORE;
    d->plot = PLOT_OLD_ROAD;
    d->torch = 100;
    Room *e = &d->rooms[0], *f = &d->rooms[1];
    e->gx = 0; e->gy = 0;
    e->content = SPOT_ENTRANCE;
    e->visited = e->done = 1;
    e->encounter = e->curio = -1;
    f->gx = 1; f->gy = 0;
    f->wall = (uint8_t)rng_range(r, 0, 255);
    f->encounter = dd_enc_tutorial[1];
    f->content = f->encounter >= 0 ? SPOT_BATTLE : SPOT_CURIO;
    f->curio = (int16_t)find_curio("bandits_trapped_chest");
    d->nrooms = 2;
    add_corridor(d, r, 0, 1, 3);
    Corridor *c = &d->corr[0];
    c->seg[0] = SPOT_BATTLE;
    c->seg_enc[0] = dd_enc_tutorial[0];
    c->seg[2] = SPOT_CURIO;
    c->seg_enc[2] = (int16_t)find_curio("travellers_tent_tutorial");
    for (int s = 0; s < c->nseg; s++)
        if (c->seg_enc[s] < 0) c->seg[s] = SPOT_EMPTY;
    d->room = 0;
    d->corridor = -1;
}

/* A Darkest Dungeon expedition (plot quest qn 0..3): the game's fixed map, exported from
 * maps/DD_map*.dm into dd_plot_rooms / dd_plot_corrs. Room positions are map tiles, so the
 * minimap draws them at 1/6 scale (a room and its hall span about 6 tiles). */
void dungeon_plot(Dungeon *d, Rng *r, int qn) {
    const DDPlotQuest *q = &dd_plot_quests[qn];
    memset(d, 0, sizeof(*d));
    d->dungeon = q->dungeon; /* DG_DARKEST, or the town / weald for the event quests */
    d->level = q->level;     /* difficulty in quest.plot_quests.json (6 = "Darkest") */
    d->length = q->length;
    d->quest = q->quest;
    d->plot = (uint8_t)(PLOT_DD + qn);
    d->cell = 6;
    d->torch = 100;
    d->heir = HEIR_CREST;
    d->nrooms = q->nrooms < DG_MAX_ROOMS ? q->nrooms : DG_MAX_ROOMS;
    for (int i = 0; i < d->nrooms; i++) {
        const DDPlotRoom *pr = &dd_plot_rooms[q->room_first + i];
        Room *rm = &d->rooms[i];
        rm->gx = pr->gx;
        rm->gy = pr->gy;
        rm->content = pr->spot;
        rm->encounter = pr->enc;
        rm->curio = pr->curio;
        rm->wall = (uint8_t)rng_range(r, 0, 255);
        if (pr->spot == SPOT_ENTRANCE) {
            rm->visited = rm->done = 1;
            d->room = i;
        }
    }
    for (int i = 0; i < q->ncorr; i++) {
        const DDPlotCorr *pc = &dd_plot_corrs[q->corr_first + i];
        if (pc->a >= d->nrooms || pc->b >= d->nrooms) continue;
        int n = d->ncorr;
        add_corridor(d, r, pc->a, pc->b, pc->nseg);
        if (d->ncorr == n) continue;
        Corridor *c = &d->corr[n];
        for (int s = 0; s < c->nseg; s++) {
            c->seg[s] = pc->seg[s];
            c->seg_enc[s] = pc->ref[s];
        }
    }
    d->corridor = -1;
}

/* Index of the room holding the boss, or -1. */
int dungeon_boss_room(const Dungeon *d) {
    for (int i = 0; i < d->nrooms; i++)
        if (d->rooms[i].content == SPOT_BOSS) return i;
    return -1;
}

/* List the halls attached to a room into out[] (max 4: grid neighbours). */
int dungeon_room_exits(const Dungeon *d, int room, int out[4]) {
    int n = 0;
    for (int i = 0; i < d->ncorr && n < 4; i++)
        if (d->corr[i].a == room || d->corr[i].b == room) out[n++] = i;
    return n;
}

#define SCOUT_BASE 0.25f /* rules.json scouting_chance_base */
#define SCOUT_CRIT 0.50f /* rules.json scouting_crit_success */

/* Reveal rooms/halls around `room`: depth 1 = neighbours, 2 = two steps. */
static void scout_ring(Dungeon *d, int room, int depth) {
    int ex[4], n = dungeon_room_exits(d, room, ex);
    for (int i = 0; i < n; i++) {
        int o = dungeon_other_end(&d->corr[ex[i]], room);
        d->rooms[o].scouted = 1;
        d->corr[ex[i]].scouted = 1; /* the hall on the way: its traps become visible */
        if (depth > 1) scout_ring(d, o, depth - 1);
    }
}

/* Scouting: on entering a room roll once. Chance = base + light bonus + party
 * bonus. A roll below the chance scouts neighbours; below half of it, a
 * "critical" scouts two rooms deep. Disabled on most story quests. */
int dungeon_scout(Dungeon *d, Rng *r, float bonus) {
    /* darkness player_scouting_increase (+15 at 76-100 light, +7.5 at 51-75) */
    float light = dark_for(d->torch)[DK_SCOUT] / 100.0f;
    /* is_scouting_enabled: off on the plot quests except the first Ruins quest */
    if (d->plot == PLOT_OLD_ROAD || (d->plot >= PLOT_DD && DUNGEON_DD_QUEST(d) != PQ_TUTORIAL)) return 0;
    /* Party::OnAreaVisit (decomp10): one roll; under chance scouts 2 steps, under chance *
     * scouting_crit_success it is a crit that reaches 4 steps (two rooms on) */
    float chance = SCOUT_BASE + light + bonus, roll = rng_float(r);
    if (d->room < 0 || roll >= chance) return 0;
    int crit = roll < chance * SCOUT_CRIT;
    scout_ring(d, d->room, crit ? 2 : 1);
    return 1 + crit;
}

/* RaidDisplay::MoveToArea (decomp24) + Map::MaybeAddToCorridor: walking back into an explored hall
 * (not in the Darkest Dungeon) rolls corridor_return_content for a battle, a hunger check and a
 * trap; each hit goes on a random empty tile. Returns how many were added. */
int dungeon_return_content(Dungeon *d, Corridor *c, Rng *r) {
    static const uint8_t kind[3] = {SPOT_BATTLE, SPOT_HUNGER, SPOT_TRAP};
    int band = d->torch <= 0 ? 0 : d->torch <= 25 ? 1 : d->torch <= 50 ? 2 : d->torch <= 75 ? 3 : 4, added = 0;
    for (int k = 0; k < 3; k++) {
        int empty[DG_MAX_SEGMENTS], ne = 0;
        for (int s = 0; s < c->nseg; s++)
            if (c->seg[s] == SPOT_EMPTY || (c->seg_done[s] & 1)) empty[ne++] = s;
        if (!ne || rng_float(r) >= dd_corridor_return[k][band]) continue;
        int s = empty[rng_range(r, 0, ne - 1)];
        if (kind[k] == SPOT_BATTLE) {
            int enc = dungeon_pick_encounter(r, d->dungeon, d->level, ENC_HALL);
            if (enc < 0) continue;
            c->seg_enc[s] = (int16_t)enc;
        }
        c->seg[s] = kind[k];
        c->seg_done[s] &= 2; /* keep "walked", the new content is unresolved */
        added++;
    }
    return added;
}

/* The room at the far end of a hall from `room`. */
int dungeon_other_end(const Corridor *c, int room) { return c->a == room ? c->b : c->a; }

/* % of rooms visited (explore quests need 90%). */
int dungeon_explored_percent(const Dungeon *d) {
    int v = 0;
    for (int i = 0; i < d->nrooms; i++) v += d->rooms[i].visited;
    return d->nrooms ? v * 100 / d->nrooms : 0;
}

/* Unfinished room battles (cleanse quests need 0). */
int dungeon_battles_left(const Dungeon *d) {
    int n = 0;
    for (int i = 0; i < d->nrooms; i++) n += d->rooms[i].encounter >= 0 && !d->rooms[i].done;
    return n;
}

/* Has the quest goal been reached? Depends on quest type: explore 90%,
 * cleanse = all room battles won, boss = boss room done; story quests have
 * their own conditions. */
int dungeon_goal_met(const Dungeon *d) {
    if (d->plot >= PLOT_DD) {
        const DDPlotQuest *q = &dd_plot_quests[DUNGEON_DD_QUEST(d)];
        if (q->goals) return d->goals_done >= q->goals; /* activate the quest curios */
        if (q->quest == QUEST_EXPLORE) { /* explore_all_rooms */
            for (int i = 0; i < d->nrooms; i++)
                if (!d->rooms[i].visited) return 0;
            return 1;
        }
        int b = dungeon_boss_room(d);
        return b < 0 || d->rooms[b].done;
    }
    if (d->plot) return dungeon_battles_left(d) == 0; /* goal "tutorial_final_room": both brigands dead */
    if (d->quest == QUEST_EXPLORE) return dungeon_explored_percent(d) >= 90;
    if (d->quest == QUEST_BOSS) {
        int b = dungeon_boss_room(d);
        return b < 0 || d->rooms[b].done;
    }
    return dungeon_battles_left(d) == 0;
}

const char *const heir_names[HEIR_COUNT] = {"Busts", "Portraits", "Deeds", "Crests"};

/* Heirlooms paid on success (from the original's amount table by level and
 * length). */
int dungeon_heirloom_reward(const Dungeon *d) {
    if (d->plot >= PLOT_DD) return dd_plot_quests[DUNGEON_DD_QUEST(d)].crests; /* paid in crests */
    if (d->plot) return 0; /* the Old Road pays gold only */
    /* quest.generation.json heirloom_amount_table: [type][level][length] */
    static const int amt[2][3][3] = {{{2, 2, 4}, {2, 3, 6}, {3, 5, 9}}, {{3, 5, 9}, {4, 6, 12}, {6, 9, 18}}};
    int lv = d->level >= 5 ? 2 : d->level >= 3 ? 1 : 0;
    int len = d->length >= 1 && d->length <= 3 ? d->length - 1 : 0;
    return amt[d->heir >= HEIR_DEED][lv][len];
}

/* Gold paid on success, by difficulty (1/3/5 -> row) and length (column). */
int dungeon_reward(const Dungeon *d) {
    if (d->plot >= PLOT_DD) return dd_plot_quests[DUNGEON_DD_QUEST(d)].gold;
    if (d->plot) return 5000; /* the Old Road's completion_reward */
    /* campaign/quest/quest.generation.json item_table: gold by difficulty and length */
    static const int gold[3][3] = {{3000, 4500, 7500}, {4500, 6750, 11250}, {6000, 9000, 15000}};
    int lv = d->level >= 5 ? 2 : d->level >= 3 ? 1 : 0;
    int len = d->length >= 1 && d->length <= 3 ? d->length - 1 : 0;
    return gold[lv][len];
}
