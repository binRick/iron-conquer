#include "tilemap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/* Noise sample at the *tile* grid (not pixel) — used by terrain
 * generation. Same bilerp scheme as `bilerp_noise` but the grid
 * scaling is interpreted in tile-units. */
static int tile_noise(int tx, int ty, int scale_log2, uint32_t seed) {
    int mask = (1 << scale_log2) - 1;
    int mx = tx >> scale_log2;
    int my = ty >> scale_log2;
    int fx = tx & mask;
    int fy = ty & mask;
    uint32_t n00 = hash2(mx,     my,     seed);
    uint32_t n10 = hash2(mx + 1, my,     seed);
    uint32_t n01 = hash2(mx,     my + 1, seed);
    uint32_t n11 = hash2(mx + 1, my + 1, seed);
    int v00 = (int)(n00 & 0xFF) - 128;
    int v10 = (int)(n10 & 0xFF) - 128;
    int v01 = (int)(n01 & 0xFF) - 128;
    int v11 = (int)(n11 & 0xFF) - 128;
    int v0 = v00 + ((v10 - v00) * fx >> scale_log2);
    int v1 = v01 + ((v11 - v01) * fx >> scale_log2);
    return v0 + ((v1 - v0) * fy >> scale_log2);
}

static void paint_ore_patches(TileMap *map, unsigned int seed) {
    /* Two large ore clusters at opposite corners away from the spawn
     * corridor — players have to send harvesters out. */
    static const struct { int cx, cy, r; } patches[] = {
        { 12, 12, 6 },
        { 52, 52, 6 },
    };
    int n = (int)(sizeof(patches) / sizeof(patches[0]));
    for (int p = 0; p < n; ++p) {
        int cx = patches[p].cx, cy = patches[p].cy, r = patches[p].r;
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                int dist2 = dx * dx + dy * dy;
                if (dist2 > r * r) continue;
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) continue;
                TileType t = map->tiles[y][x];
                if (t != TILE_GRASS && t != TILE_DIRT) continue;
                /* Density falls off with radius for an organic edge. */
                int chance = 95 - (dist2 * 65) / (r * r);
                if ((int)(hash2(x, y, seed + 0xCAFEu) % 100) < chance)
                    map->tiles[y][x] = TILE_ORE;
            }
        }
    }
}

void tilemap_init(TileMap *map, unsigned int seed) {
    map->rendered_built = false;
    memset(map->blocked, 0, sizeof(map->blocked));

    /* Region-based generation: three independent noise channels drive
     * elevation, moisture, and small-scale variation. Tiles end up in
     * **large connected regions** of the same type instead of being
     * randomly assigned per-tile (which reads as visual noise).
     *
     * Layout intent (similar to a C&C 1995 map):
     *   - broad grass for the playable area
     *   - dirt patches in dry mid-elevation pockets
     *   - rocky outcrops on high ground
     *   - water lakes/rivers in low-elevation low-moisture spots
     */
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) {
            int elev    = tile_noise(x, y, 3, seed);
            int moist   = tile_noise(x, y, 4, seed + 0x11u);
            int detail  = tile_noise(x, y, 2, seed + 0x22u);

            TileType t;
            if (elev < -45 && moist < -20) {
                t = TILE_WATER;
            } else if (elev > 55) {
                t = TILE_ROCK;
            } else if (detail < -55 || (moist < -25 && elev > 10)) {
                t = TILE_DIRT;
            } else {
                t = TILE_GRASS;
            }
            map->tiles[y][x] = t;
        }
    }
    paint_ore_patches(map, seed);
}

