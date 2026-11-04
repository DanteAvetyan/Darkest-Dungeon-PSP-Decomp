/*
 * main_host.c - the PC build's main(): a SCRIPTED TEST RUNNER.
 * It implements platform.h with ordinary C file I/O and drives the exact same
 * game_update/game_draw as the PSP, but instead of a controller it reads
 * "tokens" from the command line (below) and turns each into frames of input.
 * A fake clock (`ticks`, +16 ms per frame) makes runs deterministic. This is
 * how screenshots and regression tests are produced without hardware.
 * PC test runner: runs the real game loop at 60 fps with scripted input and saves screenshots.
 * Run from the folder that contains assets/.
 * Usage: darkest_host <token>...
 *   X O SQ TRI U D L R ST SEL LT RT   press a button (held 2 frames, then released 2 frames)
 *   hold:R:30                       hold a button for 30 frames
 *   wait:60                         run 60 frames with no input
 *   shot:name                       save name.ppm
 *   fight:N                         start battle with dd_encounters[N] (test shortcut)
 *   quest:D                         make the first board quest one in dungeon D (0 ruins .. 3 cove)
 *   clear                           resolve every room/hall event (quest goal tests)
 *   plotq:N                         put plot quest N on the board (4 town invasion, 5 Shrieker)
 *   week:N                          set the campaign week (Stygian limit tests)
 *   journal:MASK                    set found journal pages (bit = page)
 *   event:N                         force town event dd_town_events[N]
 *   heir:N                          set every heirloom count to N (estate upgrade tests)
 *   give:I:N                        set dungeon item I (ItemId) to N, e.g. give:9:1 firewood
 *   fuzz:N                          N frames of random button presses (crash testing)
 *   skipintro                       count the Old Road as done and go to the hamlet (week 2)
 *   trinket:N                       put dd_trinkets[N] in the estate trinket chest
 *   dd:N                            embark the current party on Darkest Dungeon quest N (1-4)
 *   won                             mark the campaign won (the epilogue plays in the hamlet) */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "campaign.h"
#include "town_event.h"
#include "game.h"
#include "gfx.h"
#include "platform.h"
#include "scenes.h"

int gfx_soft_save_ppm(const char *path);

/* Fake clock: advanced 16 ms per frame, returned by plat_ticks_ms(). */
static uint32_t ticks;

/* No alignment needs on the PC. */
void *plat_alloc(int n) { return malloc(n > 0 ? n : 1); }

/* Read a whole file (see platform.h). */
void *plat_read_file(const char *path, int *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n > 0 ? n : 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf && size) *size = (int)n;
    return buf;
}

/* Write a whole file. */
int plat_write_file(const char *path, const void *data, int size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, size, f) == (size_t)size;
    fclose(f);
    return ok ? 0 : -1;
}

uint32_t plat_ticks_ms(void) { return ticks; }

/* Log lines go to stderr. */
void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Run ONE frame: advance the fake clock, update with 1/60 s, draw. */
static void frame(uint32_t buttons) {
    ticks += 16;
    game_update(1.0f / 60.0f, buttons);
    game_draw();
}

/* Map a token name ("X", "ST"...) to a BTN_* bit, or 0 if unknown. */
static uint32_t button(const char *t) {
    static const struct { const char *n; uint32_t b; } map[] = {
        {"X", BTN_CROSS}, {"O", BTN_CIRCLE}, {"SQ", BTN_SQUARE}, {"TRI", BTN_TRIANGLE},
        {"U", BTN_UP}, {"D", BTN_DOWN}, {"L", BTN_LEFT}, {"R", BTN_RIGHT}, {"LT", BTN_L}, {"RT", BTN_R},
        {"ST", BTN_START}, {"SEL", BTN_SELECT}, {"LT", BTN_L}, {"RT", BTN_R},
    };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(map[i].n, t) == 0) return map[i].b;
    return 0;
}

/* Start the game with a fixed seed, then process each command-line token in
 * order: waits, button presses, screenshots and the many test shortcuts
 * listed in the header comment. */
