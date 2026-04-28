#ifndef IRON_TILEMAP_H
#define IRON_TILEMAP_H

#include "raylib.h"

#define MAP_WIDTH  64
#define MAP_HEIGHT 64
#define TILE_SIZE  32

typedef enum {
    TILE_GRASS,
    TILE_DIRT,
    TILE_WATER,
    TILE_ROCK,
    TILE_ORE,
    TILE_TYPE_COUNT,
} TileType;

typedef struct {
    TileType      tiles[MAP_HEIGHT][MAP_WIDTH];
    /* 1 = blocked by a building/obstacle that's not part of the underlying
     * terrain. tile_passable consults this in addition to the tile type. */
    unsigned char blocked[MAP_HEIGHT][MAP_WIDTH];
    Texture2D     rendered;
    bool          rendered_built;
} TileMap;

void tilemap_init(TileMap *map, unsigned int seed);

/* Bake the entire map into a single noise-textured Texture2D to avoid
 * drawing tens of thousands of solid rectangles per frame and to give
 * the ground a less flat, OpenRA-ish look. Safe to call at most once
 * after `InitWindow`. */
void tilemap_build_texture(TileMap *map);
void tilemap_unload_texture(TileMap *map);

/* Patch a single tile's pixels in the already-baked texture using the
 * current `tiles[ty][tx]` value. Used after ore depletion so the visual
 * keeps up with the data without re-baking the whole map. */
void tilemap_repaint_tile(TileMap *map, int tx, int ty);

/* Mark or clear a w×h rectangle of tiles as blocked by a building. */
void tilemap_set_blocked(TileMap *map, int tx, int ty, int w, int h, bool blocked);

void tilemap_draw(const TileMap *map, Camera2D camera);

#endif
