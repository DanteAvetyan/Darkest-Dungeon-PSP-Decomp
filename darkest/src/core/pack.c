/*
 * pack.c - reader for assets.pak (written by tools/pack_assets.py).
 *
 * File layout (all integers little-endian):
 *   header, 16 bytes:  "DDPK" | version (=1) | entry count | index byte size
 *   index: for each entry   offset u32 | packed size u32 | real size u32 |
 *                           name length u16 | name bytes (no terminator)
 *   data:  the (zlib-compressed) file bodies at the stored offsets.
 * Entries are sorted by name so lookup is a binary search.
 * If packed size == real size the body was stored uncompressed.
 *
 * Only the index is kept in RAM; each asset is read from disk on demand.
 */
#include "pack.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "platform.h"

#define PACK_PATH "assets.pak"
#define PREFIX "assets/"

/* In-memory index record. `name` points into the shared `names` buffer. */
typedef struct { uint32_t off, packed, size; const char *name; } Entry;

static FILE *pak;
static Entry *ents;
static char *names;
static int nents, tried;

/* Read a little-endian u32 byte-by-byte (safe for unaligned data and any CPU). */
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Open the pak and build the index. Runs once, lazily, on first asset_read.
 * If the file is missing or malformed we simply leave `pak` NULL and every
 * read falls back to loose files - this is what host/dev runs use. */
static void pack_open(void) {
    tried = 1;
    uint8_t h[16];
    FILE *f = fopen(PACK_PATH, "rb");
    if (!f) return;
    if (fread(h, 1, 16, f) != 16 || memcmp(h, "DDPK", 4) || rd32(h + 4) != 1) { fclose(f); return; }
    int n = (int)rd32(h + 8);
    uint32_t ib = rd32(h + 12);
    uint8_t *idx = malloc(ib);
    ents = malloc(sizeof(Entry) * (n ? n : 1));
    names = malloc(ib); /* names are shorter than their index records: always fits */
    if (!idx || !ents || !names || fread(idx, 1, ib, f) != ib) {
        free(idx); free(ents); free(names); ents = NULL; names = NULL;
        fclose(f);
        return;
    }
    uint32_t p = 0, q = 0;
    for (int i = 0; i < n && p + 14 <= ib; i++) {
        uint16_t len = (uint16_t)(idx[p + 12] | idx[p + 13] << 8);
        if (p + 14 + len > ib) break;
        ents[i] = (Entry){rd32(idx + p), rd32(idx + p + 4), rd32(idx + p + 8), names + q};
        memcpy(names + q, idx + p + 14, len);
        names[q + len] = 0;
        q += len + 1;
        p += 14 + len;
        nents = i + 1;
    }
    free(idx);
    pak = f;
}

/* Binary search by name (entries are stored sorted). */
static const Entry *find(const char *name) {
    int lo = 0, hi = nents - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, ents[mid].name);
        if (!c) return &ents[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

/* Load one asset. Steps: look it up in the pak; allocate the output buffer
 * (plat_alloc gives 16-byte alignment, needed for PSP textures); read the
 * compressed bytes; inflate with zlib. Falls back to the loose file. */
void *asset_read(const char *path, int *size) {
    if (!tried) pack_open();
    const Entry *e = pak && !strncmp(path, PREFIX, strlen(PREFIX)) ? find(path + strlen(PREFIX)) : NULL;
    if (!e) return plat_read_file(path, size);
    uint8_t *out = plat_alloc(e->size ? (int)e->size : 1);
    uint8_t *in = e->packed == e->size ? out : malloc(e->packed ? e->packed : 1);
    int ok = out && in && fseek(pak, (long)e->off, SEEK_SET) == 0 && fread(in, 1, e->packed, pak) == e->packed;
    if (ok && in != out) {
        uLongf n = e->size;
        ok = uncompress(out, &n, in, e->packed) == Z_OK && n == e->size;
    }
    if (in != out) free(in);
    if (!ok) {
        free(out);
        return NULL;
    }
    if (size) *size = (int)e->size;
    return out;
}
