/*
 * audio_psp.c - PSP mixer.  One 44100 Hz stereo channel (mixed at 22050 Hz, upsampled 2x), fed by a thread that mixes up to
 * 8 sfx voices and one streamed music track.  Ogg Vorbis decoding by libvorbisfile (Tremor decoded wrong on hardware).
 * sfx are decoded whole on first use and cached (small); music is decoded as it plays.
 */
#include "audio.h"
#include "platform.h"
#include <pspaudio.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <vorbis/vorbisfile.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 22050
#define BLOCK 512
#define NVOICE 8
#define CACHE_N 192
#define CACHE_BYTES (4 * 1024 * 1024)
#define SFX_VOL 0x6000
#define MUS_VOL 0x2000
#define AMB_VOL 0x0800
#define VOX_VOL 0x5000

typedef struct { const int16_t *pcm; int len; volatile int pos; volatile int active; } Voice;
typedef struct { char key[48]; int16_t *pcm; int len; } Cached;

static Voice voices[NVOICE];
static Cached cache[CACHE_N];
static int ncache, cache_bytes, next_voice;
static int chan = -1;

/* music */
typedef struct { const unsigned char *d; int size, pos; } Mem;
static size_t mem_read(void *p, size_t sz, size_t n, void *src) {
    Mem *m = src;
    size_t want = sz * n, left = m->size - m->pos;
    if (want > left) want = left;
    memcpy(p, m->d + m->pos, want);
    m->pos += want;
    return want / sz;
}
static int mem_seek(void *src, ogg_int64_t off, int whence) {
    Mem *m = src;
    long np = whence == SEEK_SET ? off : whence == SEEK_CUR ? m->pos + off : m->size + off;
    if (np < 0 || np > m->size) return -1;
    m->pos = np;
    return 0;
}
static long mem_tell(void *src) { return ((Mem *)src)->pos; }
static const ov_callbacks cb = { mem_read, mem_seek, NULL, mem_tell };

typedef struct { void *data; Mem mem; OggVorbis_File vf; int open; } Stream;
static Stream track[2];            /* [0] intro, [1] loop */
static volatile int mus_cur = -1;  /* index of the stream playing, -1 none */
static volatile int mus_gen;       /* bumped when the game swaps tracks */
static volatile int mus_pending;
static void *new_data[2]; static int new_size[2];
static SceUID lock;
static Stream vox;                 /* narrator line, plays once */
static void *new_vox; static int new_vox_size; static volatile int vox_gen;
static Stream amb;                 /* looping ambience bed */
static void *new_amb; static int new_amb_size; static volatile int amb_gen;

static void stream_free(Stream *s) {
    if (s->open) ov_clear(&s->vf);
    free(s->data);
    memset(s, 0, sizeof *s);
}
static int stream_open(Stream *s, void *d, int size) {
    memset(s, 0, sizeof *s);
    if (!d) return -1;
    s->data = d; s->mem.d = d; s->mem.size = size;
    if (ov_open_callbacks(&s->mem, &s->vf, NULL, 0, cb) < 0) { free(d); s->data = NULL; return -1; }
    s->open = 1;
    { vorbis_info *vi = ov_info(&s->vf, -1); plat_log("audio: stream %d bytes, %ld Hz, %d ch, %lld frames", size, vi ? vi->rate : 0, vi ? vi->channels : 0, (long long)ov_pcm_total(&s->vf, -1)); }
    return 0;
}

