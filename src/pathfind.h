#ifndef IRON_PATHFIND_H
#define IRON_PATHFIND_H

#include "tilemap.h"

#include <stdbool.h>

#define MAX_PATH 512

typedef struct {
    int xs[MAX_PATH];
    int ys[MAX_PATH];
    int len;
} Path;

bool tile_passable(const TileMap *map, int x, int y);
bool pathfind(const TileMap *map, int sx, int sy, int gx, int gy, Path *out);

#endif
