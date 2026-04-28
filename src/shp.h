#ifndef IRON_SHP_H
#define IRON_SHP_H

#include "raylib.h"
#include "palette.h"

#include <stdbool.h>

/* Loaded Westwood SHP_TD sprite: a frame canvas of (width × height) and
 * `frame_count` palette-indexed frames already converted to RGBA Texture2D
 * using the supplied palette (index 0 → transparent). */
typedef struct {
    int       frame_count;
    int       width;
    int       height;
    Texture2D *frames;
} ShpSprite;

bool shp_load(ShpSprite *out, const char *path, const Palette *palette);
void shp_unload(ShpSprite *s);

#endif
