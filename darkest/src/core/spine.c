/*
 * spine.c - .dda loader, pose evaluation and triangle emission.
 *
 * .dda file layout (little-endian): "DDA1" header (page size, counts,
 * duration), then bones, slots, attachments, timelines, draw-order table, IK
 * constraints, FFD tracks, then (16-byte aligned) the 256-colour CLUT and the
 * 8-bit atlas pixels.  Written by tools/export_art.py from the game's Spine
 * JSON; reading it mirrors that writer field for field.
 */
#include "spine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG2RAD 0.017453292519943295f

/* ---------------------------------------------------------------- reading */

/* Bounds-checked cursor over the file bytes.  Any over-read sets `bad` and
 * yields zeros instead of crashing, so a corrupt file fails cleanly. */
typedef struct {
    const uint8_t *p, *end;
    int bad;
} Reader;

static void rd(Reader *r, void *dst, int n) {
    if (r->p + n > r->end) {
        r->bad = 1;
        memset(dst, 0, n);
        return;
    }
    memcpy(dst, r->p, n); /* memcpy: the file has unaligned floats, MIPS needs aligned loads */
    r->p += n;
}
static uint8_t rd_u8(Reader *r) { uint8_t v; rd(r, &v, 1); return v; }
static uint16_t rd_u16(Reader *r) { uint16_t v; rd(r, &v, 2); return v; }
static int16_t rd_i16(Reader *r) { int16_t v; rd(r, &v, 2); return v; }
static float rd_f32(Reader *r) { float v; rd(r, &v, 4); return v; }
static void skip(Reader *r, int n) { r->p += n; if (r->p > r->end) r->bad = 1; }

/* Simple bump allocator over one malloc block so an animation frees in one call. */
typedef struct { uint8_t *base; size_t used, cap; } Arena;
static void *arena_get(Arena *a, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (a->used + n > a->cap) return NULL;
    void *p = a->base + a->used;
    a->used += n;
    return p;
}

/* Decode the whole file.  Because parsed data is far smaller than a naive
 * per-array malloc scheme, everything is carved out of one arena block.
 * Parses into one block of `cap` bytes (0 = a safe upper bound); *used gets the bytes needed. */
