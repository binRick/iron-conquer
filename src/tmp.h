#ifndef IRON_TMP_H
#define IRON_TMP_H

#include "raylib.h"
#include "palette.h"

#include <stdbool.h>

/* Westwood TmpRA terrain template: a (width × height) tile size and
 * `frame_count` sub-tiles. Some slots may be blank (the source format uses
 * 0xFF as a "no tile" marker) — those entries get a Texture2D with id=0
 * which `tmp_draw` callers should skip. Format reference:
 *   third_party/OpenRA/OpenRA.Mods.Cnc/SpriteLoaders/TmpRALoader.cs */
typedef struct {
    int        width;
    int        height;
    int        frame_count;
    Texture2D *frames;
} TmpSprite;

bool tmp_load(TmpSprite *out, const char *path, const Palette *palette);
void tmp_unload(TmpSprite *t);

#endif