/* Read n mono frames from the music stream into out; returns frames produced (0 = ended). */
static int stream_read(Stream *s, int16_t *out, int n) {
    int got = 0, bs;
    while (got < n) {
        long r = ov_read(&s->vf, (char *)(out + got), (n - got) * 2, 0, 2, 1, &bs);
        if (r <= 0) break;
        got += r / 2;
    }
    { /* diagnostics: first blocks of each stream */
        static int logged;
        if (logged < 6 && got > 16) {
            int pk = 0;
            long long e = 0;
            for (int i = 0; i < got; i++) { int v = out[i]; e += (long long)v * v; if (v < 0) v = -v; if (v > pk) pk = v; }
            plat_log("audio: raw decode n=%d peak=%d rms=%d first: %d %d %d %d %d %d %d %d", got, pk, (int)__builtin_sqrt((double)e / got),
                     out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
            logged++;
        }
    }
    return got;
}

static int thread_main(SceSize a, void *p) {
    (void)a; (void)p;
    static int16_t mus[BLOCK];
    static int16_t outbuf[3][BLOCK * 4] __attribute__((aligned(64)));   /* 44100 Hz stereo: each 22050 Hz frame twice; rotated so the hardware never reads a buffer being refilled */
    int ob = 0;
    int prev = 0, nblk = 0, slow = 0;
    uint32_t tmax = 0, tsum = 0;
    static int16_t ab[BLOCK];
    int gen = 0, agen = 0, vgen = 0, fade = 0;
    for (;;) {
        uint32_t t0 = sceKernelGetSystemTimeLow();
        int16_t *out = outbuf[ob];
        if (vgen != vox_gen) {
            sceKernelWaitSema(lock, 1, NULL);
            stream_free(&vox);
            if (new_vox) stream_open(&vox, new_vox, new_vox_size);
            new_vox = NULL;
            vgen = vox_gen;
            sceKernelSignalSema(lock, 1);
        }
        if (agen != amb_gen) {
            sceKernelWaitSema(lock, 1, NULL);
            stream_free(&amb);
            if (new_amb) stream_open(&amb, new_amb, new_amb_size);
            new_amb = NULL;
            agen = amb_gen;
            sceKernelSignalSema(lock, 1);
        }
        if (gen != mus_gen) {   /* swap in new tracks (game thread filled new_data) */
            sceKernelWaitSema(lock, 1, NULL);
            stream_free(&track[0]); stream_free(&track[1]);
            if (new_data[0]) stream_open(&track[0], new_data[0], new_size[0]);
            if (new_data[1]) stream_open(&track[1], new_data[1], new_size[1]);
            new_data[0] = new_data[1] = NULL;
            mus_cur = track[0].open ? 0 : track[1].open ? 1 : -1;
            gen = mus_gen;
            fade = 0;
            sceKernelSignalSema(lock, 1);
        }
        int mn = 0, tries = 0;
        while (mus_cur >= 0 && mn < BLOCK && tries < 4) {
            Stream *s = &track[mus_cur];
            int g = stream_read(s, mus + mn, BLOCK - mn);
            mn += g;
            if (mn < BLOCK) {   /* ended: intro -> loop, loop -> restart */
                tries++;
                if (mus_cur == 0 && track[1].open) mus_cur = 1;
                else if (mus_cur == 1) ov_pcm_seek(&s->vf, 0);
                else mus_cur = -1;
            }
        }
        for (int i = mn; i < BLOCK; i++) mus[i] = 0;
        int32_t mix[BLOCK];
        if (fade < 43) fade++;   /* ~1 s fade-in of a new track */
        for (int i = 0; i < BLOCK; i++) mix[i] = mus[i] * (MUS_VOL * fade / 43) / 0x8000;
        if (amb.open) {
            int an = 0, atry = 0;
            while (an < BLOCK && atry < 3) {
                int g = stream_read(&amb, ab + an, BLOCK - an);
                an += g;
                if (an < BLOCK) { atry++; ov_pcm_seek(&amb.vf, 0); }
            }
            for (int i = 0; i < an; i++) mix[i] += ab[i] * AMB_VOL / 0x8000;
        }
        if (vox.open) {
            int g = stream_read(&vox, ab, BLOCK);
            for (int i = 0; i < g; i++) mix[i] += ab[i] * VOX_VOL / 0x8000;
            if (g < BLOCK) stream_free(&vox);
        }
        for (int v = 0; v < NVOICE; v++) {
            Voice *vo = &voices[v];
            if (!vo->active) continue;
            int pos = vo->pos, n = vo->len - pos;
            if (n > BLOCK) n = BLOCK;
            for (int i = 0; i < n; i++) mix[i] += vo->pcm[pos + i] * SFX_VOL / 0x8000;
            vo->pos = pos + n;
            if (vo->pos >= vo->len) vo->active = 0;
        }
        for (int i = 0; i < BLOCK; i++) {
            int m = mix[i];
            if (m > 22000) m = 22000 + (m - 22000) / 4; else if (m < -22000) m = -22000 + (m + 22000) / 4;   /* soft knee */
            if (m > 32767) m = 32767; else if (m < -32768) m = -32768;
            int h = (prev + m) / 2;   /* linear 2x upsample */
            prev = m;
            out[i * 4] = out[i * 4 + 1] = (int16_t)h;
            out[i * 4 + 2] = out[i * 4 + 3] = (int16_t)m;
        }
        {
            uint32_t dt = sceKernelGetSystemTimeLow() - t0;   /* mix time before blocking output */
            if (dt > tmax) tmax = dt;
            tsum += dt;
            if (dt > 23000) slow++;
            if (++nblk == 215) {   /* ~5 s */
                int pk = 0;
                for (int i = 0; i < BLOCK; i++) { int v = out[i * 4 + 2]; if (v < 0) v = -v; if (v > pk) pk = v; }
                plat_log("audio: 215 blocks, mix avg %u us, max %u us, %d slow(>23ms), peak %d, mus_cur %d", (unsigned)(tsum / 215), (unsigned)tmax, slow, pk, (int)mus_cur);
                nblk = 0; tmax = 0; tsum = 0; slow = 0;
            }
        }
        sceAudioOutputBlocking(chan, PSP_AUDIO_VOLUME_MAX, out);
        ob = (ob + 1) % 3;
    }
    return 0;
}

static SceUID clock_sema, queue_sema;
#define QN 256
static struct { char key[48]; void *ogg; int size; uint32_t t; int play; } queue[QN];
static int q_head, q_tail;
static int loader_main(SceSize a, void *p);

void plat_snd_init(void) {
    chan = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, BLOCK * 2, PSP_AUDIO_FORMAT_STEREO);   /* 1024 frames @ 44100 */
    if (chan < 0) { plat_log("audio: reserve failed %x", chan); return; }
    lock = sceKernelCreateSema("sndlock", 0, 1, 1, NULL);
    clock_sema = sceKernelCreateSema("sndcache", 0, 1, 1, NULL);
    queue_sema = sceKernelCreateSema("sndq", 0, 0, QN, NULL);
    int lt = sceKernelCreateThread("sndload", loader_main, 0x1F, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (lt >= 0) sceKernelStartThread(lt, 0, NULL);
    int th = sceKernelCreateThread("audio", thread_main, 0x12, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

/* sfx cache + loader thread: decoding an Ogg takes several ms, so it never happens on the game
 * thread.  The game thread only looks the sample up (cheap) or queues the compressed bytes. */

static void cache_flush(void) {   /* clock_sema held */
    for (int i = 0; i < ncache; i++) {
        int used = 0;
        for (int v = 0; v < NVOICE; v++) if (voices[v].active && voices[v].pcm == cache[i].pcm) used = 1;
        if (used) continue;
        free(cache[i].pcm);
        cache_bytes -= cache[i].len * 2;
        cache[i] = cache[--ncache];
        i--;
    }
}

static int16_t *decode_sfx(const void *ogg, int size, long *len) {
    Mem m = { ogg, size, 0 };
    OggVorbis_File vf;
    if (ov_open_callbacks(&m, &vf, NULL, 0, cb) < 0) return NULL;
    long frames = (long)ov_pcm_total(&vf, -1);
    if (frames <= 0 || frames > RATE * 6) { ov_clear(&vf); return NULL; }
    int16_t *pcm = malloc(frames * 2);
    long got = 0; int bs;
    while (pcm && got < frames) {
        long r = ov_read(&vf, (char *)(pcm + got), (frames - got) * 2, 0, 2, 1, &bs);
        if (r <= 0) break;
        got += r / 2;
    }
    ov_clear(&vf);
    if (!pcm || got <= 0) { free(pcm); return NULL; }
    *len = got;
    return pcm;
}

static void start_voice(const int16_t *pcm, int len) {
    Voice *v = NULL;
    for (int i = 0; i < NVOICE; i++) if (!voices[i].active) { v = &voices[i]; break; }
    if (!v) v = &voices[next_voice++ % NVOICE];   /* steal */
    v->active = 0;
    v->pcm = pcm; v->len = len; v->pos = 0;
    __sync_synchronize();
    v->active = 1;
}

/* Cached sample: start it now.  Returns 1 if it was cached. */
int plat_snd_sfx_cached(const char *key) {
    if (chan < 0) return 1;
    const int16_t *pcm = NULL;
    int len = 0;
    sceKernelWaitSema(clock_sema, 1, NULL);
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].key, key)) { pcm = cache[i].pcm; len = cache[i].len; break; }
    if (pcm) start_voice(pcm, len);   /* under the lock so a flush can't free it first */
    sceKernelSignalSema(clock_sema, 1);
    return pcm != NULL;
}

