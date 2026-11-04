/* Loot rolls (see loot.h). Table choice: same id, difficulty equal to the dungeon level
 * (0 = any) and dungeon equal or unset; the most specific match wins. */
#include "loot.h"

#include <stdio.h>

#include "trinket.h"

/* Several rows can share an id, specialised by difficulty and dungeon. Pick the
 * most specific row that still applies (score: dungeon match counts double). */
static const DDLootTable *find_table(int id, int level, int dungeon) {
    const DDLootTable *best = NULL;
    int best_score = -1;
    for (int i = 0; i < dd_loot_table_count; i++) {
        const DDLootTable *t = &dd_loot_tables[i];
        if (t->id != id) continue;
        if (t->difficulty && t->difficulty != level) continue;
        if (t->dungeon >= 0 && t->dungeon != dungeon) continue;
        int score = (t->difficulty != 0) + 2 * (t->dungeon >= 0);
        if (score > best_score) { best = t; best_score = score; }
    }
    if (!best && level != 1) return find_table(id, 1, dungeon); /* e.g. no level-6 row */
    return best;
}

/* Add to the result list, merging with an existing stack of the same thing. */
static int add(Loot *out, int n, int kind, int ref, int amount) {
    if (amount <= 0) return n;
    for (int i = 0; i < n; i++)
        if (out[i].kind == kind && out[i].ref == ref) {
            out[i].amount = (int16_t)(out[i].amount + amount);
            return n;
        }
    if (n >= LOOT_MAX) return n;
    out[n].kind = (uint8_t)kind;
    out[n].ref = (int16_t)ref;
    out[n].amount = (int16_t)amount;
    return n + 1;
}

/* One weighted draw. Nested tables recurse (depth-limited to stay safe). */
static int draw_once(Rng *r, int table, int level, int dungeon, Loot *out, int n, int depth) {
    const DDLootTable *t = find_table(table, level, dungeon);
    if (!t || depth > 4) return n;
    int total = 0;
    for (int i = 0; i < t->count; i++) total += dd_loot_entries[t->first + i].chances;
    if (total <= 0) return n;
    int x = rng_range(r, 0, total - 1);
    const DDLootEntry *e = NULL;
    for (int i = 0; i < t->count && !e; i++) {
        const DDLootEntry *c = &dd_loot_entries[t->first + i];
        if (x < c->chances) e = c;
        else x -= c->chances;
    }
    if (!e) return n;
    switch (e->kind) {
    case LK_TABLE: return draw_once(r, e->ref, level, dungeon, out, n, depth + 1);
    case LK_GOLD: return add(out, n, LK_GOLD, 0, e->amount);
    case LK_GEM: return add(out, n, LK_GEM, e->ref, e->amount ? e->amount : 1);
    case LK_HEIRLOOM: return add(out, n, LK_HEIRLOOM, e->ref, e->amount);
    case LK_ITEM: return add(out, n, LK_ITEM, e->ref, e->amount ? e->amount : 1);
    case LK_TRINKET: { /* entry ref = rarity; a random trinket of it that can drop here */
        int tr = trinket_roll(r, e->ref, dungeon);
        return tr >= 0 ? add(out, n, LK_TRINKET, tr, 1) : n;
    }
    case LK_JOURNAL: /* page range ref..amount; not merged like stacks */
        if (n >= LOOT_MAX) return n;
        out[n].kind = LK_JOURNAL;
        out[n].ref = e->ref;
        out[n].amount = e->amount;
        return n + 1;
    default: return n;
    }
}

/* Draw `draws` times from a table, appending into out[]. */
int loot_draw(Rng *r, int table, int draws, int level, int dungeon, Loot *out, int n) {
    for (int k = 0; k < draws; k++) n = draw_once(r, table, level, dungeon, out, n, 0);
    return n;
}

/* Loot after a won fight: each dead monster rolls its own table; low torch
 * light can add a bonus draw. */
