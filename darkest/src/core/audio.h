/*
 * audio.h - sound effects and music.  Portable half: names -> files.
 * Assets: assets/audio/index.txt (one sample per line: "s name" or "m name"), sfx/<name>.ogg,
 * music/<name>.ogg (made by tools/export_audio.py).  The platform decodes and mixes (plat_snd_*).
 */
#ifndef DD_AUDIO_H
#define DD_AUDIO_H

void audio_init(void);
/* Play a sample by name.  A name that has numbered variants ("x_01", "x_02"...) picks one at
 * random.  Unknown names are ignored. */
void audio_sfx(const char *name);
/* Between begin/end, audio_sfx and the skill/voice helpers only load samples into the cache (battle start). */
void audio_preload_begin(void);
void audio_preload_end(void);
/* True when a sample (or a variant of it) exists. */
int audio_has(const char *name);
/* Combat voices.  mid = monster id ("bone_soldier_A" style), kind = hurt/death/aggro/crit. */
void audio_monster(const char *mid, const char *kind);
/* Fuzzy prefix lookup for props: audio_sfx_like("prop_trap_", "poison_cloud"). */
void audio_sfx_like(const char *prefix, const char *id);
/* Monster skill sound by monster id and skill id. */
void audio_monster_skill(const char *mid, const char *skill, int miss);
/* Hero skill sound by class id and skill id; miss picks the "_miss" variant. */
void audio_hero_skill(const char *cls, const char *skill, int miss);
/* scene 0 title, 1 town, 2 raid; dungeon = DG_*, battle = in a fight. */
void audio_scene_music(int scene, int dungeon, int battle);
void audio_dungeon_ambience(int dungeon, int torch);
/* Narrator: plays a random variant of vo_narr_<name> if the narrator is free, the cooldown has
 * passed and the chance (percent) rolls.  Names are sample-name prefixes (see index.txt "v" lines). */
void audio_narrate(const char *name, int chance);
/* Same, but only if no line of that group was spoken yet this raid ("...first" lines). */
void audio_narrate_once(const char *name, int chance);
/* Same, with a short cooldown (deaths, afflictions, virtues). */
void audio_narrate_important(const char *name, int chance);
/* Forget which lines were spoken: new expedition or new town visit (each line plays once). */
void audio_narrate_reset(void);
/* Looping ambience bed by exact name ("amb_dun_ruins_base"); NULL stops. */
void audio_ambience(const char *name);
/* Music: optional intro then a looping track.  Same pair as now = no restart.  NULL loop = stop. */
void audio_music(const char *intro, const char *loop);

/* Platform side (audio_psp.c / audio_host.c).  The platform owns the ogg buffers it is handed. */
void plat_snd_init(void);
/* Starts a cached sample and returns 1; 0 = not cached, the core then loads the ogg and queues it. */
int plat_snd_sfx_cached(const char *key);
int plat_snd_has(const char *key);
void plat_snd_preload(const char *key, const void *ogg, int size);
void plat_snd_sfx(const char *key, const void *ogg, int size);
void plat_snd_voice(const void *ogg, int size);
int plat_snd_voice_busy(void);
void plat_snd_ambience(const void *ogg, int size);
void plat_snd_music(const void *intro, int intro_size, const void *loop, int loop_size);

#endif
