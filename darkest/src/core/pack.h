/* pack.h - reading assets.pak.  On a real PSP shipping 120 MB of loose files is
 * slow and awkward, so pack_assets.py bundles them into ONE file with every
 * asset zlib-compressed (31 MB).  Everything loads through asset_read().
 *
 * assets.pak: the converted assets in one zlib-compressed file (tools/pack_assets.py). */
#ifndef DD_PACK_H
#define DD_PACK_H

/* Reads "assets/<path>" from assets.pak, or the loose file when there is no pack (or the path
 * isn't in it). Returns a buffer from plat_alloc (16-byte aligned on PSP); free() it. */
void *asset_read(const char *path, int *size);

#endif
