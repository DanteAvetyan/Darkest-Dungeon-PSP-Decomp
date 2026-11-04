#include "audio.h"
#include "pack.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SND 2048
static char *names;                /* index.txt text, split in place */
static const char *snd[MAX_SND];   /* sfx names */
static int nsnd;
static const char *mus[64];
static int nmus;
static const char *vox[512];
static int nvox;
static uint32_t rnd = 12345;
static char cur_intro[64], cur_loop[64];

void audio_init(void) {
    int sz = 0;
    char *t = asset_read("assets/audio/index.txt", &sz);
    if (t) {
        names = realloc(t, sz + 1);
        names[sz] = 0;
        for (char *p = names; *p;) {
            char *e = strchr(p, '\n');
            if (e) *e = 0;
            if ((p[0] == 's' || p[0] == 'm' || p[0] == 'v') && p[1] == ' ') {
                if (p[0] == 's' && nsnd < MAX_SND) snd[nsnd++] = p + 2;
                else if (p[0] == 'm' && nmus < 64) mus[nmus++] = p + 2;
                else if (p[0] == 'v' && nvox < 512) vox[nvox++] = p + 2;
            }
            if (!e) break;
            p = e + 1;
        }
    }
    plat_snd_init();
    plat_log("audio: %d sfx, %d music", nsnd, nmus);
}