static int loader_main(SceSize a, void *p) {
    (void)a; (void)p;
    for (;;) {
        sceKernelWaitSema(queue_sema, 1, NULL);
        sceKernelWaitSema(clock_sema, 1, NULL);
        if (q_head == q_tail) { sceKernelSignalSema(clock_sema, 1); continue; }
        char key[48]; void *ogg = queue[q_head].ogg; int size = queue[q_head].size; uint32_t t = queue[q_head].t; int play = queue[q_head].play;
        snprintf(key, sizeof key, "%s", queue[q_head].key);
        q_head = (q_head + 1) % QN;
        sceKernelSignalSema(clock_sema, 1);
        sceKernelChangeThreadPriority(0, play ? 0x1F : 0x28);   /* preloads run in the background, below the game thread */
        long len = 0;
        int16_t *pcm = decode_sfx(ogg, size, &len);
        free(ogg);
        if (!pcm) continue;
        sceKernelWaitSema(clock_sema, 1, NULL);
        for (int i = 0; i < ncache; i++) {   /* loaded twice in the meantime (preload + play) */
            if (strcmp(cache[i].key, key)) continue;
            if (play && sceKernelGetSystemTimeLow() - t < 1000000) start_voice(cache[i].pcm, cache[i].len);
            free(pcm); pcm = NULL;
            break;
        }
        if (!pcm) { sceKernelSignalSema(clock_sema, 1); continue; }
        if (ncache >= CACHE_N || cache_bytes > CACHE_BYTES) cache_flush();
        int fresh = play && sceKernelGetSystemTimeLow() - t < 1000000;   /* very late = skip the sound */
        if (ncache < CACHE_N) {
            Cached *c = &cache[ncache++];
            snprintf(c->key, sizeof c->key, "%s", key);
            c->pcm = pcm; c->len = len;
            cache_bytes += len * 2;
            if (fresh) start_voice(pcm, len);
        } else {
            if (fresh) { /* no room to cache: play from a one-shot buffer that the mixer frees via cache */ }
            free(pcm);
        }
        sceKernelSignalSema(clock_sema, 1);
    }
    return 0;
}