int loot_battle(Rng *r, const Battle *b, int level, int dungeon, int torch, Loot *out) {
    int n = 0;
    for (int i = 0; i < MAX_ACTORS; i++) {
        const Actor *a = &b->actors[i];
        if (!a->used || a->is_hero || a->alive || a->is_corpse || a->no_loot) continue;
        const DDMonsterClass *m = &dd_monsters[a->cls];
        if (m->loot_table >= 0) n = loot_draw(r, m->loot_table, m->loot_count, level, dungeon, out, n);
    }
    /* darkness_overrides: fighting in low light pays extra draws */
    for (int k = dd_darkness_battle_count - 1; k >= 0; k--) {
        const DDDarknessLoot *dk = &dd_darkness_battle[k];
        if (torch < dk->darkness) continue;
        if (rng_chance(r, dk->chance)) n = loot_draw(r, dk->code, dk->ncodes, level, dungeon, out, n);
        break;
    }
    return n;
}

/* Move found loot into the expedition (gems convert to gold at once). */
void loot_take(Dungeon *d, const Loot *l, int n) {
    for (int i = 0; i < n; i++) {
        switch (l[i].kind) {
        case LK_GOLD: d->gold += l[i].amount; break;
        case LK_GEM: d->gold += l[i].amount * dd_gems[l[i].ref].value; break;
        case LK_HEIRLOOM: d->heirlooms[l[i].ref % HEIR_COUNT] += (uint16_t)l[i].amount; break;
        case LK_ITEM: {
            int v = d->items[l[i].ref] + l[i].amount;
            d->items[l[i].ref] = (uint8_t)(v > 255 ? 255 : v);
            break;
        }
        case LK_TRINKET:
            for (int k = 0; k < l[i].amount && d->ntrinkets < DG_TRINKETS; k++) d->trinkets[d->ntrinkets++] = l[i].ref;
            break;
        case LK_JOURNAL: { /* the next page of the range not found yet (Hypothesis: pages come in order) */
            int p = loot_journal_page(d, &l[i]);
            if (p >= 0) d->journal |= 1u << p;
            break;
        }
        }
    }
}

/* Journal::System::RaidAttemptReadRandomPage (decomp11): a random page in the range; if it is
 * already read, the next unread one after it (then from the start of the range) */
int loot_journal_page(const Dungeon *d, const Loot *l) {
    int lo = l->ref, hi = l->amount < JOURNAL_PAGES ? l->amount : JOURNAL_PAGES - 1;
    if (lo > hi) return -1;
    static uint32_t seed = 0x51ed;
    seed = (seed * 1103515245u + 12345u) ^ d->steps;
    int start = lo + (int)((seed >> 8) % (uint32_t)(hi - lo + 1));
    for (int k = 0; k <= hi - lo; k++) {
        int p = lo + (start - lo + k) % (hi - lo + 1);
        if (!(d->journal >> p & 1)) return p;
    }
    return -1;
}

/* Human-readable text for a loot stack. */
const char *loot_label(const Loot *l, char *buf, int len) {
    switch (l->kind) {
    case LK_JOURNAL: snprintf(buf, len, "Journal page"); break;
    case LK_GOLD: snprintf(buf, len, "%d Gold", l->amount); break;
    case LK_GEM:
        if (l->amount > 1) snprintf(buf, len, "%d %s (%dg)", l->amount, dd_gems[l->ref].name, dd_gems[l->ref].value * l->amount);
        else snprintf(buf, len, "%s (%dg)", dd_gems[l->ref].name, dd_gems[l->ref].value);
        break;
    case LK_HEIRLOOM: snprintf(buf, len, "%d %s", l->amount, heir_names[l->ref % HEIR_COUNT]); break;
    case LK_ITEM: snprintf(buf, len, "%d %s", l->amount, item_names[l->ref]); break;
    case LK_TRINKET: snprintf(buf, len, "%s (%s)", dd_trinkets[l->ref].name, trinket_rarity_name(dd_trinkets[l->ref].rarity)); break;
    default: buf[0] = 0;
    }
    return buf;
}
