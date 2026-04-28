#include "palette.h"

#include <stdio.h>

bool palette_load(Palette *out, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    unsigned char raw[768];
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (n != sizeof(raw)) return false;

    for (int i = 0; i < 256; ++i) {
        unsigned char r6 = raw[i * 3 + 0];
        unsigned char g6 = raw[i * 3 + 1];
        unsigned char b6 = raw[i * 3 + 2];
        /* Expand 6-bit (0..63) to 8-bit by replicating high bits into low. */
        unsigned char r = (unsigned char)((r6 << 2) | (r6 >> 4));
        unsigned char g = (unsigned char)((g6 << 2) | (g6 >> 4));
        unsigned char b = (unsigned char)((b6 << 2) | (b6 >> 4));
        out->colors[i] = (Color){ r, g, b, 255 };
    }
    /* Index 0 is the transparent colour key for sprites. */
    out->colors[0].a = 0;
    return true;
}

void palette_remap_team(const Palette *base, Palette *out,
                        const Color team_ramp[16]) {
    *out = *base;
    for (int i = 0; i < 16; ++i) {
        out->colors[80 + i]   = team_ramp[i];
        out->colors[80 + i].a = 255;
    }
    out->colors[0].a = 0;  /* preserve colour-key transparency */
}

void palette_indexed_to_rgba(const Palette *palette,
                             const unsigned char *indexed,
                             int width, int height,
                             unsigned char *rgba_dst) {
    int n = width * height;
    for (int i = 0; i < n; ++i) {
        unsigned char idx = indexed[i];
        Color c = palette->colors[idx];
        if (idx == 0) {
            rgba_dst[i * 4 + 0] = 0;
            rgba_dst[i * 4 + 1] = 0;
            rgba_dst[i * 4 + 2] = 0;
            rgba_dst[i * 4 + 3] = 0;
        } else {
            rgba_dst[i * 4 + 0] = c.r;
            rgba_dst[i * 4 + 1] = c.g;
            rgba_dst[i * 4 + 2] = c.b;
            rgba_dst[i * 4 + 3] = 255;
        }
    }
}