static int parse(SpAnim *a, void *blob, int size, size_t cap, size_t *used) {
    memset(a, 0, sizeof(*a));
    Reader r = {blob, (const uint8_t *)blob + size, 0};
    char magic[4];
    rd(&r, magic, 4);
    if (memcmp(magic, "DDA1", 4) != 0) return -1;
    a->page_w = rd_u16(&r);
    a->page_h = rd_u16(&r);
    a->nbones = rd_u16(&r);
    a->nslots = rd_u16(&r);
    a->natts = rd_u16(&r);
    a->ntl = rd_u16(&r);
    a->duration = rd_f32(&r);
    if (a->nbones > SP_MAX_BONES || a->nslots > SP_MAX_SLOTS) return -2;

    /* The parsed form is never larger than the file plus per-item headers. */
    Arena ar = {0};
    ar.cap = cap ? cap : (size_t)size * 3 + 4096 +
             a->nbones * sizeof(SpBoneData) + a->nslots * sizeof(SpSlotData) +
             a->natts * sizeof(SpAttachment) + a->ntl * sizeof(SpTimeline);
    ar.base = malloc(ar.cap);
    if (!ar.base) return -3;
    a->mem = ar.base;

    a->bones = arena_get(&ar, a->nbones * sizeof(SpBoneData));
    for (int i = 0; i < a->nbones; i++) {
        SpBoneData *b = &a->bones[i];
        b->parent = rd_i16(&r);
        b->flags = rd_u8(&r);
        skip(&r, 1);
        b->x = rd_f32(&r); b->y = rd_f32(&r); b->rot = rd_f32(&r);
        b->sx = rd_f32(&r); b->sy = rd_f32(&r); b->len = rd_f32(&r);
    }
    a->slots = arena_get(&ar, a->nslots * sizeof(SpSlotData));
    for (int i = 0; i < a->nslots; i++) {
        SpSlotData *s = &a->slots[i];
        s->bone = rd_u16(&r);
        s->attachment = rd_i16(&r);
        s->r = rd_u8(&r); s->g = rd_u8(&r); s->b = rd_u8(&r); s->a = rd_u8(&r);
        s->additive = rd_u8(&r);
        skip(&r, 3);
    }
    a->atts = arena_get(&ar, a->natts * sizeof(SpAttachment));
    for (int i = 0; i < a->natts && !r.bad; i++) {
        SpAttachment *t = &a->atts[i];
        t->type = rd_u8(&r);
        skip(&r, 1);
        t->slot = rd_u16(&r);
        t->nverts = rd_u16(&r);
        t->ntris = rd_u16(&r);
        t->uv = arena_get(&ar, t->nverts * 2 * sizeof(float));
        if (t->type == SP_REGION) {
            for (int k = 0; k < 7; k++) t->region[k] = rd_f32(&r);
            for (int k = 0; k < 8; k++) t->uv[k] = rd_f32(&r);
            continue;
        }
        if (t->type == SP_MESH) {
            t->xy = arena_get(&ar, t->nverts * 2 * sizeof(float));
            for (int v = 0; v < t->nverts; v++) {
                t->xy[v * 2] = rd_f32(&r);
                t->xy[v * 2 + 1] = rd_f32(&r);
                t->uv[v * 2] = rd_f32(&r);
                t->uv[v * 2 + 1] = rd_f32(&r);
            }
        } else {
            /* First pass to count weights, then read for real. */
            const uint8_t *start = r.p;
            int total = 0;
            for (int v = 0; v < t->nverts && !r.bad; v++) {
                skip(&r, 8);
                int n = rd_u8(&r);
                skip(&r, 1 + n * 14);
                total += n;
            }
            r.p = start;
            t->wcount = arena_get(&ar, t->nverts);
            t->wbone = arena_get(&ar, total * sizeof(uint16_t));
            t->wdata = arena_get(&ar, total * 3 * sizeof(float));
            if (!t->wbone || !t->wdata) { r.bad = 1; break; }
            int w = 0;
            for (int v = 0; v < t->nverts; v++) {
                t->uv[v * 2] = rd_f32(&r);
                t->uv[v * 2 + 1] = rd_f32(&r);
                int n = rd_u8(&r);
                skip(&r, 1);
                t->wcount[v] = (uint8_t)n;
                for (int k = 0; k < n; k++, w++) {
                    t->wbone[w] = rd_u16(&r);
                    t->wdata[w * 3] = rd_f32(&r);
                    t->wdata[w * 3 + 1] = rd_f32(&r);
                    t->wdata[w * 3 + 2] = rd_f32(&r);
                }
            }
        }
        t->tris = arena_get(&ar, t->ntris * sizeof(uint16_t));
        for (int k = 0; k < t->ntris; k++) t->tris[k] = rd_u16(&r);
    }
    a->tl = arena_get(&ar, a->ntl * sizeof(SpTimeline));
    for (int i = 0; i < a->ntl && !r.bad; i++) {
        SpTimeline *tl = &a->tl[i];
        tl->kind = rd_u8(&r);
        skip(&r, 1);
        tl->target = rd_u16(&r);
        tl->nkeys = rd_u16(&r);
        tl->keys = arena_get(&ar, tl->nkeys * sizeof(SpKey));
        if (!tl->keys) { r.bad = 1; break; }
        for (int k = 0; k < tl->nkeys; k++) {
            SpKey *key = &tl->keys[k];
            key->time = rd_f32(&r);
            for (int j = 0; j < 4; j++) key->v[j] = rd_f32(&r);
            key->curve = rd_u8(&r);
            skip(&r, 3);
            for (int j = 0; j < 4; j++) key->bz[j] = rd_f32(&r);
        }
    }
    int norder = rd_u16(&r);
    a->order_table = arena_get(&ar, (norder + 1) * sizeof(uint16_t));
    for (int k = 0; k < norder; k++) a->order_table[k] = rd_u16(&r);

    a->nik = rd_u16(&r);
    a->ik = arena_get(&ar, (a->nik + 1) * sizeof(SpIk));
    for (int k = 0; k < a->nik; k++) {
        SpIk *c = &a->ik[k];
        c->parent = rd_u16(&r);
        c->child = rd_u16(&r);
        c->target = rd_u16(&r);
        c->bend = (int8_t)rd_u8(&r);
        skip(&r, 1);
        c->mix = rd_f32(&r);
    }
    a->nffd = rd_u16(&r);
    a->ffd = arena_get(&ar, (a->nffd + 1) * sizeof(SpFfd));
    for (int k = 0; k < a->nffd && !r.bad; k++) {
        SpFfd *f = &a->ffd[k];
        f->att = rd_u16(&r);
        f->nkeys = rd_u16(&r);
        f->nvals = rd_u16(&r);
        f->keys = arena_get(&ar, f->nkeys * sizeof(SpFfdKey));
        if (!f->keys) { r.bad = 1; break; }
        for (int j = 0; j < f->nkeys; j++) {
            SpFfdKey *key = &f->keys[j];
            key->time = rd_f32(&r);
            key->curve = rd_u8(&r);
            skip(&r, 3);
            for (int b = 0; b < 4; b++) key->bz[b] = rd_f32(&r);
            key->offset = rd_u16(&r);
            key->count = rd_u16(&r);
            key->deltas = arena_get(&ar, (key->count + 1) * sizeof(float));
            if (!key->deltas) { r.bad = 1; break; }
            for (int v = 0; v < key->count; v++) key->deltas[v] = rd_f32(&r);
        }
    }
    /* the writer pads to 16 bytes so the texture data is aligned for the PSP GE */
    while (((r.p - (const uint8_t *)blob) & 15) != 0 && r.p < r.end) r.p++;
    a->clut = r.p;
    skip(&r, 256 * 4);
    a->pixels = r.p;
    skip(&r, a->page_w * a->page_h);
    if (r.bad) {
        free(a->mem);
        a->mem = NULL;
        return -4;
    }
    a->blob = blob;
    *used = ar.used;
    return 0;
}