/* base == name, or base == name + "_" + digits */
static int match(const char *base, const char *name) {
    size_t n = strlen(name);
    if (strncmp(base, name, n) != 0) return 0;
    if (!base[n] || !strncmp(base + n, " (", 2)) return 1;
    if (base[n] != '_' || !base[n + 1]) return 0;
    for (const char *p = base + n + 1; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static const char *pick(const char *name) {
    const char *hit[16];
    int n = 0;
    for (int i = 0; i < nsnd && n < 16; i++) if (match(snd[i], name)) hit[n++] = snd[i];
    if (!n) return NULL;
    rnd = rnd * 1664525u + 1013904223u;
    return hit[(rnd >> 16) % n];
}

int audio_has(const char *name) { return pick(name) != NULL; }

static int preloading;
void audio_preload_begin(void) { preloading = 1; }
void audio_preload_end(void) { preloading = 0; }

/* Preload mode: load every variant of the sample into the platform cache instead of playing it. */
static void preload_all(const char *name) {
    for (int i = 0; i < nsnd; i++) {
        if (!match(snd[i], name) || plat_snd_has(snd[i])) continue;
        char path[128];
        snprintf(path, sizeof path, "assets/audio/sfx/%s.ogg", snd[i]);
        int sz = 0;
        void *d = asset_read(path, &sz);
        if (d) plat_snd_preload(snd[i], d, sz);
    }
}

void audio_sfx(const char *name) {
    if (preloading) { preload_all(name); return; }
    const char *s = pick(name);
    if (!s) return;
    if (plat_snd_sfx_cached(s)) return;
    char path[128];
    snprintf(path, sizeof path, "assets/audio/sfx/%s.ogg", s);
    int sz = 0;
    void *d = asset_read(path, &sz);
    if (d) plat_snd_sfx(s, d, sz);   /* platform keeps or frees it */
}

static void *load_mus(const char *n, int *sz) {
    if (!n || !*n) return NULL;
    for (int i = 0; i < nmus; i++) {
        if (strncmp(mus[i], n, strlen(n)) == 0) {
            char path[128];
            snprintf(path, sizeof path, "assets/audio/music/%s.ogg", mus[i]);
            return asset_read(path, sz);
        }
    }
    return NULL;
}

void audio_music(const char *intro, const char *loop) {
    const char *i = intro ? intro : "", *l = loop ? loop : "";
    if (!strcmp(cur_intro, i) && !strcmp(cur_loop, l)) return;
    snprintf(cur_intro, sizeof cur_intro, "%s", i);
    snprintf(cur_loop, sizeof cur_loop, "%s", l);
    int is = 0, ls = 0;
    void *a = load_mus(intro, &is), *b = load_mus(loop, &ls);
    plat_snd_music(a, is, b, ls);
}

/* Monster id prefix -> char_en_vo_<name>.  Longest matching prefix wins. */
static const struct { const char *prefix, *vo; } mon_vo[] = {
    {"ancestor_heart", "ancestor_heart"}, {"ancestor_pod", "ancestor_pod"}, {"ancestor_small", "ancestor_small"},
    {"brigand_cannon", "brigcannon"}, {"brigand_sapper", "sapper"}, {"brigand", "brig_1"},
    {"bloated_corpse", "zombie_1"}, {"carrion_eater_big", "carrionlarge"}, {"carrion_eater", "carrionsmall"},
    {"cauldron", "cauldron_full"}, {"cell_white", "white_cell"}, {"cell", "white_cell"}, {"collector", "collector"},
    {"crone", "crone"}, {"crow", "crow"}, {"cultist_brawler", "cultist_m"}, {"cultist_harpy", "cultist_f"},
    {"cultist_orgiastic", "cultist_orgiastic"}, {"cultist_shrouded", "cultistshrouded"},
    {"cultist_warlord", "cultist_m"}, {"cultist_witch", "cultist_f"}, {"cyst", "cyst"},
    {"drowned_captain", "drowncap"}, {"drowned", "drowncap"}, {"errant_flesh_bat", "fleshbat"},
    {"errant_flesh_dog", "fleshdog"}, {"fishman_shaman", "fish_2"}, {"fishman", "fish_1"},
    {"formless", "form"}, {"fungal", "slime_1"}, {"gargoyle", "garg"}, {"ghoul", "garg"}, {"hag", "hag"},
    {"madman", "madman"}, {"maggot", "bug_1"}, {"necromancer", "nec"}, {"nest", "nest"},
    {"pew", "pew"}, {"prophet", "prophet"}, {"rabid_dog", "rbdog"}, {"shambler_tentacle", "shamtent"},
    {"shambler", "shambler"}, {"siren", "siren"}, {"skeleton_arbalist", "skel"}, {"skeleton_bearer", "skelbearer"},
    {"skeleton_captain", "sklcap"}, {"skeleton_courtier", "sklbis"}, {"skeleton", "skel"},
    {"spider", "bug_2"}, {"swine_prince", "swine_3"}, {"swine_", "swine_1"}, {"swinetaur", "swine_2"},
    {"templar", "templar"}, {"totem_attack", "totemattack"}, {"unclean_giant", "uncgi"},
    {"virago_shroom", "viragoshroom"}, {"virago", "virago"}, {"totem_guard", "totemattack"}, {"ectoplasm", "slime_2"}, {"jellyfish", "slime_2"},
    {"octotank", "slime_1"}, {"snail", "slime_1"}, {"corpse", "zombie_2"},
};

/* kind: "hurt", "death", "aggro", "crit", "idle" ... */
void audio_monster(const char *mid, const char *kind) {
    size_t best = 0;
    const char *vo = NULL;
    for (unsigned i = 0; i < sizeof mon_vo / sizeof *mon_vo; i++) {
        size_t n = strlen(mon_vo[i].prefix);
        if (n > best && !strncmp(mid, mon_vo[i].prefix, n)) { best = n; vo = mon_vo[i].vo; }
    }
    if (!vo) return;
    char nm[64];
    snprintf(nm, sizeof nm, "char_en_vo_%s_%s", vo, kind);
    audio_sfx(nm);
}

/* Hero skill sound: char_al_<abbr>_<skill>[_miss].  Skill ids and sample names differ in
 * underscores and length ("zealous_accusation" / "zealousacc"), so compare squashed prefixes. */
static const struct { const char *cls, *abbr; } hero_abbr[] = {
    {"crusader", "cru"}, {"arbalest", "arb"}, {"abomination", "abom"}, {"antiquarian", "antq"},
    {"bounty_hunter", "bhr"}, {"grave_robber", "grv"}, {"hellion", "hll"}, {"highwayman", "hwy"},
    {"houndmaster", "hnd"}, {"jester", "jest"}, {"leper", "leper"}, {"man_at_arms", "maa"},
    {"occultist", "occ"}, {"plague_doctor", "plg"}, {"vestal", "vst"},
};

static void squash(char *o, const char *s, int max) {
    int n = 0;
    for (; *s && n < max - 1; s++) if (*s != '_') o[n++] = *s;
    o[n] = 0;
}

/* Skill ids whose sample names are too abbreviated for the fuzzy match. */
static const struct { const char *skill, *sample; } skill_alias[] = {
    {"holy_lance", "char_al_cry_holylance"}, {"gods_illumination", "char_al_vst_illum"},
    {"hook_and_slice", "char_al_bhr_hookslice"}, {"barbaric_yawp", "char_al_hll_barbyawp"},
    {"adrenaline_rush", "char_al_hll_adrush"}, {"chop", "char_al_leper_chop"}, {"dirk_stab", "char_al_jest_dirk"},
    {"festering_vapours", "char_al_antq_festvap"}, {"fortifying_vapours", "char_al_antq_fortvap"},
    {"brawler_claw", "char_en_cultist_m_claw"}, {"brawler_claw_D", "char_en_cultist_warlord_claw"}, {"totem_stun", "char_en_totemattack_weaken"}, {"take_aim", "char_al_arb_sniper_mark"}, {"brawler_claw_weak", "char_en_cultist_m_claw"},
    {"spider_web", "char_en_spidspit_spidweb"}, {"spider_bite", "char_en_spidspit_spidbite"},
    {"spider_spit", "char_en_spidspit_spidspit"}, {"slime_lg", "char_en_ectolg_slime"},
    {"light_fuse", "char_en_fuseman_light"}, {"peck", "en_crow_peck"}, {"flutter", "en_crow_flutter"},
    {"distract", "en_crow_distract"}, {"caw", "en_crow_caw"}, {"hook_where_it_hurts", "char_en_swineslasher_hook"},
    {"formless_melee_attack", "char_en_formmelee_attack"}, {"formless_weak_heal", "char_en_formweak_heal"},
    {"formless_guard_attack", "char_en_formguard_attack"}, {"formless_ranged_attack", "char_en_formranged_attack"},
    {"pig_spear", "char_en_swinetaur_spear"}, {"trot_retreat", "char_en_swinetaur_trot"},
    {"fish_cutlass", "char_en_fishharp_cut"}, {"fish_stresscast", "char_en_fishsham_stress"},
    {"call_of_the_deep", "char_en_fishsham_call"}, {"shaman_stab", "char_en_fishsham_stab"},
    {"shaman_heal", "char_en_fishsham_heal"}, {"snail_slime", "char_en_snailurch_slime"},
    {"jellyfish_sting", "char_en_jelly_sting"}, {"jellyfish_rend", "char_en_jelly_rend"},
    {"captain_shout", "char_en_drowncap_shout"}, {"captain_shout_debuff", "char_en_drowncap_shoutdebuff"},
    {"the_pinch", "char_en_fishcrab_pinch"}, {"totem_maul", "char_en_totemattack_maul"},
    {"totem_bolster", "char_en_totemguard_bolster"}, {"totem_heal", "char_en_totemguard_heal"},
    {"totem_stressblast", "char_en_totemguard_stress"}, {"errant_spit", "char_en_fleshbat_spit"},
    {"errant_cough", "char_en_fleshbat_cough"}, {"errant_push", "char_en_fleshbat_push"},
    {"errant_bite", "char_en_fleshdog_bite"}, {"errant_nip", "char_en_fleshdog_nip"},
    {"errant_pull", "char_en_fleshdog_pull"}, {"cell_reconstruct", "char_en_cellwhite_reconstruct"},
    {"cell_shuffle", "char_en_cellwhite_shuffle"}, {"beholder_gaze", "char_en_cyst_gaze"},
    {"beholder_burn", "char_en_cyst_burn"}, {"beholder_healself", "char_en_cyst_healself"},
    {"beholder_summon", "char_en_cyst_summon"},
    {"obliterate_marked_one", "char_en_swineprince_marked"}, {"obliterate_marked_two", "char_en_swineprince_markedtwo"},
    {"obliterate_enraged", "char_en_swineprince_enrage"}, {"obliterate_blind", "char_en_swineprince_blind"},
    {"conch_summon", "char_en_siren_summon"}, {"conch_horror", "char_en_siren_horror"},
    {"conch_celldisrupt", "char_en_siren_celldisrupt"}, {"eye_on_you", "char_en_prophet_eye"},
    {"head_guard", "char_en_collecthead_protect"}, {"embrace_the_dark", "char_en_crone_howl"},
    {"darkest_eye", "char_en_templar_eye"}, {"cell_teleport", "char_en_cellwhite_shuffle"},
    {"kill_pill", "char_en_ancestor_pod_die"}, {"rushed_shot", "char_en_brigfus_blunderbuuss"},
    {"bolster", "char_maa_bolster"}, {"battlefield_bandage", "char_al_arb_bandage"},
    {"if_it_bleeds", "char_al_hll_bleedout"}, {"heroic_end", "char_al_jest_finale"},
};

static int play_alias(const char *skill, int miss) {
    for (unsigned i = 0; i < sizeof skill_alias / sizeof *skill_alias; i++) {
        if (strcmp(skill_alias[i].skill, skill)) continue;
        char nm[96];
        snprintf(nm, sizeof nm, "%s%s", skill_alias[i].sample, miss ? "_miss" : "");
        audio_sfx(nm);
        return 1;
    }
    return 0;
}

void audio_hero_skill(const char *cls, const char *skill, int miss) {
    if (play_alias(skill, miss)) return;
    const char *ab = NULL;
    for (unsigned i = 0; i < sizeof hero_abbr / sizeof *hero_abbr; i++)
        if (!strcmp(cls, hero_abbr[i].cls)) ab = hero_abbr[i].abbr;
    if (!ab) return;
    char pre[32], key[48], rest[64];
    snprintf(pre, sizeof pre, "char_al_%s_", ab);
    squash(key, skill, sizeof key);
    size_t pl = strlen(pre), kl = strlen(key);
    const char *best = NULL;
    size_t bestl = 0;
    for (int i = 0; i < nsnd; i++) {
        const char *n = snd[i];
        if (strncmp(n, pre, pl)) continue;
        int ism = strlen(n) > 5 && !strcmp(n + strlen(n) - 5, "_miss");
        if (ism != (miss != 0)) continue;
        squash(rest, n + pl, sizeof rest);
        if (ism) rest[strlen(rest) - 4] = 0;
        size_t rl = strlen(rest), c = 0;
        while (c < rl && c < kl && rest[c] == key[c]) c++;
        if (c >= 5 || (c == rl && c == kl)) { if (c > bestl) { bestl = c; best = n; } }
    }
    if (best) audio_sfx(best);
}

/* Per-scene music choice.  Names are prefixes of the files in assets/audio/music. */
void audio_scene_music(int scene, int dungeon, int battle) {
    static const char *const explore[] = {"Explore_Vaults_Level_1_Loop2", "Mournweald_LEVEL1_LOOP1", NULL, NULL, "Explore_Vaults_Level_1_Loop2"};
    static const char *const combat[] = {"Combat_Level1_Loop1", "mus_combat_weald_hallway", "WARRENS_Combat_LOOP1_LEVEL1",
                                         "mus_combat_cove_lvl1_loop1", "mus_combat_dd_lvl1_loop1"};
    if (scene == 0) { audio_music("mus_theme_intro_v2", "mus_theme_loop"); audio_ambience(NULL); }
    else if (scene == 1) { audio_music(NULL, "Town_Stereo_Mix_LOOP_1"); audio_ambience("amb_town_gen_base"); }
    else if (dungeon < 0 || dungeon > 4) audio_music(NULL, NULL);
    else if (battle) audio_music(NULL, combat[dungeon]);
    else audio_music(NULL, explore[dungeon]);
}

static int subseq(const char *small, const char *big) {
    if (*small != *big) return 0;
    for (; *big && *small; big++) if (*big == *small) small++;
    return !*small;
}

/* Monster skill sound char_en_<mon>_<skill>[_miss] (abbreviated names): best fuzzy match of the
 * skill id inside the squashed sample name, ties broken by the monster token. */
void audio_monster_skill(const char *mid, const char *skill, int miss) {
    char key[48], rest[64], mon[64], tok[32];
    if (play_alias(skill, miss)) return;
    squash(key, skill, sizeof key);
    squash(mon, mid, sizeof mon);
    size_t kl = strlen(key);
    const char *best = NULL;
    int bests = 0;
    for (int i = 0; i < nsnd; i++) {
        const char *n = snd[i];
        if (strncmp(n, "char_en_", 8) || !strncmp(n, "char_en_vo_", 11)) continue;
        int ism = strlen(n) > 5 && !strcmp(n + strlen(n) - 5, "_miss");
        if (ism != (miss != 0)) continue;
        squash(rest, n + 8, sizeof rest);
        if (ism) rest[strlen(rest) - 4] = 0;
        int k = (int)kl;
        for (; k >= 4; k--) { char sv = key[k]; key[k] = 0; char *f = strstr(rest, key); key[k] = sv; if (f) break; }
        if (k < (int)(kl < 6 ? kl : 6)) continue;
        const char *us = strchr(n + 8, '_');
        size_t tl = us ? (size_t)(us - (n + 8)) : 0;
        if (tl >= sizeof tok) tl = sizeof tok - 1;
        memcpy(tok, n + 8, tl); tok[tl] = 0;
        int score = k * 10 + (tl && subseq(tok, mon) ? 100 : 0);
        if (score > bests) { bests = score; best = n; }
    }
    if (best) audio_sfx(best);
}

/* Play the sample "<prefix><x>" where x shares a >=5 char squashed prefix with id ("poison_cloud"
 * ~ "poisoncloud"); the shortest such sample wins (skips "_dodge", "_smash" variants). */
void audio_sfx_like(const char *prefix, const char *id) {
    char key[48], rest[64];
    squash(key, id, sizeof key);
    size_t pl = strlen(prefix), kl = strlen(key), need = kl < 5 ? kl : 5;
    const char *best = NULL;
    size_t bl = 999;
    for (int i = 0; i < nsnd; i++) {
        const char *n = snd[i];
        if (strncmp(n, prefix, pl)) continue;
        squash(rest, n + pl, sizeof rest);
        size_t c = 0;
        while (rest[c] && key[c] && rest[c] == key[c]) c++;
        if (c >= need && strlen(rest) < bl) { bl = strlen(rest); best = n; }
    }
    if (best) audio_sfx(best);
}

static char cur_amb[64];
void audio_ambience(const char *name) {
    const char *n = name ? name : "";
    if (!strcmp(cur_amb, n)) return;
    snprintf(cur_amb, sizeof cur_amb, "%s", n);
    void *d = NULL;
    int sz = 0;
    for (int i = 0; name && i < nmus; i++) {
        if (!strcmp(mus[i], name)) {
            char path[128];
            snprintf(path, sizeof path, "assets/audio/music/%s.ogg", mus[i]);
            d = asset_read(path, &sz);
            break;
        }
    }
    plat_snd_ambience(d, sz);
}

/* Dungeon ambience bed; the "dark" bed takes over when the torch is low. */
void audio_dungeon_ambience(int dungeon, int torch) {
    static const char *const dir[] = {"ruins", "weald", "warrens", "cove", "darkest_1"};
    if (dungeon < 0 || dungeon > 4) { audio_ambience(NULL); return; }
    char nm[48];
    snprintf(nm, sizeof nm, "amb_dun_%s_%s", dir[dungeon], torch < 26 ? "dark" : "base");
    audio_ambience(nm);
}

static uint32_t last_narr;
static const char *used[256];   /* narrator lines already spoken this raid / town visit (the game allows each once) */
static int nused;

void audio_narrate_reset(void) { nused = 0; }

static int was_used(const char *n) {
    for (int i = 0; i < nused; i++) if (used[i] == n) return 1;
    return 0;
}

/* once: plays only if no line of this group was spoken yet ("...first" lines).
 * important: bypasses the long chatter cooldown (deaths, afflictions, virtues). */
static void narrate(const char *name, int chance, int once, int important) {
    uint32_t now = plat_ticks_ms();
    if (plat_snd_voice_busy() || (last_narr && now - last_narr < (important ? 3000u : 25000u))) return;
    char pre[96];
    snprintf(pre, sizeof pre, "vo_narr_%s", name);
    const char *hit[24];
    int n = 0, any_used = 0;
    for (int i = 0; i < nvox && n < 24; i++) {
        if (!match(vox[i], pre)) continue;
        if (was_used(vox[i])) { any_used = 1; continue; }
        hit[n++] = vox[i];
    }
    if (!n || (once && any_used)) return;
    rnd = rnd * 1664525u + 1013904223u;
    if ((int)((rnd >> 8) % 100) >= chance) return;
    rnd = rnd * 1664525u + 1013904223u;
    const char *pick_n = hit[(rnd >> 16) % n];
    char path[160];
    snprintf(path, sizeof path, "assets/audio/voice/%s.ogg", pick_n);
    int sz = 0;
    void *d = asset_read(path, &sz);
    if (!d) return;
    last_narr = now;
    if (nused < 256) used[nused++] = pick_n;
    plat_snd_voice(d, sz);
}

void audio_narrate(const char *name, int chance) { narrate(name, chance, 0, 0); }
void audio_narrate_once(const char *name, int chance) { narrate(name, chance, 1, 0); }
void audio_narrate_important(const char *name, int chance) { narrate(name, chance, 0, 1); }