static Color tile_color(TileType t) {
    switch (t) {
        case TILE_GRASS: return (Color){ 70, 130, 60, 255 };
        case TILE_DIRT:  return (Color){ 130, 100, 60, 255 };
        case TILE_WATER: return (Color){ 40, 80, 150, 255 };
        case TILE_ROCK:  return (Color){ 110, 110, 115, 255 };
        case TILE_ORE:   return (Color){ 200, 220, 60, 255 };
        default:         return MAGENTA;
    }
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Tile-type "softness": when blending across a tile boundary, dirt and
 * grass blend into each other (close colours, both organic), but
 * water and rock have hard edges (no blend). Returns a per-pair
 * blendable distance — 0 = blend freely, large = hard edge. Used to
 * smooth grass↔dirt transitions while keeping water shorelines crisp. */
static int blend_softness(TileType a, TileType b) {
    if (a == b) return 0;
    /* Symmetric pair handling. */
    if ((a == TILE_GRASS && b == TILE_DIRT) ||
        (a == TILE_DIRT  && b == TILE_GRASS)) return 6;
    if ((a == TILE_GRASS && b == TILE_ORE)  ||
        (a == TILE_ORE   && b == TILE_GRASS) ||
        (a == TILE_DIRT  && b == TILE_ORE)  ||
        (a == TILE_ORE   && b == TILE_DIRT))  return 5;
    /* Water/rock — no blending. Caller treats as a hard boundary. */
    return 0xFFFF;
}

/* Bilinear-interpolate a 256-step random field at the given grid scale.
 * Returns a signed value roughly in [-128, 127]. Used to compose
 * smooth low/mid-frequency noise on top of per-pixel speckle. */
static int bilerp_noise(int x, int y, int scale_log2, uint32_t seed) {
    int mask = (1 << scale_log2) - 1;
    int mx = x >> scale_log2;
    int my = y >> scale_log2;
    int fx = x & mask;
    int fy = y & mask;
    uint32_t n00 = hash2(mx,     my,     seed);
    uint32_t n10 = hash2(mx + 1, my,     seed);
    uint32_t n01 = hash2(mx,     my + 1, seed);
    uint32_t n11 = hash2(mx + 1, my + 1, seed);
    int v00 = (int)(n00 & 0xFF) - 128;
    int v10 = (int)(n10 & 0xFF) - 128;
    int v01 = (int)(n01 & 0xFF) - 128;
    int v11 = (int)(n11 & 0xFF) - 128;
    int v0 = v00 + ((v10 - v00) * fx >> scale_log2);
    int v1 = v01 + ((v11 - v01) * fx >> scale_log2);
    return v0 + ((v1 - v0) * fy >> scale_log2);
}

/* Per-pixel coloring used by both tilemap_build_texture and
 * tilemap_repaint_tile, so the depleted-tile patch matches what was
 * baked initially. */
static Color pixel_color_at(const TileMap *map, int x, int y) {
    int ty = y / TILE_SIZE;
    int tx = x / TILE_SIZE;
    TileType type = map->tiles[ty][tx];
    Color base = tile_color(type);

    /* Multi-octave brightness noise. Kept gentle on purpose — too
     * much amplitude reads as static. The low-frequency component
     * adds map-scale shading, the mid adds blotches, the high adds
     * a touch of grit. */
    uint32_t n     = hash2(x, y, 0xA5A5u);
    int delta_hi   = (int)(n & 0xF) - 8;              /* ±8, per pixel */
    int delta_mid  = bilerp_noise(x, y, 3, 0xBEEFu);  /* ±128 → /5 → ~±25 */
    int delta_low  = bilerp_noise(x, y, 5, 0xCEEDu);  /* ±128 → /6 → ~±21 */
    int delta = delta_hi + delta_mid / 5 + delta_low / 6;

    int r = clampi((int)base.r + delta, 0, 255);
    int g = clampi((int)base.g + delta, 0, 255);
    int b = clampi((int)base.b + delta, 0, 255);

    /* Subtle hue drift via three independent low-freq channels. */
    int hr = bilerp_noise(x, y, 5, 0x1111u);
    int hg = bilerp_noise(x, y, 5, 0x2222u);
    int hb = bilerp_noise(x, y, 5, 0x3333u);
    r = clampi(r + hr / 12, 0, 255);
    g = clampi(g + hg / 12, 0, 255);
    b = clampi(b + hb / 12, 0, 255);

    int sx = x % TILE_SIZE;
    int sy = y % TILE_SIZE;
    (void)sx; (void)sy;

    /* Sparse per-type speckle. Kept low-frequency so tiles read as
     * mostly-flat with occasional detail rather than busy static. */
    uint32_t spec = (n >> 5) & 0xFF;
    if (type == TILE_DIRT && spec < 3) {
        r = clampi(r - 20, 0, 255);
        g = clampi(g - 20, 0, 255);
        b = clampi(b - 20, 0, 255);
    } else if (type == TILE_ROCK && spec < 4) {
        int s = (int)(n >> 13) & 0x1F;
        r = clampi(r + s - 8, 0, 255);
        g = clampi(g + s - 8, 0, 255);
        b = clampi(b + s - 6, 0, 255);
    } else if (type == TILE_GRASS && spec < 2) {
        /* Tiny grass tuft highlight. */
        r = clampi(r + 10, 0, 255);
        g = clampi(g + 18, 0, 255);
        b = clampi(b + 4,  0, 255);
    } else if (type == TILE_WATER && spec < 2) {
        r = clampi(r + 20, 0, 255);
        g = clampi(g + 25, 0, 255);
        b = clampi(b + 25, 0, 255);
    } else if (type == TILE_ORE && spec < 14) {
        int boost = (int)(n >> 17) & 0x3F;
        r = clampi(r + 20 + boost / 2, 0, 255);
        g = clampi(g + 30 + boost,     0, 255);
        b = clampi(b - 10,             0, 255);
    }

    /* Edge-blend toward the nearest neighbour tile if the pair is soft
     * (grass↔dirt, ore↔grass/dirt). Distance to nearest edge weighted
     * with a per-pixel jitter to break the otherwise-too-clean boundary. */
    int dl = sx, dr = TILE_SIZE - 1 - sx;
    int dt = sy, db = TILE_SIZE - 1 - sy;
    int min_d = dl, min_dir = 0;
    if (dr < min_d) { min_d = dr; min_dir = 1; }
    if (dt < min_d) { min_d = dt; min_dir = 2; }
    if (db < min_d) { min_d = db; min_dir = 3; }

    int nx = tx, ny = ty;
    if      (min_dir == 0) nx--;
    else if (min_dir == 1) nx++;
    else if (min_dir == 2) ny--;
    else                   ny++;

    if (nx >= 0 && ny >= 0 && nx < MAP_WIDTH && ny < MAP_HEIGHT) {
        TileType cur = map->tiles[ty][tx];
        TileType nb  = map->tiles[ny][nx];
        int soft = blend_softness(cur, nb);
        if (soft > 0 && soft <= TILE_SIZE && min_d < soft) {
            /* Per-pixel jitter on the threshold so the boundary isn't
             * a perfect straight line — looks more like worn turf. */
            int jitter = ((int)((n >> 23) & 0x3)) - 1;
            int eff_d  = min_d + jitter;
            if (eff_d < 0) eff_d = 0;
            if (eff_d < soft) {
                Color nc = tile_color(nb);
                float alpha = 1.0f - ((float)eff_d + 0.5f) / (float)soft;
                alpha *= 0.45f;  /* keep blend subtle, don't lose tile identity */
                int rr = (int)((1.0f - alpha) * (float)r + alpha * (float)nc.r);
                int gg = (int)((1.0f - alpha) * (float)g + alpha * (float)nc.g);
                int bb = (int)((1.0f - alpha) * (float)b + alpha * (float)nc.b);
                r = clampi(rr, 0, 255);
                g = clampi(gg, 0, 255);
                b = clampi(bb, 0, 255);
            }
        }
    }

    return (Color){
        (unsigned char)r, (unsigned char)g, (unsigned char)b, 255 };
}

void tilemap_build_texture(TileMap *map) {
    if (map->rendered_built) return;

    const int w = MAP_WIDTH  * TILE_SIZE;
    const int h = MAP_HEIGHT * TILE_SIZE;

    Color *pixels = (Color *)malloc((size_t)(w * h) * sizeof(Color));
    if (!pixels) return;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            pixels[y * w + x] = pixel_color_at(map, x, y);
        }
    }

    Image img = {
        .data    = pixels,
        .width   = w,
        .height  = h,
        .mipmaps = 1,
        .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };
    map->rendered = LoadTextureFromImage(img);
    map->rendered_built = true;
    free(pixels);
}