/* Public loader: two-pass parse (measure, then allocate exactly) - see comment inside. */
int sp_anim_load(SpAnim *a, void *blob, int size) {
    /* parse once with a generous scratch block to learn the exact size, then again into a
     * block of exactly that size: keeps several MB free on a 24 MB PSP-1000 */
    size_t used = 0;
    int rc = parse(a, blob, size, 0, &used);
    if (rc) return rc;
    free(a->mem);
    rc = parse(a, blob, size, used, &used);
    if (rc) {
        free(a->mem);
        a->mem = NULL;
    }
    return rc;
}

/* Frees the arena and the file blob. */
void sp_anim_free(SpAnim *a) {
    free(a->mem);
    free(a->blob);
    memset(a, 0, sizeof(*a));
}

/* ---------------------------------------------------------------- posing */

/* Spine 2.1 evaluates bezier curves on 10 precomputed segments; sampling the curve at the same
 * resolution and interpolating gives the same easing. */
static float bezier_percent(const float bz[4], float x) {
    float px = 0, py = 0;
    for (int i = 1; i <= 10; i++) {
        float t = i / 10.0f, it = 1 - t;
        float cx = 3 * it * it * t * bz[0] + 3 * it * t * t * bz[2] + t * t * t;
        float cy = 3 * it * it * t * bz[1] + 3 * it * t * t * bz[3] + t * t * t;
        if (cx >= x) {
            float span = cx - px;
            return span > 1e-6f ? py + (cy - py) * (x - px) / span : cy;
        }
        px = cx;
        py = cy;
    }
    return py + (1 - py) * (x - px) / (1 - px + 1e-6f);
}

/* Timeline lookup: which two keys surround time t, and how far between them
 * (pct 0..1, after applying the curve easing).
 * Finds the key interval for t. Returns -1 before the first key; otherwise sets *pct and
 * returns the index of the key at or before t (pct == 0 at/after the last key). */
static int find_key(const SpTimeline *tl, float t, float *pct) {
    if (t < tl->keys[0].time) return -1;
    int last = tl->nkeys - 1;
    if (t >= tl->keys[last].time) { *pct = 0; return last; }
    int i = 0;
    while (i < last - 1 && tl->keys[i + 1].time <= t) i++;
    const SpKey *k0 = &tl->keys[i], *k1 = &tl->keys[i + 1];
    float span = k1->time - k0->time;
    float x = span > 0 ? (t - k0->time) / span : 0;
    if (k0->curve == 1) x = 0;
    else if (k0->curve == 2) x = bezier_percent(k0->bz, x);
    *pct = x;
    return i;
}

/* Interpolate rotations the SHORT way round (e.g. 350deg -> 10deg is +20, not -340). */
static float lerp_angle(float a, float b, float pct) {
    float d = b - a;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return a + d * pct;
}

