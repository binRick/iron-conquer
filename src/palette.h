#ifndef IRON_PALETTE_H
#define IRON_PALETTE_H

#include "raylib.h"

#include <stdbool.h>

/* Westwood 6-bit palette: 256 RGB triples (one byte per channel, 0..63 range).
 * Loader expands each component to 8-bit and treats index 0 as transparent
 * (used as colour-key for sprites). */
typedef struct {
    Color colors[256];
} Palette;

bool palette_load(Palette *out, const char *path);

/* Convenience: build an RGBA byte buffer from an indexed bitmap, using
 * `palette` and treating index 0 as fully transparent. Caller-owned dst. */
void palette_indexed_to_rgba(const Palette *palette,
                             const unsigned char *indexed,
                             int width, int height,
                             unsigned char *rgba_dst);

/* Westwood "remap colors" convention: indices 80..95 of the palette are
 * the per-player team-colour swatches. Build a remapped variant of
 * `base` where those 16 indices are replaced with the supplied `ramp`.
 * Used to render the same SHP sprite in different faction colours. */
void palette_remap_team(const Palette *base, Palette *out,
                        const Color team_ramp[16]);

#endif
