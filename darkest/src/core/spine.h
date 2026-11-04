/*
 * spine.h - minimal runtime for Spine 2.1 skeletal animation.
 * Reads the demake's .dda files (written by tools/export_art.py).
 *
 * BACKGROUND: the original game animates characters with Spine.  A character
 * is a SKELETON of BONES (a tree: hip -> torso -> arm -> hand...).  Textured
 * pieces (ATTACHMENTS) are hung on bones through SLOTS.  An animation is a set
 * of TIMELINES holding KEYFRAMES that change bone rotation/position/scale,
 * slot colour, which attachment is visible, and draw order over time.
 *
 * Pipeline per frame:  sp_pose (evaluate timelines -> bone transforms)
 *                      sp_emit (turn attachments into screen triangles)
 *
 * Supported: bones (rotate/translate/scale timelines, bezier/stepped curves),
 * slot attachment + colour timelines, draw order, region / mesh /
 * weighted-skinned-mesh attachments, IK constraints and FFD (free-form mesh
 * deformation).  Not supported: events, path/transform constraints (not in 2.1).
 */
#ifndef DD_SPINE_H
#define DD_SPINE_H

#include <stdint.h>

/* One output vertex.  Layout matches sceGu GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF. */
typedef struct {
    float u, v;
    uint32_t color; /* ABGR in memory order R,G,B,A */
    float x, y, z;
} SpVertex;

/* Static (setup-pose) data of one bone.  parent = index of parent bone, -1 for
 * the root.  Bones are stored parent-first so one forward pass can compute
 * world transforms.  x,y,rot,sx,sy = local transform relative to the parent;
 * len = bone length (needed for IK). */
typedef struct {
    int16_t parent;
    uint8_t flags; /* 1 inherit scale, 2 inherit rotation */
    float x, y, rot, sx, sy, len;
} SpBoneData;

/* A slot: a "socket" on a bone that shows at most one attachment at a time.
 * attachment = default attachment index (-1 none); rgba = default tint;
 * additive = draw with additive blending. */
typedef struct {
    uint16_t bone;
    int16_t attachment;
    uint8_t r, g, b, a;
    uint8_t additive;
} SpSlotData;

/* Attachment kinds: REGION = a plain textured rectangle; MESH = free
 * triangle mesh rigid to one bone; SKINNED = mesh whose vertices blend
 * several bones by weights (smooth bending limbs). */
enum { SP_REGION, SP_MESH, SP_SKINNED };

typedef struct {
    uint8_t type;
    uint16_t slot, nverts, ntris; /* ntris = number of indices */
    float region[7];   /* x, y, rot, sx, sy, w, h (region only) */
    float *uv;         /* nverts * 2, page pixels */
    float *xy;         /* mesh: nverts * 2 local */
    uint8_t *wcount;   /* skinned: weights per vertex */
    uint16_t *wbone;   /* skinned: total weights */
    float *wdata;      /* skinned: total weights * 3 (x, y, weight) */
    uint16_t *tris;
} SpAttachment;

/* One keyframe.  `v` holds the value(s) (meaning depends on timeline kind);
 * `curve` selects interpolation to the NEXT key; bz = bezier control points. */
typedef struct {
    float time, v[4];
    uint8_t curve; /* 0 linear, 1 stepped, 2 bezier */
    float bz[4];
} SpKey;

/* What a timeline animates; `target` is the bone or slot index. */
enum { SP_TL_ROTATE, SP_TL_TRANSLATE, SP_TL_SCALE, SP_TL_ATTACHMENT, SP_TL_COLOR, SP_TL_DRAW_ORDER };

typedef struct {
    uint8_t kind;
    uint16_t target, nkeys;
    SpKey *keys;
} SpTimeline;

/* Inverse-kinematics constraint: make the end of a bone chain reach a target
 * bone (used for feet planting / reaching arms). */
typedef struct {
    uint16_t parent, child; /* child == 0xffff: one-bone constraint */
    uint16_t target;
    int8_t bend;
    float mix;
} SpIk;

/* FFD (free-form deformation): per-vertex offsets animated over time on a
 * mesh, giving cloth/flesh wobble beyond what bones can do. */
typedef struct {
    float time;
    uint8_t curve;
    float bz[4];
    uint16_t offset, count;
    float *deltas;
} SpFfdKey;

typedef struct {
    uint16_t att, nkeys, nvals;
    SpFfdKey *keys;
} SpFfd;

/* A fully parsed animation (immutable once loaded).  All the arrays live in
 * one block `mem`; clut/pixels point into the original file `blob`.
 * `page` = the single texture atlas containing every body part. */
typedef struct {
    uint16_t page_w, page_h;
    uint16_t nbones, nslots, natts, ntl, nik, nffd;
    float duration;
    SpBoneData *bones;
    SpSlotData *slots;
    SpAttachment *atts;
    SpTimeline *tl;
    SpIk *ik;
    SpFfd *ffd;
    uint16_t *order_table;
    const uint8_t *clut;   /* 256 * RGBA, points into blob */
    const uint8_t *pixels; /* page_w * page_h indices, points into blob */
    void *blob;            /* file contents (owned) */
    void *mem;             /* parsed arrays (owned) */
} SpAnim;

/* A bone's state for the current frame.  Local values come from animation;
 * world values (w*) and the 2x2 matrix m are derived from the parent chain. */
typedef struct {
    float x, y, rot, sx, sy;        /* local, animated */
    float wx, wy, wrot, wsx, wsy;   /* world */
    float m00, m01, m10, m11;
} SpBone;

/* Fixed upper bounds so a pose is a plain struct with no malloc per frame. */
#define SP_MAX_BONES 512
#define SP_MAX_SLOTS 96
#define SP_MAX_FFD 8
#define SP_FFD_POOL 4096

/* Scratch result of sp_pose: everything sp_emit needs to build triangles. */
typedef struct {
    const SpAnim *anim;
    SpBone bones[SP_MAX_BONES];
    int16_t slot_att[SP_MAX_SLOTS];
    uint8_t slot_rgba[SP_MAX_SLOTS][4];
    uint16_t draw_order[SP_MAX_SLOTS];
    /* active mesh deforms for this frame: attachment index -> deltas */
    int nffd;
    uint16_t ffd_att[SP_MAX_FFD];
    float *ffd_vals[SP_MAX_FFD];
    float ffd_pool[SP_FFD_POOL];
} SpPose;

/* Parse a .dda file. Takes ownership of `blob` (malloc'd) on success. Returns 0 on success. */
int sp_anim_load(SpAnim *a, void *blob, int size);
void sp_anim_free(SpAnim *a);

/* Pose the skeleton at time t (seconds; looped when loop != 0). */
void sp_pose(SpPose *p, const SpAnim *a, float t, int loop);

/* Emit triangles for the pose. Screen position (x, y) is the skeleton root, y down.
 * `scale` is extra on-screen scale, `flip` mirrors horizontally. `tint` multiplies colours (RGBA).
 * Writes at most max_verts vertices; returns the count. Additive slots are skipped when
 * additive_pass == 0 and drawn alone when additive_pass == 1. */
int sp_emit(const SpPose *p, float x, float y, float scale, int flip, const uint8_t tint[4],
            int additive_pass, SpVertex *out, int max_verts);

#endif