static void update_world(SpPose *p, const SpAnim *a);
static void apply_ik(SpPose *p, const SpAnim *a, const SpIk *c);
static void apply_ffd(SpPose *p, const SpAnim *a, float t);

/* Evaluate animation `a` at time t into pose `p`.
 * 1. wrap/clamp time   2. reset bones+slots to the setup pose
 * 3. apply every timeline on top   4. compute world transforms
 * 5. IK (needs world positions) then recompute   6. FFD mesh offsets. */
void sp_pose(SpPose *p, const SpAnim *a, float t, int loop) {
    p->anim = a;
    if (loop && a->duration > 0) t = fmodf(t, a->duration);
    else if (t > a->duration) t = a->duration;

    for (int i = 0; i < a->nbones; i++) {
        const SpBoneData *d = &a->bones[i];
        SpBone *b = &p->bones[i];
        b->x = d->x; b->y = d->y; b->rot = d->rot; b->sx = d->sx; b->sy = d->sy;
    }
    for (int i = 0; i < a->nslots; i++) {
        p->slot_att[i] = a->slots[i].attachment;
        p->slot_rgba[i][0] = a->slots[i].r;
        p->slot_rgba[i][1] = a->slots[i].g;
        p->slot_rgba[i][2] = a->slots[i].b;
        p->slot_rgba[i][3] = a->slots[i].a;
        p->draw_order[i] = (uint16_t)i;
    }

    for (int i = 0; i < a->ntl; i++) {
        const SpTimeline *tl = &a->tl[i];
        float pct;
        int k = find_key(tl, t, &pct);
        if (k < 0) continue;
        const float *v0 = tl->keys[k].v;
        const float *v1 = k + 1 < tl->nkeys ? tl->keys[k + 1].v : v0;
        switch (tl->kind) {
        case SP_TL_ROTATE: {
            SpBone *b = &p->bones[tl->target];
            b->rot = a->bones[tl->target].rot + lerp_angle(v0[0], v1[0], pct);
            break;
        }
        case SP_TL_TRANSLATE: {
            SpBone *b = &p->bones[tl->target];
            b->x = a->bones[tl->target].x + v0[0] + (v1[0] - v0[0]) * pct;
            b->y = a->bones[tl->target].y + v0[1] + (v1[1] - v0[1]) * pct;
            break;
        }
        case SP_TL_SCALE: {
            SpBone *b = &p->bones[tl->target];
            b->sx = a->bones[tl->target].sx * (v0[0] + (v1[0] - v0[0]) * pct);
            b->sy = a->bones[tl->target].sy * (v0[1] + (v1[1] - v0[1]) * pct);
            break;
        }
        case SP_TL_ATTACHMENT:
            p->slot_att[tl->target] = (int16_t)v0[0];
            break;
        case SP_TL_COLOR:
            for (int c = 0; c < 4; c++)
                p->slot_rgba[tl->target][c] = (uint8_t)((v0[c] + (v1[c] - v0[c]) * pct) * 255.0f + 0.5f);
            break;
        case SP_TL_DRAW_ORDER:
            memcpy(p->draw_order, a->order_table + (int)v0[0], a->nslots * sizeof(uint16_t));
            break;
        }
    }

    update_world(p, a);
    if (a->nik) {
        for (int i = 0; i < a->nik; i++) apply_ik(p, a, &a->ik[i]);
        update_world(p, a);
    }
    apply_ffd(p, a, t);
}

/* Interpolated FFD deltas for attachments visible this frame. */
static void apply_ffd(SpPose *p, const SpAnim *a, float t) {
    int used = 0;
    p->nffd = 0;
    for (int i = 0; i < a->nffd && p->nffd < SP_MAX_FFD; i++) {
        const SpFfd *f = &a->ffd[i];
        if (f->nkeys == 0 || t < f->keys[0].time) continue;
        if (p->slot_att[a->atts[f->att].slot] != (int16_t)f->att) continue;
        if (used + f->nvals > SP_FFD_POOL) break;
        float *out = p->ffd_pool + used;
        used += f->nvals;
        memset(out, 0, f->nvals * sizeof(float));
        int k = 0;
        while (k + 1 < f->nkeys && f->keys[k + 1].time <= t) k++;
        const SpFfdKey *k0 = &f->keys[k];
        float pct = 0;
        const SpFfdKey *k1 = k0;
        if (k + 1 < f->nkeys) {
            k1 = &f->keys[k + 1];
            float span = k1->time - k0->time;
            pct = span > 0 ? (t - k0->time) / span : 0;
            if (k0->curve == 1) pct = 0;
            else if (k0->curve == 2) pct = bezier_percent(k0->bz, pct);
        }
        for (int v = 0; v < k0->count && k0->offset + v < f->nvals; v++)
            out[k0->offset + v] += k0->deltas[v] * (1 - pct);
        for (int v = 0; v < k1->count && k1->offset + v < f->nvals; v++)
            out[k1->offset + v] += k1->deltas[v] * pct;
        p->ffd_att[p->nffd] = f->att;
        p->ffd_vals[p->nffd] = out;
        p->nffd++;
    }
}