void tilemap_set_blocked(TileMap *map, int tx, int ty, int w, int h, bool blocked) {
    for (int dy = 0; dy < h; ++dy) {
        for (int dx = 0; dx < w; ++dx) {
            int x = tx + dx, y = ty + dy;
            if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) continue;
            map->blocked[y][x] = blocked ? 1 : 0;
        }
    }
}

void tilemap_repaint_tile(TileMap *map, int tx, int ty) {
    if (!map->rendered_built) return;
    if (tx < 0 || ty < 0 || tx >= MAP_WIDTH || ty >= MAP_HEIGHT) return;

    Color buf[TILE_SIZE * TILE_SIZE];
    int x0 = tx * TILE_SIZE;
    int y0 = ty * TILE_SIZE;
    for (int py = 0; py < TILE_SIZE; ++py) {
        for (int px = 0; px < TILE_SIZE; ++px) {
            buf[py * TILE_SIZE + px] = pixel_color_at(map, x0 + px, y0 + py);
        }
    }
    Rectangle dst = { (float)x0, (float)y0, (float)TILE_SIZE, (float)TILE_SIZE };
    UpdateTextureRec(map->rendered, dst, buf);
}

void tilemap_unload_texture(TileMap *map) {
    if (!map->rendered_built) return;
    UnloadTexture(map->rendered);
    map->rendered_built = false;
}

void tilemap_draw(const TileMap *map, Camera2D camera) {
    if (map->rendered_built) {
        DrawTexture(map->rendered, 0, 0, WHITE);
        return;
    }

    /* Fallback path: per-tile rectangles when the baked texture isn't
     * available (e.g. baking failed). Camera-frustum culled. */
    Vector2 tl = GetScreenToWorld2D((Vector2){ 0, 0 }, camera);
    Vector2 br = GetScreenToWorld2D(
        (Vector2){ (float)GetScreenWidth(), (float)GetScreenHeight() }, camera);

    int x0 = (int)(tl.x / TILE_SIZE) - 1;
    int y0 = (int)(tl.y / TILE_SIZE) - 1;
    int x1 = (int)(br.x / TILE_SIZE) + 1;
    int y1 = (int)(br.y / TILE_SIZE) + 1;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > MAP_WIDTH)  x1 = MAP_WIDTH;
    if (y1 > MAP_HEIGHT) y1 = MAP_HEIGHT;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            DrawRectangle(x * TILE_SIZE, y * TILE_SIZE, TILE_SIZE, TILE_SIZE,
                          tile_color(map->tiles[y][x]));
        }
    }
}
