/* Host test: load .dda animations and render a few frames to PPM.
 * Usage: spine_test <out_prefix> <file.dda> [<file.dda> ...] */
#include <stdio.h>
#include <stdlib.h>

#include "gfx.h"
#include "spine.h"

int gfx_soft_save_ppm(const char *path);

/* Tiny file reader for the test. */
static void *read_file(const char *path, int *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(*size);
    if (fread(buf, 1, *size, f) != (size_t)*size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* Load one .dda animation, pose it at a given time, rasterize with the
 * software renderer and write a PPM - a quick way to check the Spine
 * runtime in isolation. */
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s out_prefix file.dda...\n", argv[0]);
        return 1;
    }
    int n = argc - 2;
    SpAnim *anims = calloc(n, sizeof(SpAnim));
    GfxTexture **tex = calloc(n, sizeof(GfxTexture *));
    for (int i = 0; i < n; i++) {
        int size;
        void *blob = read_file(argv[i + 2], &size);
        int rc = blob ? sp_anim_load(&anims[i], blob, size) : -100;
        if (rc) {
            fprintf(stderr, "load %s failed: %d\n", argv[i + 2], rc);
            return 1;
        }
        tex[i] = gfx_texture_t8(anims[i].page_w, anims[i].page_h, anims[i].pixels, anims[i].clut);
        printf("%s: page %dx%d bones %d slots %d atts %d timelines %d duration %.2fs\n", argv[i + 2],
               anims[i].page_w, anims[i].page_h, anims[i].nbones, anims[i].nslots, anims[i].natts,
               anims[i].ntl, anims[i].duration);
    }
    static SpPose pose;
    static SpVertex verts[16384];
    const uint8_t white[4] = {255, 255, 255, 255};
    for (int f = 0; f < 4; f++) {
        gfx_begin(RGBA(28, 24, 30, 255));
        gfx_rect(0, 230, SCREEN_W, 2, RGBA(80, 70, 60, 255));
        for (int i = 0; i < n; i++) {
            float t = anims[i].duration * f / 4.0f;
            sp_pose(&pose, &anims[i], t, 1);
            float x = SCREEN_W * (i + 0.5f) / n;
            for (int pass = 0; pass < 2; pass++) {
                int nv = sp_emit(&pose, x, 230, 1.0f, 0, white, pass, verts, 16384);
                gfx_triangles(tex[i], verts, nv, pass ? BLEND_ADD : BLEND_ALPHA);
            }
        }
        char path[512];
        snprintf(path, sizeof(path), "%s_%d.ppm", argv[1], f);
        gfx_soft_save_ppm(path);
    }
    for (int i = 0; i < n; i++) {
        gfx_texture_free(tex[i]);
        sp_anim_free(&anims[i]);
    }
    return 0;
}