/* Two-bone (or one-bone) IK solved in world space, then written back as local rotations.
 * Same geometry as Spine 2.1's IkConstraint: law of cosines on the parent and child lengths. */
static void apply_ik(SpPose *p, const SpAnim *a, const SpIk *c) {
    const float R2D = 57.29577951308232f;
    SpBone *pb = &p->bones[c->parent];
    const SpBone *tg = &p->bones[c->target];
    int pp = a->bones[c->parent].parent;
    float pp_rot = (pp >= 0 && (a->bones[c->parent].flags & 2)) ? p->bones[pp].wrot : 0;
    float dx = tg->wx - pb->wx, dy = tg->wy - pb->wy;

    if (c->child == 0xFFFF) {
        pb->rot = lerp_angle(pb->rot, atan2f(dy, dx) * R2D - pp_rot, c->mix);
        return;
    }
    SpBone *cb = &p->bones[c->child];
    /* child joint position in the parent's frame (including the parent's scale) */
    float cx = cb->x * pb->wsx, cy = cb->y * pb->wsy;
    float offset = atan2f(cy, cx);
    float len1 = sqrtf(cx * cx + cy * cy);
    float len2 = a->bones[c->child].len * cb->wsx;
    float denom = 2 * len1 * len2;
    float target_ang = atan2f(dy, dx);
    float parent_world, child_local;
    if (denom < 1e-4f) {
        parent_world = pb->wrot * DEG2RAD;
        child_local = target_ang - parent_world;
    } else {
        float cosv = (dx * dx + dy * dy - len1 * len1 - len2 * len2) / denom;
        if (cosv < -1) cosv = -1;
        if (cosv > 1) cosv = 1;
        float child_ang = acosf(cosv) * c->bend;
        float adj = len1 + len2 * cosv, opp = len2 * sinf(child_ang);
        float joint_dir = atan2f(dy * adj - dx * opp, dx * adj + dy * opp);
        parent_world = joint_dir - offset;
        child_local = child_ang + offset;
    }
    pb->rot = lerp_angle(pb->rot, parent_world * R2D - pp_rot, c->mix);
    cb->rot = lerp_angle(cb->rot, child_local * R2D, c->mix);
}

/* Compute each bone's world transform from its parent's.  Because bones are
 * stored parent-first, one forward loop is enough.  The `flags` bits say
 * whether scale/rotation are inherited.  m00..m11 is the 2x2 matrix
 * (rotation * scale) later used to transform attachment vertices. */
static void update_world(SpPose *p, const SpAnim *a) {
    for (int i = 0; i < a->nbones; i++) {
        SpBone *b = &p->bones[i];
        const SpBoneData *d = &a->bones[i];
        if (d->parent >= 0) {
            const SpBone *pb = &p->bones[d->parent];
            b->wx = b->x * pb->m00 + b->y * pb->m01 + pb->wx;
            b->wy = b->x * pb->m10 + b->y * pb->m11 + pb->wy;
            b->wsx = (d->flags & 1) ? pb->wsx * b->sx : b->sx;
            b->wsy = (d->flags & 1) ? pb->wsy * b->sy : b->sy;
            b->wrot = (d->flags & 2) ? pb->wrot + b->rot : b->rot;
        } else {
            b->wx = b->x; b->wy = b->y;
            b->wsx = b->sx; b->wsy = b->sy;
            b->wrot = b->rot;
        }
        float c = cosf(b->wrot * DEG2RAD), s = sinf(b->wrot * DEG2RAD);
        b->m00 = c * b->wsx; b->m01 = -s * b->wsy;
        b->m10 = s * b->wsx; b->m11 = c * b->wsy;
    }
}

/* ---------------------------------------------------------------- emitting */