static void enqueue(const char *key, const void *ogg, int size, int play);
void plat_snd_sfx(const char *key, const void *ogg, int size) { enqueue(key, ogg, size, 1); }
void plat_snd_preload(const char *key, const void *ogg, int size) { enqueue(key, ogg, size, 0); }
int plat_snd_has(const char *key) {
    int has = 0;
    sceKernelWaitSema(clock_sema, 1, NULL);
    for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].key, key)) { has = 1; break; }
    sceKernelSignalSema(clock_sema, 1);
    return has;
}

static void enqueue(const char *key, const void *ogg, int size, int play) {
    if (chan < 0) { free((void *)ogg); return; }
    sceKernelWaitSema(clock_sema, 1, NULL);
    int next = (q_tail + 1) % QN;
    if (next == q_head) { sceKernelSignalSema(clock_sema, 1); free((void *)ogg); return; }   /* queue full */
    snprintf(queue[q_tail].key, sizeof queue[q_tail].key, "%s", key);
    queue[q_tail].ogg = (void *)ogg; queue[q_tail].size = size; queue[q_tail].t = sceKernelGetSystemTimeLow(); queue[q_tail].play = play;
    q_tail = next;
    sceKernelSignalSema(clock_sema, 1);
    sceKernelSignalSema(queue_sema, 1);
}

void plat_snd_music(const void *intro, int is, const void *loop, int ls) {
    if (chan < 0) { free((void *)intro); free((void *)loop); return; }
    sceKernelWaitSema(lock, 1, NULL);
    free(new_data[0]); free(new_data[1]);   /* a swap the thread hasn't taken yet */
    new_data[0] = (void *)intro; new_size[0] = is;
    new_data[1] = (void *)loop; new_size[1] = ls;
    mus_gen++;
    sceKernelSignalSema(lock, 1);
}

void plat_snd_ambience(const void *ogg, int size) {
    if (chan < 0) { free((void *)ogg); return; }
    sceKernelWaitSema(lock, 1, NULL);
    free(new_amb);
    new_amb = (void *)ogg; new_amb_size = size;
    amb_gen++;
    sceKernelSignalSema(lock, 1);
}

void plat_snd_voice(const void *ogg, int size) {
    if (chan < 0) { free((void *)ogg); return; }
    sceKernelWaitSema(lock, 1, NULL);
    free(new_vox);
    new_vox = (void *)ogg; new_vox_size = size;
    vox_gen++;
    sceKernelSignalSema(lock, 1);
}
int plat_snd_voice_busy(void) { return vox.open || new_vox != NULL; }
