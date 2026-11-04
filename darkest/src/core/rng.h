#ifndef DD_RNG_H
#define DD_RNG_H

#include <stdint.h>

/*
 * rng.h - the game's random number generator.
 *
 * WHY our own RNG instead of rand()?  Determinism.  Given the same seed the
 * same sequence comes out on every platform (PSP, PC, tests).  That lets the
 * battle simulator and the scripted test runner replay a fight exactly, and
 * lets a dungeon map be regenerated from a stored seed instead of saving it.
 *
 * Algorithm: xorshift32.  Very small and fast: three shift/xor steps on one
 * 32-bit word.  Not cryptographic - just good enough for a game.
 * All functions are `static inline` so they live in the header and the
 * compiler can inline them (no .c file needed).
 *
 * The whole generator state is ONE integer, so an Rng is cheap to copy,
 * store in a struct, or save to disk.
 */
typedef struct { uint32_t s; } Rng;

/* The seed is scrambled first: raw small seeds would make the first xorshift draws tiny. */
static inline void rng_seed(Rng *r, uint32_t seed) {
    seed = (seed ^ (seed >> 16)) * 0x45d9f3bu;
    seed = (seed ^ (seed >> 16)) * 0x45d9f3bu;
    seed ^= seed >> 16;
    r->s = seed ? seed : 0x9e3779b9u;
}

/* Next raw 32-bit value. xorshift: x ^= x<<13; x ^= x>>17; x ^= x<<5.
 * State must never be 0 (it would stay 0 forever) - rng_seed guards that. */
static inline uint32_t rng_u32(Rng *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return r->s = x;
}

/* Uniform integer in [lo, hi] inclusive. (Modulo bias is negligible for the
 * small ranges used in game rules.) If hi <= lo it just returns lo. */
static inline int rng_range(Rng *r, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(rng_u32(r) % (uint32_t)(hi - lo + 1));
}

/* Uniform float in [0, 1). Keeps the top 24 bits (a float's mantissa width)
 * and divides by 2^24 so every result is exactly representable. */
static inline float rng_float(Rng *r) { return (rng_u32(r) >> 8) * (1.0f / 16777216.0f); }

/* True with probability p (0..1). The standard way to roll "x% chance" in
 * game rules, e.g. rng_chance(r, 0.35f) for a 35% crit. */
static inline int rng_chance(Rng *r, float p) { return rng_float(r) < p; }

#endif