/* Multiply slot colour by the caller's tint per channel, pack as ABGR u32. */
static uint32_t pack_rgba(const uint8_t s[4], const uint8_t tint[4]) {
    uint32_t r = s[0] * tint[0] / 255, g = s[1] * tint[1] / 255;
    uint32_t b = s[2] * tint[2] / 255, al = s[3] * tint[3] / 255;
    return r | (g << 8) | (b << 16) | (al << 24);
}

/* Build the triangle list.  Walk slots in draw order (back to front); skip
 * empty slots and ones not matching this additive pass.  For each attachment
 * compute world-space vertex positions:
 *   REGION  - 4 corners of a rotated, scaled rectangle on the bone
 *   MESH    - local vertex transformed by the bone matrix (+FFD offset)
 *   SKINNED - weighted sum over several bones
 * then output indexed triangles as plain vertices. Screen y points DOWN while
 * skeleton y points UP, hence `y - wy`. flip mirrors x. */
int sp_emit(const SpPose *p, float x, float y, float scale, int flip, const uint8_t tint[4],
            int additive_pass, SpVertex *out, int max_verts) {
    const SpAnim *a = p->anim;
    float wx[512], wy[512];
    int n = 0;
    float fx = flip ? -scale : scale;

    for (int o = 0; o < a->nslots; o++) {
        int si = p->draw_order[o];
        int ai = p->slot_att[si];
        if (ai < 0 || ai >= a->natts) continue;
        if ((a->slots[si].additive != 0) != (additive_pass != 0)) continue;
        const SpAttachment *t = &a->atts[ai];
        const SpBone *b = &p->bones[a->slots[si].bone];
        uint32_t col = pack_rgba(p->slot_rgba[si], tint);
        if (t->nverts > 512) continue;
        const float *def = NULL;
        for (int k = 0; k < p->nffd; k++)
            if (p->ffd_att[k] == ai) def = p->ffd_vals[k];

        static const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
        const uint16_t *idx = t->tris;
        int nidx = t->ntris;

        if (t->type == SP_REGION) {
            const float *r = t->region;
            float rc = cosf(r[2] * DEG2RAD), rs = sinf(r[2] * DEG2RAD);
            float hw = r[5] * 0.5f * r[3], hh = r[6] * 0.5f * r[4];
            const float cx[4] = {-hw, hw, hw, -hw}, cy[4] = {-hh, -hh, hh, hh};
            for (int v = 0; v < 4; v++) {
                float lx = r[0] + cx[v] * rc - cy[v] * rs;
                float ly = r[1] + cx[v] * rs + cy[v] * rc;
                wx[v] = lx * b->m00 + ly * b->m01 + b->wx;
                wy[v] = lx * b->m10 + ly * b->m11 + b->wy;
            }
            idx = quad;
            nidx = 6;
        } else if (t->type == SP_MESH) {
            for (int v = 0; v < t->nverts; v++) {
                float lx = t->xy[v * 2], ly = t->xy[v * 2 + 1];
                if (def) { lx += def[v * 2]; ly += def[v * 2 + 1]; }
                wx[v] = lx * b->m00 + ly * b->m01 + b->wx;
                wy[v] = lx * b->m10 + ly * b->m11 + b->wy;
            }
        } else {
            int w = 0;
            for (int v = 0; v < t->nverts; v++) {
                float sx = 0, sy = 0;
                for (int k = 0; k < t->wcount[v]; k++, w++) {
                    const SpBone *wb = &p->bones[t->wbone[w]];
                    float lx = t->wdata[w * 3], ly = t->wdata[w * 3 + 1], wt = t->wdata[w * 3 + 2];
                    if (def) { lx += def[w * 2]; ly += def[w * 2 + 1]; }
                    sx += (lx * wb->m00 + ly * wb->m01 + wb->wx) * wt;
                    sy += (lx * wb->m10 + ly * wb->m11 + wb->wy) * wt;
                }
                wx[v] = sx;
                wy[v] = sy;
            }
        }
        if (n + nidx > max_verts) break;
        for (int k = 0; k < nidx; k++) {
            int v = idx[k];
            SpVertex *ov = &out[n++];
            ov->u = t->uv[v * 2];
            ov->v = t->uv[v * 2 + 1];
            ov->color = col;
            ov->x = x + wx[v] * fx;
            ov->y = y - wy[v] * scale;
            ov->z = 0;
        }
    }
    return n;
}