int main(int argc, char **argv) {
    gfx_init();
    if (game_init(12345) != 0) {
        fprintf(stderr, "game_init failed (run from the folder containing assets/)\n");
        return 1;
    }
    frame(0);
    for (int i = 1; i < argc && !game_quit_requested(); i++) {
        const char *t = argv[i];
        if (strncmp(t, "wait:", 5) == 0) {
            for (int k = atoi(t + 5); k > 0; k--) frame(0);
        } else if (strcmp(t, "listmax") == 0) {
            extern long gfx_soft_max_bytes;
            fprintf(stderr, "max display list bytes so far: %ld\n", gfx_soft_max_bytes);
            gfx_soft_max_bytes = 0;
        } else if (strncmp(t, "shot:", 5) == 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s.ppm", t + 5);
            gfx_soft_save_ppm(path);
        } else if (strncmp(t, "fight:", 6) == 0) {
            battle_scene_start(atoi(t + 6), "assets/dungeons/crypts/room_wall.altar.ddi");
            frame(0);
        } else if (strcmp(t, "skipintro") == 0) {
            if (g_camp.in_dungeon && g_camp.dungeon.plot) {
                g_camp.in_dungeon = 0;
                g_camp.quests_done++;
                g_camp.gold += 5000;
                campaign_next_week();
                scene_switch(SCENE_TOWN);
                frame(0);
            }
        } else if (strncmp(t, "quest:", 6) == 0) { /* first board quest goes to dungeon D (0-3) */
            if (g_camp.nboard) {
                g_camp.board[0].dungeon = (uint8_t)atoi(t + 6);
                g_camp.board[0].plot = 0;
            }
        } else if (strcmp(t, "clear") == 0) { /* every room and hall event of the expedition resolved */
            Dungeon *d = &g_camp.dungeon;
            for (int i = 0; i < d->nrooms; i++) d->rooms[i].done = d->rooms[i].visited = 1;
            for (int i = 0; i < d->ncorr; i++)
                for (int k = 0; k < d->corr[i].nseg; k++) d->corr[i].seg_done[k] |= 1;
        } else if (strncmp(t, "plotq:", 6) == 0) { /* post plot quest N (4 invasion, 5 Shrieker) */
            town_event_post_quest(atoi(t + 6));
            if (g_camp.nboard > 1) { /* move it to the front so ST X picks it */
                QuestOffer q = g_camp.board[g_camp.nboard - 1];
                memmove(&g_camp.board[1], &g_camp.board[0], (g_camp.nboard - 1) * sizeof(QuestOffer));
                g_camp.board[0] = q;
            }
        } else if (strncmp(t, "act:", 4) == 0) { /* act:H:A puts roster hero H in activity A (1..6) */
            int h = atoi(t + 4), a = atoi(strchr(t + 4, ':') ? strchr(t + 4, ':') + 1 : "1");
            if (h >= 0 && h < g_camp.nroster) g_camp.roster[h].busy = (uint8_t)a;
        } else if (strcmp(t, "nextweek") == 0) { /* end the week (activity side effects), print the log */
            campaign_next_week();
            for (int i = 0; i < g_camp.nlog; i++) fprintf(stderr, "LOG %s\n", g_camp.log[i]);
            g_camp.nlog = 0;
        } else if (strncmp(t, "bleed:", 6) == 0) { /* first party hero bleeds N per tile for 4 tiles */
            int hi = g_camp.party[0];
            if (hi >= 0) { g_camp.roster[hi].bleed[0].amount = (int8_t)atoi(t + 6); g_camp.roster[hi].bleed[0].rounds = 4; }
        } else if (strcmp(t, "hp") == 0) { /* print the party's HP */
            for (int r = 0; r < 4; r++)
                if (g_camp.party[r] >= 0) fprintf(stderr, "HP %s %d\n", g_camp.roster[g_camp.party[r]].name, g_camp.roster[g_camp.party[r]].hp);
        } else if (strcmp(t, "obstacles") == 0) { /* every empty hall tile becomes an obstacle (testing) */
            for (int i = 0; i < g_camp.dungeon.ncorr; i++)
                for (int s = 0; s < g_camp.dungeon.corr[i].nseg; s++)
                    if (g_camp.dungeon.corr[i].seg[s] == SPOT_EMPTY) g_camp.dungeon.corr[i].seg[s] = SPOT_OBSTACLE;
        } else if (strcmp(t, "traps") == 0) { /* every empty hall tile becomes a trap (testing) */
            for (int i = 0; i < g_camp.dungeon.ncorr; i++)
                for (int s = 0; s < g_camp.dungeon.corr[i].nseg; s++)
                    if (g_camp.dungeon.corr[i].seg[s] == SPOT_EMPTY) g_camp.dungeon.corr[i].seg[s] = SPOT_TRAP;
            for (int i = 0; i < g_camp.dungeon.ncorr; i++) {
                fprintf(stderr, "corr %d:", i);
                for (int s = 0; s < g_camp.dungeon.corr[i].nseg; s++) fprintf(stderr, " %d", g_camp.dungeon.corr[i].seg[s]);
                fprintf(stderr, "\n");
            }
        } else if (strcmp(t, "scoutall") == 0) { /* scout every room and hall of the expedition */
            for (int i = 0; i < g_camp.dungeon.nrooms; i++) g_camp.dungeon.rooms[i].scouted = 1;
            for (int i = 0; i < g_camp.dungeon.ncorr; i++) g_camp.dungeon.corr[i].scouted = 1;
        } else if (strncmp(t, "torch:", 6) == 0) { /* set the expedition's torchlight 0..100 */
            g_camp.dungeon.torch = atoi(t + 6);
        } else if (strncmp(t, "week:", 5) == 0) { /* set the campaign week */
            g_camp.week = atoi(t + 5);
        } else if (strncmp(t, "journal:", 8) == 0) { /* journal page bits */
            g_camp.journal = (uint32_t)strtoul(t + 8, NULL, 0);
        } else if (strncmp(t, "event:", 6) == 0) { /* force town event N (dd_town_events) */
            g_camp.event = (int8_t)atoi(t + 6);
            g_camp.event_seen = 0;
        } else if (strncmp(t, "heir:", 5) == 0) {
            for (int k = 0; k < HEIR_COUNT; k++) g_camp.heirlooms[k] = atoi(t + 5);
        } else if (strcmp(t, "won") == 0) {
            g_camp.game_won = 1;
        } else if (strncmp(t, "dd:", 3) == 0) {
            int q = atoi(t + 3) - 1;
            if (q >= 0 && q < DD_PLOT_QUESTS && !g_camp.in_dungeon) {
                Rng r;
                rng_seed(&r, 99);
                dungeon_plot(&g_camp.dungeon, &r, q);
                g_camp.dungeon.items[ITEM_FOOD] = 8;
                g_camp.dungeon.items[ITEM_TORCH] = 8;
                g_camp.in_dungeon = 1;
                scene_switch(SCENE_DUNGEON);
                frame(0);
            }
        } else if (strncmp(t, "trinket:", 8) == 0) {
            int tr = atoi(t + 8);
            if (tr >= 0 && tr < dd_trinket_count) stash_add(tr);
        } else if (strncmp(t, "give:", 5) == 0) {
            int item = atoi(t + 5);
            const char *n = strchr(t + 5, ':');
            if (item >= 0 && item < ITEM_COUNT && n) g_camp.dungeon.items[item] = (uint8_t)atoi(n + 1);
        } else if (strncmp(t, "fuzz:", 5) == 0) {
            static const uint32_t keys[] = {BTN_CROSS, BTN_CROSS, BTN_CROSS, BTN_CIRCLE, BTN_LEFT, BTN_RIGHT,
                                            BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_SQUARE, BTN_TRIANGLE, BTN_SELECT, BTN_START};
            uint32_t r = 2463534242u;
            for (int k = atoi(t + 5); k > 0 && !game_quit_requested(); k--) {
                r ^= r << 13; r ^= r >> 17; r ^= r << 5;
                frame((r >> 8) % 3 ? 0 : keys[r % (sizeof(keys) / sizeof(keys[0]))]);
            }
        } else if (strncmp(t, "hold:", 5) == 0) {
            char name[8] = {0};
            int n = 0;
            sscanf(t + 5, "%7[^:]:%d", name, &n);
            for (int k = 0; k < n; k++) frame(button(name));
            frame(0);
        } else {
            uint32_t b = button(t);
            if (!b) fprintf(stderr, "unknown token %s\n", t);
            frame(b);
            frame(b);
            frame(0);
            frame(0);
        }
    }
    game_shutdown();
    return 0;
}
