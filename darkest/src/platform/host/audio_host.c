/* PC build: no sound, but DD_AUDIO_LOG=1 prints what would play (for scripted tests). */
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>

static int logging;
void plat_snd_init(void) { logging = getenv("DD_AUDIO_LOG") != NULL; }
void plat_snd_sfx(const char *key, const void *ogg, int size) {
    if (logging) fprintf(stderr, "SFX %s\n", key);
    (void)size;
    free((void *)ogg);
}
void plat_snd_music(const void *intro, int is, const void *loop, int ls) {
    if (logging) fprintf(stderr, "MUSIC intro=%d loop=%d\n", is, ls);
    free((void *)intro);
    free((void *)loop);
}
void plat_snd_ambience(const void *ogg, int size) {
    if (logging) fprintf(stderr, "AMB %d\n", size);
    free((void *)ogg);
}
void plat_snd_voice(const void *ogg, int size) {
    if (logging) fprintf(stderr, "VOICE %d\n", size);
    free((void *)ogg);
}
int plat_snd_voice_busy(void) { return 0; }
int plat_snd_sfx_cached(const char *key) { (void)key; return 0; }
int plat_snd_has(const char *key) { (void)key; return 0; }
void plat_snd_preload(const char *key, const void *ogg, int size) { (void)key; (void)size; free((void *)ogg); }
