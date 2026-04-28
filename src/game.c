#include "game.h"
#include "debug_log.h"
#include "pathfind.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synthesise a Wave with a sine + linear-decay envelope; pitch_hz can
 * sweep from start_hz to end_hz over the duration. Square-ish flavor
 * via a tiny clip + saturation. Caller owns the data buffer. */
static Wave synth_beep(float start_hz, float end_hz, float duration_s,
                       float volume) {
    const int sr = 44100;
    int frames = (int)(duration_s * (float)sr);
    if (frames < 1) frames = 1;
    short *data = (short *)malloc((size_t)frames * sizeof(short));
    if (!data) {
        Wave empty = { 0 };
        return empty;
    }
    double phase = 0.0;
    for (int i = 0; i < frames; ++i) {
        float t   = (float)i / (float)frames;            /* 0..1 */
        float env = (1.0f - t) * (1.0f - t);             /* faster decay */
        float hz  = start_hz + (end_hz - start_hz) * t;
        phase += (double)hz / (double)sr;
        float s = (float)sin(phase * 2.0 * 3.14159265);
        /* light saturation gives a chip-tune-ish edge */
        s = (s > 0.6f) ? 0.6f : (s < -0.6f ? -0.6f : s);
        s *= env * volume;
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        data[i] = (short)(s * 30000.0f);
    }
    Wave w = {
        .frameCount = (unsigned int)frames,
        .sampleRate = (unsigned int)sr,
        .sampleSize = 16,
        .channels   = 1,
        .data       = data,
    };
    return w;
}

/* Coarse noise burst — single low note + bandlimited "thump" for explosions. */
static Wave synth_noise(float duration_s, float volume) {
    const int sr = 44100;
    int frames = (int)(duration_s * (float)sr);
    if (frames < 1) frames = 1;
    short *data = (short *)malloc((size_t)frames * sizeof(short));
    if (!data) {
        Wave empty = { 0 };
        return empty;
    }
    uint32_t seed = 0xCAFEBABEu;
    float lp = 0.0f;
    for (int i = 0; i < frames; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float r = ((float)(int)(seed >> 8) / (float)0x800000) - 1.0f;
        lp = lp * 0.92f + r * 0.08f;       /* low-pass to make it thump-y */
        float t   = (float)i / (float)frames;
        float env = (1.0f - t) * (1.0f - t);
        float s = lp * env * volume;
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        data[i] = (short)(s * 32000.0f);
    }
    Wave w = {
        .frameCount = (unsigned int)frames,
        .sampleRate = (unsigned int)sr,
        .sampleSize = 16,
        .channels   = 1,
        .data       = data,
    };
    return w;
}

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float vec2_dist(Vector2 a, Vector2 b) {
    float dx = a.x - b.x, dy = a.y - b.y;
    return sqrtf(dx*dx + dy*dy);
}

/* Push a status message into the floating top-of-screen stack. If a
 * message with the same text is already active and reasonably fresh,
 * we just refresh its timer instead of spawning a duplicate so events
 * like "Insufficient funds" don't pile up under spam-clicks. */
static void push_status(GameState *gs, const char *text, unsigned char tone) {
    if (!gs || !text) return;
    for (int i = 0; i < MAX_STATUS_MSGS; ++i) {
        StatusMsg *m = &gs->status_msgs[i];
        if (m->active && strncmp(m->text, text, STATUS_MSG_LEN) == 0) {
            m->age = 0.0f;
            m->tone = tone;
            return;
        }
    }
    /* Find an inactive slot, or evict the oldest. */
    int slot = -1;
    float oldest = -1.0f;
    int   oldest_idx = 0;
    for (int i = 0; i < MAX_STATUS_MSGS; ++i) {
        if (!gs->status_msgs[i].active) { slot = i; break; }
        if (gs->status_msgs[i].age > oldest) {
            oldest = gs->status_msgs[i].age;
            oldest_idx = i;
        }
    }
    if (slot < 0) slot = oldest_idx;
    StatusMsg *m = &gs->status_msgs[slot];
    m->active   = true;
    m->age      = 0.0f;
    m->duration = 3.5f;
    m->tone     = tone;
    strncpy(m->text, text, STATUS_MSG_LEN - 1);
    m->text[STATUS_MSG_LEN - 1] = '\0';
}

/* Forward decls: tables defined further down but referenced by spawn /
 * economy / build helpers. */
extern const UnitCombatInfo     g_unit_combat_info[UNIT_SHP_COUNT];
static const UnitProductionInfo g_unit_production_info[UNIT_SHP_COUNT];
static const char              *g_unit_short_names[UNIT_SHP_COUNT];
static const int                g_buildable_types[];
#define BUILDABLE_COUNT    8
#define CREDITS_PER_SECOND 6
#define STARTING_CREDITS   600

/* Spawn `count` units of mixed unit_types around (cx,cy), spiralling
 * outward until enough passable, unoccupied tiles are found. */
static void spawn_cluster(GameState *gs, int cx, int cy, int count,
                          unsigned char faction) {
    int placed = 0;
    for (int r = 0; r <= 12 && placed < count; ++r) {
        for (int dy = -r; dy <= r && placed < count; ++dy) {
            for (int dx = -r; dx <= r && placed < count; ++dx) {
                if (abs(dx) != r && abs(dy) != r) continue;
                int tx = cx + dx, ty = cy + dy;
                if (!tile_passable(&gs->map, tx, ty)) continue;
                bool occupied = false;
                for (int i = 0; i < gs->units.count; ++i) {
                    if (gs->units.units[i].tile_x == tx &&
                        gs->units.units[i].tile_y == ty) {
                        occupied = true; break;
                    }
                }
                if (occupied) continue;
                /* Cycle only through buildable (non-factory) types. */
                unsigned char ut =
                    (unsigned char)g_buildable_types[placed % BUILDABLE_COUNT];
                units_spawn(&gs->units, tx, ty, ut, faction);
                placed++;
            }
        }
    }
}

static void spawn_initial_units(GameState *gs) {
    /* Two clusters on opposite sides of the map centre. */
    int mid_x = MAP_WIDTH  / 2;
    int mid_y = MAP_HEIGHT / 2;

    gs->factions[FACTION_GOLD].credits = STARTING_CREDITS;
    gs->factions[FACTION_GOLD].spawn_x = mid_x - 8;
    gs->factions[FACTION_GOLD].spawn_y = mid_y;
    gs->factions[FACTION_RED].credits  = STARTING_CREDITS;
    gs->factions[FACTION_RED].spawn_x  = mid_x + 8;
    gs->factions[FACTION_RED].spawn_y  = mid_y;

    /* Spawn the factory FIRST at the rally tile, then mark its 2×2
     * footprint as blocked terrain. Force a 4×3 dirt patch around the
     * anchor so the building sprite sits on uniform brown ground —
     * stops grass from showing through any transparent sprite pixels
     * (loading bays, sprite-canvas corners, etc.). */
    int gx = mid_x - 8, gy = mid_y;
    int rx = mid_x + 8, ry = mid_y;
    for (int dy = -1; dy <= 2; ++dy) {
        for (int dx = -1; dx <= 2; ++dx) {
            int gtx = gx + dx, gty = gy + dy;
            int rtx = rx + dx, rty = ry + dy;
            if (gtx >= 0 && gty >= 0 && gtx < MAP_WIDTH && gty < MAP_HEIGHT)
                gs->map.tiles[gty][gtx] = TILE_DIRT;
            if (rtx >= 0 && rty >= 0 && rtx < MAP_WIDTH && rty < MAP_HEIGHT)
                gs->map.tiles[rty][rtx] = TILE_DIRT;
        }
    }
    units_spawn(&gs->units, gx, gy, UNIT_TYPE_FACTORY, FACTION_GOLD);
    tilemap_set_blocked(&gs->map, gx, gy, 2, 2, true);
    units_spawn(&gs->units, rx, ry, UNIT_TYPE_FACTORY, FACTION_RED);
    tilemap_set_blocked(&gs->map, rx, ry, 2, 2, true);

    /* Refinery: 4 tiles to the side of each factory. 2×2 footprint. We
     * also force the perimeter passable so a deposit-tile is always
     * reachable. */
    int gref_x = gx - 4, gref_y = gy;
    int rref_x = rx + 4, rref_y = ry;
    for (int dy = -1; dy <= 2; ++dy) {
        for (int dx = -1; dx <= 2; ++dx) {
            int gx2 = gref_x + dx, gy2 = gref_y + dy;
            int rx2 = rref_x + dx, ry2 = rref_y + dy;
            if (gx2 >= 0 && gy2 >= 0 && gx2 < MAP_WIDTH && gy2 < MAP_HEIGHT)
                gs->map.tiles[gy2][gx2] = TILE_DIRT;
            if (rx2 >= 0 && ry2 >= 0 && rx2 < MAP_WIDTH && ry2 < MAP_HEIGHT)
                gs->map.tiles[ry2][rx2] = TILE_DIRT;
        }
    }
    units_spawn(&gs->units, gref_x, gref_y, UNIT_TYPE_REFINERY, FACTION_GOLD);
    tilemap_set_blocked(&gs->map, gref_x, gref_y, 2, 2, true);
    units_spawn(&gs->units, rref_x, rref_y, UNIT_TYPE_REFINERY, FACTION_RED);
    tilemap_set_blocked(&gs->map, rref_x, rref_y, 2, 2, true);

    spawn_cluster(gs, gx, gy, 8, FACTION_GOLD);
    spawn_cluster(gs, rx, ry, 8, FACTION_RED);

    /* Initialise HP from combat-info so each unit type spawns at the
     * correct max HP. units_spawn doesn't know the combat table. */
    for (int i = 0; i < gs->units.count; ++i) {
        Unit *unit = &gs->units.units[i];
        if (unit->unit_type < UNIT_SHP_COUNT) {
            unit->hp_max = g_unit_combat_info[unit->unit_type].hp_max;
            unit->hp     = unit->hp_max;
        }
    }
}

/* Find a passable, unoccupied tile within `max_r` rings of (cx,cy). */
static bool find_free_tile_near(const GameState *gs, int cx, int cy,
                                int max_r, int *out_x, int *out_y) {
    if (tile_passable(&gs->map, cx, cy) &&
        units_find_at_tile(&gs->units, cx, cy, -1) < 0) {
        *out_x = cx; *out_y = cy; return true;
    }
    for (int r = 1; r <= max_r; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (abs(dx) != r && abs(dy) != r) continue;
                int tx = cx + dx, ty = cy + dy;
                if (!tile_passable(&gs->map, tx, ty)) continue;
                if (units_find_at_tile(&gs->units, tx, ty, -1) >= 0) continue;
                *out_x = tx; *out_y = ty; return true;
            }
        }
    }
    return false;
}

/* True iff `faction` has at least one live factory. Production gates on this. */
static bool faction_has_factory(const GameState *gs, int faction) {
    for (int i = 0; i < gs->units.count; ++i) {
        const Unit *u = &gs->units.units[i];
        if (u->alive && u->faction == faction && u->unit_type == UNIT_TYPE_FACTORY)
            return true;
    }
    return false;
}

/* Try to enqueue a build of `unit_type` for `faction`. Deducts the cost
 * up-front; returns true on success. */
static bool try_enqueue_build(GameState *gs, int faction, int unit_type) {
    if (faction < 0 || faction >= UNIT_FACTION_COUNT) return false;
    if (unit_type < 0 || unit_type >= UNIT_SHP_COUNT) return false;
    FactionState *fs = &gs->factions[faction];
    if (g_unit_production_info[unit_type].credit_cost <= 0) {
        return false;  /* not buildable (factory) */
    }
    if (!faction_has_factory(gs, faction)) {
        debug_log("build_no_factory faction=%d", faction);
        return false;
    }
    if (fs->queue_count >= MAX_BUILD_QUEUE) {
        debug_log("build_queue_full faction=%d", faction);
        if (faction == FACTION_GOLD) push_status(gs, "Build queue full", 2);
        return false;
    }
    int cost = g_unit_production_info[unit_type].credit_cost;
    if (fs->credits < cost) {
        debug_log("build_insufficient faction=%d type=%d cost=%d have=%d",
                  faction, unit_type, cost, fs->credits);
        if (faction == FACTION_GOLD) push_status(gs, "Insufficient funds", 2);
        return false;
    }
    fs->credits -= cost;
    fs->queue[fs->queue_count].unit_type = (unsigned char)unit_type;
    fs->queue[fs->queue_count].elapsed_s = 0.0f;
    fs->queue_count++;
    debug_log("build_enqueued faction=%d type=%d cost=%d remaining=%d",
              faction, unit_type, cost, fs->credits);
    return true;
}

/* Spiral outward from (x,y) returning the nearest passable tile
 * (including the starting tile if it's passable). Used to convert a
 * blocked target tile (e.g. an enemy factory's anchor) into something
 * units can actually pathfind to. */
static bool nearest_passable_tile(const TileMap *map, int x, int y,
                                  int *out_x, int *out_y) {
    if (tile_passable(map, x, y)) {
        *out_x = x; *out_y = y; return true;
    }
    for (int r = 1; r <= 5; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (abs(dx) != r && abs(dy) != r) continue;
                int tx = x + dx, ty = y + dy;
                if (tile_passable(map, tx, ty)) {
                    *out_x = tx; *out_y = ty; return true;
                }
            }
        }
    }
    return false;
}

/* Find the nearest passable perimeter tile of the closest friendly
 * deposit (refinery or factory) for `faction`. Returns false if no
 * deposit exists. The deposit's anchor + 2×2 footprint is blocked so
 * we test the 8 perimeter tiles around the anchor. */
static bool find_nearest_deposit_tile(const GameState *gs, int faction,
                                      int from_x, int from_y,
                                      int *out_x, int *out_y) {
    static const int perim_dx[8] = { 0, 1, 0, 1, -1, -1,  2,  2 };
    static const int perim_dy[8] = {-1,-1, 2, 2,  0,  1,  0,  1 };

    int best_d2 = 1 << 30;
    int bx = -1, by = -1;
    for (int i = 0; i < gs->units.count; ++i) {
        const Unit *u = &gs->units.units[i];
        if (!u->alive || u->faction != faction) continue;
        if (!unit_type_is_deposit(u->unit_type)) continue;
        for (int p = 0; p < 8; ++p) {
            int tx = u->tile_x + perim_dx[p];
            int ty = u->tile_y + perim_dy[p];
            if (!tile_passable(&gs->map, tx, ty)) continue;
            int dx = tx - from_x;
            int dy = ty - from_y;
            int d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                bx = tx; by = ty;
            }
        }
    }
    if (bx < 0) return false;
    *out_x = bx; *out_y = by;
    return true;
}

/* Find the nearest TILE_ORE to (fx,fy) that isn't already claimed by
 * another harvester (`harvester_ore_{x,y}` while in GOING_TO_ORE or
 * GATHERING state). Avoids two harvesters fighting for the same tile.
 * Returns false if no ore exists. */
static bool find_nearest_ore_tile(const GameState *gs, int self_id,
                                  int fx, int fy,
                                  int *out_x, int *out_y) {
    static bool claimed[MAP_HEIGHT][MAP_WIDTH];
    for (int y = 0; y < MAP_HEIGHT; ++y)
        for (int x = 0; x < MAP_WIDTH; ++x)
            claimed[y][x] = false;

    for (int j = 0; j < gs->units.count; ++j) {
        if (j == self_id) continue;
        const Unit *o = &gs->units.units[j];
        if (!o->alive || o->unit_type != 0) continue;
        if (o->harvester_state != HARV_GOING_TO_ORE &&
            o->harvester_state != HARV_GATHERING) continue;
        int ox = o->harvester_ore_x;
        int oy = o->harvester_ore_y;
        if (ox >= 0 && ox < MAP_WIDTH && oy >= 0 && oy < MAP_HEIGHT)
            claimed[oy][ox] = true;
    }

    int best  = (1 << 30);
    int bx = -1, by = -1;
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) {
            if (gs->map.tiles[y][x] != TILE_ORE) continue;
            if (claimed[y][x]) continue;
            int dx = x - fx, dy = y - fy;
            int d = dx * dx + dy * dy;
            if (d < best) { best = d; bx = x; by = y; }
        }
    }
    if (bx < 0) return false;
    *out_x = bx; *out_y = by;
    return true;
}

/* Drive the harvester state machine for every gold/red harvester:
 *   IDLE → drive to ore → gather (3 s) → return home → unload (1 s) → +50 credits.
 * Manual user move orders interrupt by overwriting `path` and tile;
 * the state machine just notices "I'm not where I expected to be" and
 * resets to IDLE so the harvester can pick a fresh ore patch. */
static void harvester_tick(GameState *gs, float dt) {
    for (int i = 0; i < gs->units.count; ++i) {
        Unit *unit = &gs->units.units[i];
        if (!unit->alive || unit->unit_type != 0) continue;

        /* harvester_home_{x,y} is initialised at spawn to the unit's own
         * spawn tile, which is guaranteed to be passable (spawn_cluster /
         * find_free_tile_near pick passable tiles only). No lazy init. */

        bool moving = (unit->next_x != unit->tile_x) ||
                      (unit->next_y != unit->tile_y);

        switch (unit->harvester_state) {
        case HARV_IDLE: {
            if (moving || unit->path_len > 0) break;
            int ox, oy;
            if (find_nearest_ore_tile(gs, i, unit->tile_x, unit->tile_y,
                                      &ox, &oy)) {
                if (units_order_move_one(&gs->units, &gs->map, i, ox, oy)) {
                    unit->harvester_ore_x = (short)ox;
                    unit->harvester_ore_y = (short)oy;
                    unit->harvester_state = HARV_GOING_TO_ORE;
                    debug_log("harv id=%d -> ore (%d,%d)", unit->id, ox, oy);
                }
            }
            break;
        }
        case HARV_GOING_TO_ORE: {
            if (unit->tile_x == unit->harvester_ore_x &&
                unit->tile_y == unit->harvester_ore_y &&
                !moving) {
                unit->harvester_state = HARV_GATHERING;
                unit->harvester_acc   = 0.0f;
                debug_log("harv id=%d gathering", unit->id);
            } else if (unit->path_len == 0 && !moving) {
                /* Path failed or was overwritten by a manual order that
                 * arrived elsewhere — give up and re-pick. */
                unit->harvester_state = HARV_IDLE;
            }
            break;
        }
        case HARV_GATHERING: {
            unit->harvester_acc += dt;
            if (unit->harvester_acc >= 3.0f) {
                /* Deplete the ore tile we're standing on — visible patch
                 * shrinks each cycle. */
                int ox = unit->harvester_ore_x;
                int oy = unit->harvester_ore_y;
                if (ox >= 0 && ox < MAP_WIDTH &&
                    oy >= 0 && oy < MAP_HEIGHT &&
                    gs->map.tiles[oy][ox] == TILE_ORE) {
                    gs->map.tiles[oy][ox] = TILE_DIRT;
                    tilemap_repaint_tile(&gs->map, ox, oy);
                }
                /* Deposit at the nearest friendly refinery (preferred)
                 * or factory (fallback). If both are dead the
                 * harvester gives up and goes idle. */
                int dep_x, dep_y;
                if (find_nearest_deposit_tile(gs, unit->faction,
                                              unit->tile_x, unit->tile_y,
                                              &dep_x, &dep_y) &&
                    units_order_move_one(&gs->units, &gs->map, i,
                                         dep_x, dep_y)) {
                    unit->harvester_home_x = (short)dep_x;
                    unit->harvester_home_y = (short)dep_y;
                    unit->harvester_state  = HARV_RETURNING;
                    debug_log("harv id=%d returning to (%d,%d)",
                              unit->id, dep_x, dep_y);
                } else {
                    unit->harvester_state = HARV_IDLE;
                }
            }
            break;
        }
        case HARV_RETURNING: {
            int dxh = unit->tile_x - unit->harvester_home_x;
            int dyh = unit->tile_y - unit->harvester_home_y;
            if (dxh * dxh + dyh * dyh <= 4 && !moving) {
                unit->harvester_state = HARV_UNLOADING;
                unit->harvester_acc   = 0.0f;
                /* Stop wherever we ended up. */
                unit->path_len = 0;
                unit->path_idx = 0;
            } else if (unit->path_len == 0 && !moving) {
                /* Re-issue path home in case original was interrupted. */
                if (!units_order_move_one(&gs->units, &gs->map, i,
                                          unit->harvester_home_x,
                                          unit->harvester_home_y)) {
                    unit->harvester_state = HARV_IDLE;
                }
            }
            break;
        }
        case HARV_UNLOADING: {
            unit->harvester_acc += dt;
            if (unit->harvester_acc >= 1.0f) {
                gs->factions[unit->faction].credits += 50;
                debug_log("harv id=%d deposited +50 (faction=%d total=%d)",
                          unit->id, unit->faction,
                          gs->factions[unit->faction].credits);
                if (gs->audio_loaded && unit->faction == FACTION_GOLD) {
                    SetSoundPitch(gs->sound_deposit, 1.0f);
                    PlaySound(gs->sound_deposit);
                }
                unit->harvester_state = HARV_IDLE;
            }
            break;
        }
        default:
            unit->harvester_state = HARV_IDLE;
            break;
        }
    }
}

/* Recompute fog-of-war visibility from gold units. Tiles within
 * VISION_RADIUS_TILES of any live gold unit go to VIS_VISIBLE; tiles
 * that were VIS_VISIBLE but no longer are go to VIS_DISCOVERED. */
static void recompute_visibility(GameState *gs) {
    /* Demote anything currently VIS_VISIBLE to DISCOVERED before
     * recomputing — we'll re-promote what's still in sight below. */
    for (int y = 0; y < MAP_HEIGHT; ++y) {
        for (int x = 0; x < MAP_WIDTH; ++x) {
            if (gs->visibility[y][x] == VIS_VISIBLE)
                gs->visibility[y][x] = VIS_DISCOVERED;
        }
    }

    int r = VISION_RADIUS_TILES;
    int r2 = r * r;
    for (int i = 0; i < gs->units.count; ++i) {
        const Unit *unit = &gs->units.units[i];
        if (!unit->alive || unit->faction != FACTION_GOLD) continue;
        int ux = unit->tile_x, uy = unit->tile_y;
        int x0 = ux - r, x1 = ux + r;
        int y0 = uy - r, y1 = uy + r;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > MAP_WIDTH  - 1) x1 = MAP_WIDTH  - 1;
        if (y1 > MAP_HEIGHT - 1) y1 = MAP_HEIGHT - 1;
        for (int y = y0; y <= y1; ++y) {
            int dy = y - uy;
            for (int x = x0; x <= x1; ++x) {
                int dx = x - ux;
                if (dx * dx + dy * dy <= r2)
                    gs->visibility[y][x] = VIS_VISIBLE;
            }
        }
    }
}

/* Drive credit trickle and per-faction build queues. */
static void update_economy(GameState *gs, float dt) {
    gs->credits_acc += dt * (float)CREDITS_PER_SECOND;
    while (gs->credits_acc >= 1.0f) {
        gs->credits_acc -= 1.0f;
        for (int f = 0; f < UNIT_FACTION_COUNT; ++f) gs->factions[f].credits++;
    }

    for (int f = 0; f < UNIT_FACTION_COUNT; ++f) {
        FactionState *fs = &gs->factions[f];
        if (fs->queue_count == 0) continue;
        BuildOrder *head = &fs->queue[0];
        head->elapsed_s += dt;
        float btime = g_unit_production_info[head->unit_type].build_time_s;
        if (head->elapsed_s >= btime) {
            int sx, sy;
            if (find_free_tile_near(gs, fs->spawn_x, fs->spawn_y, 6, &sx, &sy)) {
                int id = units_spawn(&gs->units, sx, sy,
                                     head->unit_type, (unsigned char)f);
                if (id >= 0) {
                    Unit *unit = &gs->units.units[id];
                    unit->hp_max = g_unit_combat_info[head->unit_type].hp_max;
                    unit->hp     = unit->hp_max;
                    debug_log("build_complete faction=%d type=%d at=(%d,%d)",
                              f, head->unit_type, sx, sy);
                    if (gs->audio_loaded && f == FACTION_GOLD)
                        PlaySound(gs->sound_build_done);
                    if (f == FACTION_GOLD)
                        push_status(gs, "Unit ready", 1);
                    /* Walk newly produced units to the faction rally
                     * point if one is set. Skip for buildings (none
                     * are produced via the queue today, but defensive)
                     * and for harvesters — they have their own
                     * IDLE→ore loop and shouldn't be diverted. */
                    if (fs->rally_set &&
                        !unit_type_is_stationary(head->unit_type) &&
                        head->unit_type != 0 /* harvester */) {
                        units_order_move_one(&gs->units, &gs->map, id,
                                             fs->rally_x, fs->rally_y);
                    }
                }
            } else {
                debug_log("build_no_spawn_tile faction=%d type=%d",
                          f, head->unit_type);
            }
            /* Pop head: shift queue. */
            for (int i = 1; i < fs->queue_count; ++i)
                fs->queue[i - 1] = fs->queue[i];
            fs->queue_count--;
        }
    }
}

static const char *g_decoration_paths[DECORATION_TMP_COUNT] = {
    "third_party/OpenRA/mods/ra/bits/deca.tem",
    "third_party/OpenRA/mods/ra/bits/decb.tem",
    "third_party/OpenRA/mods/ra/bits/decc.tem",
    "third_party/OpenRA/mods/ra/bits/decd.tem",
    "third_party/OpenRA/mods/ra/bits/dece.tem",
    "third_party/OpenRA/mods/ra/bits/decf.tem",
    "third_party/OpenRA/mods/ra/bits/decg.tem",
    "third_party/OpenRA/mods/ra/bits/dech.tem",
};

/* "mb" = "map building": civilian/inert SHP_TD-format structure stamps
 * in cnc/bits. Despite the .tem extension, these are SHP_TD files
 * (single-frame, 24×24 LCW-compressed sprites). */
static const char *g_building_paths[BUILDING_SHP_COUNT] = {
    "third_party/OpenRA/mods/cnc/bits/mbGTWR.tem",
    "third_party/OpenRA/mods/cnc/bits/mbHOSP.tem",
    "third_party/OpenRA/mods/cnc/bits/mbSILO.tem",
    "third_party/OpenRA/mods/cnc/bits/mbFIX.tem",
    "third_party/OpenRA/mods/cnc/bits/mbMISS.tem",
};

/* Three RA unit SHPs: harvester (32-facing vehicle, ~111 frames),
 * flamethrower truck (32-facing vehicle), and infantry (8-facing).
 * Infantry SHPs lay out frames as [stand x 8][walk1 x 8][walk2 x 8]...
 * so walk_anim_offset = 8 (stand) and walk_frame_count = 6 typical. */
static const char *g_unit_sprite_paths[UNIT_SHP_COUNT] = {
    "third_party/OpenRA/mods/ra/bits/harv.shp",
    "third_party/OpenRA/mods/ra/bits/ftrk.shp",
    "third_party/OpenRA/mods/ra/bits/e6.shp",
    "third_party/OpenRA/mods/ra/bits/fact.shp",
    "third_party/OpenRA/mods/ra/bits/sam2.shp",
    "third_party/OpenRA/mods/ra/bits/silo2.shp",
    /* Mammoth Tank — sourced from the redhorizon submodule's test
     * fixtures since OpenRA's vendored bits don't include it. 32
     * facings, multiple state animations. */
    "third_party/redhorizon/redhorizon-classic/test/nz/net/ultraq/redhorizon/classic/filetypes/ImageDecoders_SpriteSheet_4tnk.shp",
    /* Additional units from the raplusmod submodule. */
    "third_party/raplusmod/mods/raplus/bits/infantry/e1.shp",       /* rifle inf */
    "third_party/raplusmod/mods/raplus/bits/vehicle/gtnk.shp",      /* Grizzly tank */
};
static const UnitSpriteInfo g_unit_sprite_info[UNIT_SHP_COUNT] = {
    /* facings, walk_count, walk_offset, default_frame */
    { 32, 0, 0, 0 },   /* harvester: 32 facings */
    { 32, 0, 0, 0 },   /* flame truck: 32 facings */
    {  8, 6, 8, 0 },   /* e6 infantry */
    {  1, 0, 0, 0 },   /* factory: idle frame 0 */
    { 32, 0, 0, 0 },   /* sam2 turret */
    {  1, 0, 0, 0 },   /* silo refinery */
    { 32, 0, 0, 0 },   /* mammoth tank: 32 facings */
    {  8, 6, 8, 0 },   /* e1 rifle infantry */
    { 32, 0, 0, 0 },   /* gtnk grizzly tank: 32 facings */
};

const UnitCombatInfo g_unit_combat_info[UNIT_SHP_COUNT] = {
    /* hp_max, dmg, range_tiles, cooldown_s */
    { 200,  0, 0.0f, 1.0f },   /* harvester: tough, defenseless */
    { 120, 18, 3.5f, 0.7f },   /* flame truck: short range, hard hit */
    {  60,  6, 5.0f, 0.5f },   /* infantry: longer range, lighter dmg */
    { 800,  0, 0.0f, 1.0f },   /* factory: very tough, doesn't fight */
    { 220, 14, 6.0f, 1.0f },   /* turret: long range, slow cadence, sturdy */
    { 600,  0, 0.0f, 1.0f },   /* refinery: tough, no attack */
    { 400, 28, 4.5f, 1.4f },   /* mammoth: heavy hp, big damage, slow cadence */
    {  50,  8, 5.5f, 0.4f },   /* e1 rifle: cheap, decent range, fast cadence */
    { 220, 16, 4.0f, 1.0f },   /* grizzly: medium tank, balanced */
};

static const UnitProductionInfo g_unit_production_info[UNIT_SHP_COUNT] = {
    /* cost, build_time_s — factory cost=0 marks it as not buildable */
    { 800, 22.0f },   /* harvester */
    { 600, 16.0f },   /* flame truck */
    { 100,  6.0f },   /* infantry */
    {   0,  0.0f },   /* factory: spawns with the faction, not buildable */
    { 700, 12.0f },   /* turret: defensive structure */
    { 1500, 25.0f },  /* refinery: expensive but extends harvester economy */
    { 1200, 28.0f },  /* mammoth tank: heavy investment, late-game punch */
    {   80,  5.0f },  /* e1 rifle infantry: cheapest unit */
    {  700, 14.0f },  /* grizzly tank: mid-tier vehicle */
};

/* Subset of types the player / AI can build via HUD buttons / auto-build.
 * Factory is excluded; it spawns with the faction and is irreplaceable.
 * BUILDABLE_COUNT is defined alongside the forward-decl up top. */
static const int g_buildable_types[BUILDABLE_COUNT] = {
    0, 1, 2, 4, 5, 6, 7, 8
};
static const char *g_unit_short_names[UNIT_SHP_COUNT] = {
    "HARV", "FTRK", "INF", "FACT", "TUR", "REF", "MAM", "RIFL", "GRIZ",
};

/* Team-colour ramps for palette indices 80..95 (Westwood remap range).
 * Dark to light, 16 shades each. */
static const Color g_team_ramp_gold[16] = {
    { 40, 32,  8, 255}, { 64, 48, 12, 255}, { 88, 68, 16, 255}, {116, 88, 24, 255},
    {148,112, 36, 255}, {180,140, 52, 255}, {204,168, 72, 255}, {220,192, 92, 255},
    {236,212,116, 255}, {244,228,144, 255}, {252,240,172, 255}, {252,244,196, 255},
    {252,248,216, 255}, {252,252,232, 255}, {252,252,244, 255}, {252,252,252, 255},
};
static const Color g_team_ramp_red[16] = {
    { 40,  0,  0, 255}, { 72,  0,  0, 255}, {104,  0,  0, 255}, {140,  4,  4, 255},
    {172, 12, 12, 255}, {200, 24, 24, 255}, {224, 44, 44, 255}, {244, 72, 72, 255},
    {252,104,104, 255}, {252,132,132, 255}, {252,160,160, 255}, {252,184,184, 255},
    {252,208,208, 255}, {252,224,224, 255}, {252,240,240, 255}, {252,252,252, 255},
};
static const Color *g_team_ramps[UNIT_FACTION_COUNT] = {
    g_team_ramp_gold,
    g_team_ramp_red,
};

static void load_assets(GameState *gs) {
    const char *pal_path = "third_party/OpenRA/mods/ra/maps/chernobyl/temperat.pal";

    if (palette_load(&gs->palette, pal_path)) {
        gs->palette_loaded = true;
        debug_log("palette_loaded path=%s", pal_path);
    } else {
        debug_log("palette_load_fail path=%s", pal_path);
    }

    if (gs->palette_loaded) {
        /* Build per-faction palettes by remapping indices 80..95. */
        for (int f = 0; f < UNIT_FACTION_COUNT; ++f) {
            palette_remap_team(&gs->palette, &gs->team_palettes[f],
                               g_team_ramps[f]);
        }

        /* Effect sprites: explosion (death) and muzzle flash (firing). */
        if (shp_load(&gs->effect_sprites[EFFECT_KIND_EXPLOSION],
                     "third_party/OpenRA/mods/ra/bits/napalm1.shp",
                     &gs->palette)) {
            gs->effect_sprite_loaded[EFFECT_KIND_EXPLOSION] = true;
        }
        if (shp_load(&gs->effect_sprites[EFFECT_KIND_MUZZLE],
                     "third_party/OpenRA/mods/cnc/bits/gunfire2.shp",
                     &gs->palette)) {
            gs->effect_sprite_loaded[EFFECT_KIND_MUZZLE] = true;
        }

        /* Load each unit SHP once per faction. Layout matches
         * unit_sprites[type * UNIT_FACTION_COUNT + faction]. */
        for (int t = 0; t < UNIT_SHP_COUNT; ++t) {
            for (int f = 0; f < UNIT_FACTION_COUNT; ++f) {
                int slot = t * UNIT_FACTION_COUNT + f;
                shp_load(&gs->unit_sprites[slot], g_unit_sprite_paths[t],
                         &gs->team_palettes[f]);
            }
        }

        for (int i = 0; i < DECORATION_TMP_COUNT; ++i) {
            if (tmp_load(&gs->decoration_tmps[i], g_decoration_paths[i],
                         &gs->palette)) {
                gs->decoration_tmp_loaded[i] = true;
            }
        }
        for (int i = 0; i < BUILDING_SHP_COUNT; ++i) {
            if (shp_load(&gs->building_shps[i], g_building_paths[i],
                         &gs->palette)) {
                gs->building_shp_loaded[i] = true;
            }
        }
    }
}

/* Tiny xorshift32 for deterministic decoration placement. */
static unsigned int xs_next(unsigned int *s) {
    unsigned int x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 1;
    return *s;
}

static void place_decorations(GameState *gs) {
    gs->decoration_count = 0;

    int loaded_indices[DECORATION_TMP_COUNT];
    int loaded_n = 0;
    for (int i = 0; i < DECORATION_TMP_COUNT; ++i)
        if (gs->decoration_tmp_loaded[i]) loaded_indices[loaded_n++] = i;
    if (loaded_n == 0) return;

    unsigned int seed = 0xC0FFEE42u;
    int target = MAX_DECORATIONS;
    int attempts = 0;
    while (gs->decoration_count < target && attempts < target * 8) {
        attempts++;
        int tx = (int)(xs_next(&seed) % MAP_WIDTH);
        int ty = (int)(xs_next(&seed) % MAP_HEIGHT);
        TileType t = gs->map.tiles[ty][tx];
        if (t != TILE_GRASS && t != TILE_DIRT) continue;

        /* Don't drop decorations on top of the spawn cluster. */
        int dx = tx - MAP_WIDTH / 2;
        int dy = ty - MAP_HEIGHT / 2;
        if (dx * dx + dy * dy < 16) continue;

        int loaded_pick = (int)(xs_next(&seed) % (unsigned)loaded_n);
        int tmp_idx = loaded_indices[loaded_pick];
        const TmpSprite *tmp = &gs->decoration_tmps[tmp_idx];

        int valid_frames[64];
        int valid_n = 0;
        for (int f = 0; f < tmp->frame_count && valid_n < 64; ++f)
            if (tmp->frames[f].id != 0) valid_frames[valid_n++] = f;
        if (valid_n == 0) continue;

        int frame_pick = valid_frames[(int)(xs_next(&seed) % (unsigned)valid_n)];

        Decoration *d = &gs->decorations[gs->decoration_count++];
        d->tile_x      = tx;
        d->tile_y      = ty;
        d->tmp_index   = (unsigned char)tmp_idx;
        d->frame_index = (unsigned char)frame_pick;
        d->jitter_x    = (signed char)((int)(xs_next(&seed) % 9) - 4);
        d->jitter_y    = (signed char)((int)(xs_next(&seed) % 9) - 4);
    }
    debug_log("place_decorations placed=%d loaded_tmps=%d",
              gs->decoration_count, loaded_n);
}

static void place_structures(GameState *gs) {
    int loaded_indices[BUILDING_SHP_COUNT];
    int loaded_n = 0;
    for (int i = 0; i < BUILDING_SHP_COUNT; ++i)
        if (gs->building_shp_loaded[i]) loaded_indices[loaded_n++] = i;
    if (loaded_n == 0) return;

    unsigned int seed = 0xDEADBEEFu;
    int target = MAX_STRUCTURES;
    int attempts = 0;
    while (gs->structure_count < target && attempts < target * 32) {
        attempts++;
        int tx = (int)(xs_next(&seed) % MAP_WIDTH);
        int ty = (int)(xs_next(&seed) % MAP_HEIGHT);
        TileType t = gs->map.tiles[ty][tx];
        if (t != TILE_GRASS && t != TILE_DIRT) continue;

        /* Keep clear of the unit spawn cluster. */
        int dx = tx - MAP_WIDTH / 2;
        int dy = ty - MAP_HEIGHT / 2;
        if (dx * dx + dy * dy < 49) continue;

        /* No overlap with another structure (min separation ~5 tiles). */
        bool too_close = false;
        for (int j = 0; j < gs->structure_count; ++j) {
            int sdx = tx - gs->structures[j].tile_x;
            int sdy = ty - gs->structures[j].tile_y;
            if (sdx * sdx + sdy * sdy < 25) { too_close = true; break; }
        }
        if (too_close) continue;

        int pick = (int)(xs_next(&seed) % (unsigned)loaded_n);
        Structure *s = &gs->structures[gs->structure_count++];
        s->tile_x   = tx;
        s->tile_y   = ty;
        s->shp_index = (unsigned char)loaded_indices[pick];
    }
    debug_log("place_structures placed=%d", gs->structure_count);
}

void game_init(GameState *gs, int screen_w, int screen_h) {
    /* Preserve cursor texture and audio sounds across restarts so we
     * don't re-init the audio device or reload the same PNG. */
    Texture2D saved_cursor        = gs->cursor_tex;
    bool      saved_cursor_loaded = gs->cursor_loaded;
    Sound     saved_sel  = gs->sound_select;
    Sound     saved_done = gs->sound_build_done;
    Sound     saved_exp  = gs->sound_explosion;
    Sound     saved_dep  = gs->sound_deposit;
    Sound     saved_alm  = gs->sound_alarm;
    bool      saved_audio_loaded  = gs->audio_loaded;

    memset(gs, 0, sizeof(*gs));

    gs->cursor_tex       = saved_cursor;
    gs->cursor_loaded    = saved_cursor_loaded;
    gs->sound_select     = saved_sel;
    gs->sound_build_done = saved_done;
    gs->sound_explosion  = saved_exp;
    gs->sound_deposit    = saved_dep;
    gs->sound_alarm      = saved_alm;
    gs->audio_loaded     = saved_audio_loaded;
    gs->screen_w         = screen_w;
    gs->screen_h         = screen_h;
    gs->start_time       = GetTime();

    /* Auto-screenshot scheduling — actual camera reframing happens
     * AFTER the default camera init below, so it isn't overwritten. */
    const char *ss_env = getenv("IRON_CONQUER_AUTO_SCREENSHOT");
    if (ss_env && *ss_env) {
        double secs = atof(ss_env);
        if (secs > 0.0) {
            gs->screenshot_at   = GetTime() + secs;
            gs->screenshot_exit = true;
            debug_log("auto_screenshot scheduled at +%.1fs", secs);
        }
    }

    /* Sprite browser mode via env var. Names map to unit_type indices:
     *   harv=0, ftrk=1, e6/inf=2, fact=3, sam2/tur=4, weap3/ref=5
     * Initial frame and faction are also overridable. */
    const char *view_env = getenv("IRON_CONQUER_VIEW_SHP");
    if (view_env && *view_env) {
        gs->sprite_browser = true;
        const char *fr_env = getenv("IRON_CONQUER_VIEW_FRAME");
        gs->sprite_browser_frame = (fr_env && *fr_env) ? atoi(fr_env) : 0;
        const char *fac_env = getenv("IRON_CONQUER_VIEW_FACTION");
        gs->sprite_browser_faction = (fac_env && *fac_env) ? atoi(fac_env) : 0;
        if      (strcmp(view_env, "harv") == 0) gs->sprite_browser_type = 0;
        else if (strcmp(view_env, "ftrk") == 0) gs->sprite_browser_type = 1;
        else if (strcmp(view_env, "e6") == 0 ||
                 strcmp(view_env, "inf") == 0)  gs->sprite_browser_type = 2;
        else if (strcmp(view_env, "fact") == 0) gs->sprite_browser_type = 3;
        else if (strcmp(view_env, "sam2") == 0 ||
                 strcmp(view_env, "tur") == 0)  gs->sprite_browser_type = 4;
        else if (strcmp(view_env, "weap3") == 0 ||
                 strcmp(view_env, "ref") == 0)  gs->sprite_browser_type = 5;
        else if (strcmp(view_env, "4tnk") == 0 ||
                 strcmp(view_env, "mam") == 0 ||
                 strcmp(view_env, "tank") == 0) gs->sprite_browser_type = 6;
        else if (strcmp(view_env, "e1") == 0 ||
                 strcmp(view_env, "rifl") == 0) gs->sprite_browser_type = 7;
        else if (strcmp(view_env, "gtnk") == 0 ||
                 strcmp(view_env, "griz") == 0) gs->sprite_browser_type = 8;
        else {
            int t = atoi(view_env);
            gs->sprite_browser_type =
                (t >= 0 && t < UNIT_SHP_COUNT) ? t : 5;
        }
        debug_log("sprite_browser type=%d", gs->sprite_browser_type);
    }

    tilemap_init(&gs->map, 1337u);
    units_init(&gs->units);
    spawn_initial_units(gs);
    load_assets(gs);
    tilemap_build_texture(&gs->map);
    place_decorations(gs);
    place_structures(gs);

    /* Audio: synthesise short beeps once, lazily. Init the audio device
     * lazily too — we may be the first to use it after window init. */
    if (!gs->audio_loaded) {
        InitAudioDevice();
        if (IsAudioDeviceReady()) {
            Wave w_sel  = synth_beep(740.0f, 740.0f, 0.06f, 0.55f);
            Wave w_done = synth_beep(440.0f, 880.0f, 0.18f, 0.55f);
            Wave w_exp  = synth_noise(0.40f, 0.85f);
            Wave w_dep  = synth_beep(900.0f, 1300.0f, 0.10f, 0.45f);
            Wave w_alm  = synth_beep(440.0f, 220.0f, 0.40f, 0.60f);
            if (w_sel.data)  gs->sound_select     = LoadSoundFromWave(w_sel);
            if (w_done.data) gs->sound_build_done = LoadSoundFromWave(w_done);
            if (w_exp.data)  gs->sound_explosion  = LoadSoundFromWave(w_exp);
            if (w_dep.data)  gs->sound_deposit    = LoadSoundFromWave(w_dep);
            if (w_alm.data)  gs->sound_alarm      = LoadSoundFromWave(w_alm);
            UnloadWave(w_sel);
            UnloadWave(w_done);
            UnloadWave(w_exp);
            UnloadWave(w_dep);
            UnloadWave(w_alm);
            gs->audio_loaded = true;
            debug_log("audio_loaded synthesized");
        }
    }

    /* Cursor is loaded once, lazily. raylib has been initialised by now. */
    if (!gs->cursor_loaded) {
        Image img = LoadImage("third_party/OpenRA/mods/common-content/cursor.png");
        if (img.data) {
            gs->cursor_tex    = LoadTextureFromImage(img);
            gs->cursor_loaded = true;
            UnloadImage(img);
            HideCursor();
            debug_log("cursor_loaded %dx%d", gs->cursor_tex.width,
                      gs->cursor_tex.height);
        }
    }

    gs->camera.target = (Vector2){
        (MAP_WIDTH  * TILE_SIZE) * 0.5f,
        (MAP_HEIGHT * TILE_SIZE) * 0.5f,
    };
    gs->camera.offset   = (Vector2){ screen_w * 0.5f, screen_h * 0.5f };
    gs->camera.rotation = 0.0f;
    gs->camera.zoom     = 1.0f;
    gs->pan_speed       = 600.0f;

    /* If we're in auto-screenshot mode, override the default camera so
     * the gold base is centred and zoomed in. Done AFTER the default
     * init so it actually sticks. Also mark all gold units selected so
     * verification screenshots show the selection visuals. */
    if (gs->screenshot_at > 0.0) {
        gs->camera.target.x =
            ((float)gs->factions[FACTION_GOLD].spawn_x + 0.5f) *
            (float)TILE_SIZE;
        gs->camera.target.y =
            ((float)gs->factions[FACTION_GOLD].spawn_y + 0.5f) *
            (float)TILE_SIZE;
        gs->camera.zoom = 2.8f;
        for (int i = 0; i < gs->units.count; ++i) {
            Unit *uu = &gs->units.units[i];
            if (uu->alive && uu->faction == FACTION_GOLD)
                uu->selected = true;
        }
    }

    int sprites_loaded = 0;
    for (int i = 0; i < UNIT_SHP_COUNT; ++i)
        if (gs->unit_sprites[i].frame_count > 0) sprites_loaded++;
    debug_log("init map=%dx%d tile=%d screen=%dx%d sim_hz=%d units=%d "
              "palette=%d unit_sprites=%d/%d structures=%d decorations=%d",
              MAP_WIDTH, MAP_HEIGHT, TILE_SIZE, screen_w, screen_h,
              SIM_HZ, gs->units.count,
              gs->palette_loaded ? 1 : 0,
              sprites_loaded, UNIT_SHP_COUNT,
              gs->structure_count, gs->decoration_count);
}

void game_post_draw(GameState *gs) {
    if (gs->screenshot_at > 0.0 && GetTime() >= gs->screenshot_at) {
        TakeScreenshot("screenshot.png");
        debug_log("screenshot_saved at=%.2fs path=screenshot.png exit=%d",
                  GetTime() - gs->start_time, gs->screenshot_exit ? 1 : 0);
        gs->screenshot_at = 0.0;
        if (gs->screenshot_exit) gs->should_quit = true;
    }
}

void game_shutdown(GameState *gs) {
    int total = UNIT_SHP_COUNT * UNIT_FACTION_COUNT;
    for (int i = 0; i < total; ++i) {
        if (gs->unit_sprites[i].frame_count > 0) {
            shp_unload(&gs->unit_sprites[i]);
        }
    }
    for (int i = 0; i < DECORATION_TMP_COUNT; ++i) {
        if (gs->decoration_tmp_loaded[i]) {
            tmp_unload(&gs->decoration_tmps[i]);
            gs->decoration_tmp_loaded[i] = false;
        }
    }
    for (int i = 0; i < BUILDING_SHP_COUNT; ++i) {
        if (gs->building_shp_loaded[i]) {
            shp_unload(&gs->building_shps[i]);
            gs->building_shp_loaded[i] = false;
        }
    }
    for (int k = 0; k < EFFECT_KIND_COUNT; ++k) {
        if (gs->effect_sprite_loaded[k]) {
            shp_unload(&gs->effect_sprites[k]);
            gs->effect_sprite_loaded[k] = false;
        }
    }
    if (gs->cursor_loaded) {
        UnloadTexture(gs->cursor_tex);
        gs->cursor_loaded = false;
    }
    if (gs->audio_loaded) {
        UnloadSound(gs->sound_select);
        UnloadSound(gs->sound_build_done);
        UnloadSound(gs->sound_explosion);
        UnloadSound(gs->sound_deposit);
        UnloadSound(gs->sound_alarm);
        CloseAudioDevice();
        gs->audio_loaded = false;
    }
    tilemap_unload_texture(&gs->map);
}

static void draw_decorations(const GameState *gs) {
    for (int i = 0; i < gs->decoration_count; ++i) {
        const Decoration *d = &gs->decorations[i];
        if (!gs->decoration_tmp_loaded[d->tmp_index]) continue;
        const TmpSprite *tmp = &gs->decoration_tmps[d->tmp_index];
        if (d->frame_index >= tmp->frame_count) continue;
        Texture2D tex = tmp->frames[d->frame_index];
        if (tex.id == 0) continue;

        float cx = (d->tile_x + 0.5f) * (float)TILE_SIZE + (float)d->jitter_x;
        float cy = (d->tile_y + 0.5f) * (float)TILE_SIZE + (float)d->jitter_y;
        Vector2 origin = {
            cx - tmp->width  * 0.5f,
            cy - tmp->height * 0.5f,
        };
        DrawTextureV(tex, origin, WHITE);
    }
}

static void draw_structures(const GameState *gs) {
    for (int i = 0; i < gs->structure_count; ++i) {
        const Structure *s = &gs->structures[i];
        if (!gs->building_shp_loaded[s->shp_index]) continue;
        const ShpSprite *shp = &gs->building_shps[s->shp_index];
        if (shp->frame_count == 0 || !shp->frames) continue;
        Texture2D tex = shp->frames[0];
        if (tex.id == 0) continue;

        float cx = (s->tile_x + 0.5f) * (float)TILE_SIZE;
        float cy = (s->tile_y + 0.5f) * (float)TILE_SIZE;
        Vector2 origin = {
            cx - shp->width  * 0.5f,
            cy - shp->height * 0.5f,
        };
        DrawTextureV(tex, origin, WHITE);
    }
}

static void log_key_edges(void) {
    static const struct { int key; const char *name; } keys[] = {
        { KEY_W, "W" }, { KEY_A, "A" }, { KEY_S, "S" }, { KEY_D, "D" },
        { KEY_UP, "UP" }, { KEY_DOWN, "DOWN" },
        { KEY_LEFT, "LEFT" }, { KEY_RIGHT, "RIGHT" },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (IsKeyPressed(keys[i].key))  debug_log("input key_down=%s", keys[i].name);
        if (IsKeyReleased(keys[i].key)) debug_log("input key_up=%s",   keys[i].name);
    }
}

static void update_camera(GameState *gs, float dt) {
    /* Hold the camera steady while waiting for an auto-screenshot —
     * otherwise edge-of-screen scroll (triggered when the headless
     * launcher's mouse cursor sits at a window edge) drifts the view
     * before capture. */
    if (gs->screenshot_at > 0.0 && gs->screenshot_exit) {
        gs->camera.offset = (Vector2){
            GetScreenWidth()  * 0.5f,
            GetScreenHeight() * 0.5f,
        };
        return;
    }

    Vector2 dir = { 0 };
    /* Arrow keys pan; WASD removed to free S for "stop" and avoid the
     * RTS-classic Stop-vs-pan conflict. */
    if (IsKeyDown(KEY_UP))    dir.y -= 1.0f;
    if (IsKeyDown(KEY_DOWN))  dir.y += 1.0f;
    if (IsKeyDown(KEY_LEFT))  dir.x -= 1.0f;
    if (IsKeyDown(KEY_RIGHT)) dir.x += 1.0f;

    /* Edge-of-screen scroll: cursor within `margin` pixels of an edge
     * pans the camera. Standard RTS muscle memory. */
    const int margin = 16;
    Vector2 m = GetMousePosition();
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    if (m.x >= 0 && m.x < margin)         dir.x -= 1.0f;
    else if (m.x > sw - margin && m.x <= sw) dir.x += 1.0f;
    if (m.y >= 0 && m.y < margin)         dir.y -= 1.0f;
    else if (m.y > sh - margin && m.y <= sh) dir.y += 1.0f;

    gs->camera.target.x += dir.x * gs->pan_speed * dt / gs->camera.zoom;
    gs->camera.target.y += dir.y * gs->pan_speed * dt / gs->camera.zoom;

    float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
        float prev = gs->camera.zoom;
        gs->camera.zoom = clampf(gs->camera.zoom + wheel * 0.1f, 0.5f, 3.0f);
        if (gs->camera.zoom != prev)
            debug_log("input wheel=%+.2f zoom=%.2f", wheel, gs->camera.zoom);
    }

    float map_w = MAP_WIDTH  * TILE_SIZE;
    float map_h = MAP_HEIGHT * TILE_SIZE;
    gs->camera.target.x = clampf(gs->camera.target.x, 0.0f, map_w);
    gs->camera.target.y = clampf(gs->camera.target.y, 0.0f, map_h);

    gs->camera.offset = (Vector2){
        GetScreenWidth()  * 0.5f,
        GetScreenHeight() * 0.5f,
    };
}

#define MINIMAP_SIZE   256
#define MINIMAP_MARGIN 12
#define BUILD_BTN_W    72
#define BUILD_BTN_H    60
#define BUILD_BTN_X    10
#define BUILD_BTN_Y0   84
#define BUILD_BTN_COLS 2
#define BUILD_BTN_GAP  3

static Rectangle build_button_rect(int idx) {
    int col = idx % BUILD_BTN_COLS;
    int row = idx / BUILD_BTN_COLS;
    return (Rectangle){
        BUILD_BTN_X + col * (BUILD_BTN_W + BUILD_BTN_GAP),
        BUILD_BTN_Y0 + row * (BUILD_BTN_H + BUILD_BTN_GAP),
        BUILD_BTN_W,
        BUILD_BTN_H,
    };
}

static Rectangle minimap_rect(void) {
    return (Rectangle){
        MINIMAP_MARGIN,
        GetScreenHeight() - MINIMAP_SIZE - MINIMAP_MARGIN,
        MINIMAP_SIZE,
        MINIMAP_SIZE,
    };
}

static bool point_in_rect(Vector2 p, Rectangle r) {
    return p.x >= r.x && p.x <= r.x + r.width &&
           p.y >= r.y && p.y <= r.y + r.height;
}

static void update_mouse_input(GameState *gs) {
    Vector2 mouse_screen = GetMousePosition();

    /* Build buttons: L-click enqueues, R-click cancels the last queued
     * of that type with a 50% refund. Both swallow the click so it
     * doesn't fall through to selection / order paths. */
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        for (int i = 0; i < BUILDABLE_COUNT; ++i) {
            Rectangle br = build_button_rect(i);
            if (point_in_rect(mouse_screen, br)) {
                try_enqueue_build(gs, FACTION_GOLD, g_buildable_types[i]);
                gs->drag_button_down = false;
                gs->drag_active      = false;
                return;
            }
        }
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        for (int i = 0; i < BUILDABLE_COUNT; ++i) {
            Rectangle br = build_button_rect(i);
            if (point_in_rect(mouse_screen, br)) {
                int t = g_buildable_types[i];
                FactionState *fs = &gs->factions[FACTION_GOLD];
                for (int q = fs->queue_count - 1; q >= 0; --q) {
                    if (fs->queue[q].unit_type == t) {
                        int refund = g_unit_production_info[t].credit_cost / 2;
                        fs->credits += refund;
                        for (int k = q; k < fs->queue_count - 1; ++k)
                            fs->queue[k] = fs->queue[k + 1];
                        fs->queue_count--;
                        debug_log("build_cancel type=%d refund=%d remaining=%d",
                                  t, refund, fs->credits);
                        break;
                    }
                }
                return;
            }
        }
    }

    /* Minimap click: pan camera, swallow the click. */
    Rectangle mr = minimap_rect();
    if (point_in_rect(mouse_screen, mr)) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) ||
            IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            float fx = (mouse_screen.x - mr.x) / mr.width;
            float fy = (mouse_screen.y - mr.y) / mr.height;
            gs->camera.target.x = fx * (MAP_WIDTH  * TILE_SIZE);
            gs->camera.target.y = fy * (MAP_HEIGHT * TILE_SIZE);
        }
        /* Don't process selection / order on minimap click. */
        gs->drag_button_down = false;
        gs->drag_active      = false;
        return;
    }

    Vector2 mouse_world  = GetScreenToWorld2D(mouse_screen, gs->camera);
    int mtx = (int)floorf(mouse_world.x / TILE_SIZE);
    int mty = (int)floorf(mouse_world.y / TILE_SIZE);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        gs->drag_button_down  = true;
        gs->drag_active       = false;
        gs->drag_start_screen = mouse_screen;
        gs->drag_start_world  = mouse_world;
    }
    if (gs->drag_button_down && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (!gs->drag_active &&
            vec2_dist(mouse_screen, gs->drag_start_screen) > 4.0f) {
            gs->drag_active = true;
            debug_log("drag_begin screen=(%.0f,%.0f)",
                      gs->drag_start_screen.x, gs->drag_start_screen.y);
        }
    }
    if (gs->drag_button_down && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (gs->drag_active) {
            Rectangle r;
            r.x = fminf(gs->drag_start_world.x, mouse_world.x);
            r.y = fminf(gs->drag_start_world.y, mouse_world.y);
            r.width  = fabsf(mouse_world.x - gs->drag_start_world.x);
            r.height = fabsf(mouse_world.y - gs->drag_start_world.y);
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            int before = units_selected_count(&gs->units);
            units_select_in_world_rect(&gs->units, r, FACTION_GOLD, shift);
            int after = units_selected_count(&gs->units);
            if (gs->audio_loaded && after > before) {
                /* Pitch the selection blip by the type of the first
                 * selected unit so HARV / FTRK / INF feel distinct. */
                static const float type_pitch[UNIT_SHP_COUNT] = {
                    0.70f, 0.90f, 1.20f, 0.55f, 1.00f, 0.65f, 0.45f,
                    1.30f, 0.80f
                };
                int first_t = -1;
                for (int i = 0; i < gs->units.count; ++i) {
                    if (gs->units.units[i].alive && gs->units.units[i].selected) {
                        first_t = gs->units.units[i].unit_type; break;
                    }
                }
                SetSoundPitch(gs->sound_select,
                    (first_t >= 0 && first_t < UNIT_SHP_COUNT)
                        ? type_pitch[first_t] : 1.0f);
                PlaySound(gs->sound_select);
            }
        } else if (mtx >= 0 && mtx < MAP_WIDTH && mty >= 0 && mty < MAP_HEIGHT) {
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            int before = units_selected_count(&gs->units);
            units_select_single_at_tile(&gs->units, mtx, mty, FACTION_GOLD, shift);
            int after = units_selected_count(&gs->units);
            if (gs->audio_loaded && after > before) {
                static const float type_pitch[UNIT_SHP_COUNT] = {
                    0.70f, 0.90f, 1.20f, 0.55f, 1.00f, 0.65f, 0.45f,
                    1.30f, 0.80f
                };
                int first_t = -1;
                for (int i = 0; i < gs->units.count; ++i) {
                    if (gs->units.units[i].alive && gs->units.units[i].selected) {
                        first_t = gs->units.units[i].unit_type; break;
                    }
                }
                SetSoundPitch(gs->sound_select,
                    (first_t >= 0 && first_t < UNIT_SHP_COUNT)
                        ? type_pitch[first_t] : 1.0f);
                PlaySound(gs->sound_select);
            }
        }
        gs->drag_button_down = false;
        gs->drag_active      = false;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        if (mtx >= 0 && mtx < MAP_WIDTH && mty >= 0 && mty < MAP_HEIGHT &&
            units_selected_count(&gs->units) > 0) {
            /* If selection is buildings only, treat the click as a
             * rally-point set for the gold faction (newly produced
             * units will walk to it). Otherwise: attack-on-enemy or
             * move. */
            bool only_buildings = true;
            for (int i = 0; i < gs->units.count; ++i) {
                const Unit *uu = &gs->units.units[i];
                if (!uu->alive || !uu->selected) continue;
                if (!unit_type_is_stationary(uu->unit_type)) {
                    only_buildings = false;
                    break;
                }
            }
            if (only_buildings) {
                FactionState *fs = &gs->factions[FACTION_GOLD];
                fs->rally_x   = mtx;
                fs->rally_y   = mty;
                fs->rally_set = true;
                debug_log("rally_set faction=%d tile=(%d,%d)",
                          FACTION_GOLD, mtx, mty);
                return;
            }
            int enemy_id = units_find_at_tile(&gs->units, mtx, mty, FACTION_RED);
            if (enemy_id >= 0) {
                debug_log("input mouse_right_attack tile=(%d,%d) target=%d",
                          mtx, mty, enemy_id);
                units_order_attack(&gs->units, &gs->map, enemy_id);
            } else {
                debug_log("input mouse_right tile=(%d,%d) world=(%.1f,%.1f)",
                          mtx, mty, mouse_world.x, mouse_world.y);
                units_order_move(&gs->units, &gs->map, mtx, mty);
            }
            /* Spawn order-feedback marker at the click tile. Green pulse
             * for moves, red for attacks. Pure visual cue. */
            for (int e = 0; e < MAX_EFFECTS; ++e) {
                if (gs->effects[e].active) continue;
                gs->effects[e].active   = true;
                gs->effects[e].kind     = (enemy_id >= 0)
                    ? EFFECT_KIND_ATTACK_MARK
                    : EFFECT_KIND_MOVE_MARK;
                gs->effects[e].age      = 0.0f;
                gs->effects[e].duration = 0.5f;
                gs->effects[e].world_x  = (mtx + 0.5f) * (float)TILE_SIZE;
                gs->effects[e].world_y  = (mty + 0.5f) * (float)TILE_SIZE;
                break;
            }
        }
    }
}

void game_update(GameState *gs, float dt) {
    if (debug_log_enabled()) log_key_edges();

    /* F1 toggles sprite browser. ESC exits browser back to game. */
    if (IsKeyPressed(KEY_F1)) {
        gs->sprite_browser = !gs->sprite_browser;
        if (gs->sprite_browser && gs->sprite_browser_type == 0 &&
            gs->sprite_browser_frame == 0) {
            /* Default to refinery (war factory) since that's the
             * one we keep arguing about. */
            gs->sprite_browser_type = UNIT_TYPE_REFINERY;
        }
        debug_log("sprite_browser toggle=%d", gs->sprite_browser);
    }
    if (gs->sprite_browser) {
        if (IsKeyPressed(KEY_RIGHT))
            gs->sprite_browser_frame++;
        if (IsKeyPressed(KEY_LEFT))
            gs->sprite_browser_frame--;
        if (IsKeyPressed(KEY_UP))
            gs->sprite_browser_type =
                (gs->sprite_browser_type + 1) % UNIT_SHP_COUNT;
        if (IsKeyPressed(KEY_DOWN))
            gs->sprite_browser_type =
                (gs->sprite_browser_type + UNIT_SHP_COUNT - 1) % UNIT_SHP_COUNT;
        if (IsKeyPressed(KEY_TAB))
            gs->sprite_browser_faction =
                1 - gs->sprite_browser_faction;
        if (IsKeyPressed(KEY_ESCAPE))
            gs->sprite_browser = false;

        /* Clamp frame to current sprite range. */
        const ShpSprite *sp = &gs->unit_sprites[
            gs->sprite_browser_type * UNIT_FACTION_COUNT +
            gs->sprite_browser_faction];
        if (sp->frame_count > 0) {
            int n = sp->frame_count;
            int f = gs->sprite_browser_frame % n;
            if (f < 0) f += n;
            gs->sprite_browser_frame = f;
        }

        /* Still want screenshot + quit to work in browser. */
        if (IsKeyPressed(KEY_F12)) {
            gs->screenshot_at = GetTime();
            gs->screenshot_exit = false;
        }
        return;
    }

    if (IsKeyPressed(KEY_R)) {
        debug_log("input restart");
        game_init(gs, gs->screen_w, gs->screen_h);
        return;
    }

    if (IsKeyPressed(KEY_P)) {
        gs->paused = !gs->paused;
        debug_log("input pause=%d", gs->paused ? 1 : 0);
    }
    if (IsKeyPressed(KEY_F12)) {
        /* Manual screenshot: capture this frame at the end of draw. */
        gs->screenshot_at = GetTime();
        gs->screenshot_exit = false;
        debug_log("input screenshot");
    }
    if (gs->paused) {
        /* Frozen world. Camera + selection still respond so the player
         * can plan, but no sim, no economy, no AI, no harvesters. */
        update_camera(gs, dt);
        update_mouse_input(gs);
        return;
    }

    /* H = jump camera to gold spawn (home base). */
    if (IsKeyPressed(KEY_H)) {
        gs->camera.target.x =
            ((float)gs->factions[FACTION_GOLD].spawn_x + 0.5f) * (float)TILE_SIZE;
        gs->camera.target.y =
            ((float)gs->factions[FACTION_GOLD].spawn_y + 0.5f) * (float)TILE_SIZE;
        debug_log("input home_camera");
    }
    /* F = jump camera to centroid of current selection. */
    if (IsKeyPressed(KEY_F)) {
        float sx = 0.0f, sy = 0.0f;
        int n = 0;
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *unit = &gs->units.units[i];
            if (!unit->alive || !unit->selected) continue;
            sx += ((float)unit->tile_x + 0.5f) * (float)TILE_SIZE;
            sy += ((float)unit->tile_y + 0.5f) * (float)TILE_SIZE;
            n++;
        }
        if (n > 0) {
            gs->camera.target.x = sx / (float)n;
            gs->camera.target.y = sy / (float)n;
            debug_log("input focus_selection n=%d", n);
        }
    }

    /* S = stop: clear paths, drop combat target, reset harvester state
     * for all currently-selected gold units. */
    if (IsKeyPressed(KEY_S)) {
        int n = 0;
        for (int i = 0; i < gs->units.count; ++i) {
            Unit *unit = &gs->units.units[i];
            if (!unit->alive || !unit->selected) continue;
            unit->path_len = 0;
            unit->path_idx = 0;
            unit->target_id = -1;
            unit->harvester_state = HARV_IDLE;
            n++;
        }
        if (n > 0) debug_log("input stop count=%d", n);
    }

    /* Control groups: Ctrl+1..9 to assign current gold selection to a
     * group, 1..9 to recall it. Standard RTS muscle memory. */
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    for (int g = 0; g < 9; ++g) {
        if (!IsKeyPressed(KEY_ONE + g)) continue;
        if (ctrl) {
            unsigned long long mask = 0;
            int count = 0;
            for (int i = 0; i < gs->units.count && i < 64; ++i) {
                Unit *unit = &gs->units.units[i];
                if (!unit->alive || unit->faction != FACTION_GOLD) continue;
                if (unit->selected) { mask |= (1ull << i); count++; }
            }
            gs->groups[g] = mask;
            debug_log("group_assign idx=%d count=%d", g + 1, count);
        } else {
            int count = 0;
            for (int i = 0; i < gs->units.count && i < 64; ++i) {
                Unit *unit = &gs->units.units[i];
                bool in_group =
                    (gs->groups[g] & (1ull << i)) != 0 &&
                    unit->alive && unit->faction == FACTION_GOLD;
                unit->selected = in_group;
                if (in_group) count++;
            }
            debug_log("group_select idx=%d count=%d", g + 1, count);
        }
    }

    /* Build hotkeys: Q/W/E/T/Y/G/A/Z → buildable slot 0..7. Avoids
     * clashing with the existing R (restart), S (stop), H (home),
     * F (focus selection), P (pause), U (repair), and the
     * WASD/arrow camera. */
    static const int build_hotkey[BUILDABLE_COUNT] = {
        KEY_Q, KEY_W, KEY_E, KEY_T, KEY_Y, KEY_G, KEY_A, KEY_Z
    };
    for (int i = 0; i < BUILDABLE_COUNT; ++i) {
        if (IsKeyPressed(build_hotkey[i])) {
            try_enqueue_build(gs, FACTION_GOLD, g_buildable_types[i]);
        }
    }

    /* Repair: U toggles repair on every selected damaged gold building.
     * Pressing U again on something already repairing cancels it. */
    if (IsKeyPressed(KEY_U)) {
        int started = 0, stopped = 0;
        for (int i = 0; i < gs->units.count; ++i) {
            Unit *uu = &gs->units.units[i];
            if (!uu->alive || !uu->selected) continue;
            if (uu->faction != FACTION_GOLD) continue;
            if (!unit_type_is_stationary(uu->unit_type)) continue;
            if (uu->repairing) {
                uu->repairing = false; uu->repair_acc = 0.0f;
                stopped++;
            } else if (uu->hp < uu->hp_max) {
                uu->repairing = true; uu->repair_acc = 0.0f;
                started++;
            }
        }
        if (started > 0) push_status(gs, "Repairing", 1);
        if (stopped > 0) push_status(gs, "Repair cancelled", 0);
    }

    /* Sell: BACKSPACE marks every selected gold building for sale.
     * The drain itself runs each frame in the sell_tick block below. */
    if (IsKeyPressed(KEY_BACKSPACE)) {
        int n = 0;
        for (int i = 0; i < gs->units.count; ++i) {
            Unit *uu = &gs->units.units[i];
            if (!uu->alive || !uu->selected) continue;
            if (uu->faction != FACTION_GOLD) continue;
            if (!unit_type_is_stationary(uu->unit_type)) continue;
            if (uu->selling) continue;
            uu->selling   = true;
            uu->sell_acc  = 0.0f;
            n++;
        }
        if (n > 0) {
            push_status(gs, "Selling building", 0);
            debug_log("sell_started count=%d", n);
        }
    }

    update_camera(gs, dt);
    update_mouse_input(gs);

    gs->sim_acc += dt;
    int safety = 0;
    while (gs->sim_acc >= SIM_DT && safety < 8) {
        gs->sim_acc -= SIM_DT;
        units_tick(&gs->units, &gs->map,
                   g_unit_combat_info, UNIT_SHP_COUNT);
        gs->sim_tick++;
        safety++;
    }

    update_economy(gs, dt);
    harvester_tick(gs, dt);

    /* Repair drain — heals 12 HP/s and bills $1/HP. If credits run
     * out or HP hits max, repair auto-stops. */
    {
        const float repair_rate = 12.0f;
        for (int i = 0; i < gs->units.count; ++i) {
            Unit *uu = &gs->units.units[i];
            if (!uu->alive || !uu->repairing) continue;
            if (uu->hp >= uu->hp_max) {
                uu->repairing = false;
                if (uu->faction == FACTION_GOLD)
                    push_status(gs, "Repair complete", 1);
                continue;
            }
            FactionState *fs = &gs->factions[uu->faction];
            if (fs->credits <= 0) {
                uu->repairing = false;
                if (uu->faction == FACTION_GOLD)
                    push_status(gs, "Repair halted: no funds", 2);
                continue;
            }
            float heal = repair_rate * dt;
            int   ihp  = (int)heal;
            uu->repair_acc += heal - (float)ihp;
            if (uu->repair_acc >= 1.0f) {
                int extra = (int)uu->repair_acc;
                ihp += extra;
                uu->repair_acc -= (float)extra;
            }
            if (ihp > fs->credits) ihp = fs->credits;
            int new_hp = uu->hp + ihp;
            if (new_hp > uu->hp_max) {
                ihp -= (new_hp - uu->hp_max);
                new_hp = uu->hp_max;
            }
            uu->hp = (short)new_hp;
            fs->credits -= ihp;
        }
    }

    /* Sell drain — for each unit currently `selling`, deplete HP over
     * 1.5 s and trickle 50 % of the original credit cost back to the
     * faction. When sell_acc passes the duration, the unit dies via
     * the same path as combat death (apply_damage with -1 attacker so
     * it isn't credited as a kill). */
    {
        const float sell_dur = 1.5f;
        for (int i = 0; i < gs->units.count; ++i) {
            Unit *uu = &gs->units.units[i];
            if (!uu->alive || !uu->selling) continue;
            float prev = uu->sell_acc;
            uu->sell_acc += dt;
            int cost = g_unit_production_info[uu->unit_type].credit_cost;
            int refund_total = cost / 2;
            int paid_before = (int)((prev / sell_dur) * (float)refund_total);
            float now_t = uu->sell_acc / sell_dur;
            if (now_t > 1.0f) now_t = 1.0f;
            int paid_now = (int)(now_t * (float)refund_total);
            int delta = paid_now - paid_before;
            if (delta > 0)
                gs->factions[uu->faction].credits += delta;
            int hp_target = (int)((1.0f - now_t) * (float)uu->hp_max);
            if (hp_target < 0) hp_target = 0;
            if (uu->hp > hp_target) uu->hp = (short)hp_target;
            if (uu->sell_acc >= sell_dur) {
                /* Final kill — set hp to 0; the death-detection pass
                 * picks it up next iteration and runs the standard
                 * cleanup (footprint unblock, explosion, crater). */
                uu->hp = 0;
                uu->alive = false;
                uu->path_len = 0;
                uu->path_idx = 0;
                uu->killer_id = -1;
                uu->selling = false;
                debug_log("sell_complete id=%d type=%d refund=%d",
                          uu->id, uu->unit_type, refund_total);
            }
        }
    }

    /* Game-over latch: if either side has no live construction yard,
     * the other side has won. Latches on first detection so a
     * subsequent rebuild (none possible today, but defensive) doesn't
     * un-win it. */
    if (gs->game_over == 0) {
        bool factory_alive[UNIT_FACTION_COUNT] = { false, false };
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *uu = &gs->units.units[i];
            if (!uu->alive) continue;
            if (uu->unit_type != UNIT_TYPE_FACTORY) continue;
            if (uu->faction < UNIT_FACTION_COUNT)
                factory_alive[uu->faction] = true;
        }
        if (!factory_alive[FACTION_GOLD]) {
            gs->game_over = 2;
            debug_log("game_over winner=red");
        } else if (!factory_alive[FACTION_RED]) {
            gs->game_over = 1;
            debug_log("game_over winner=gold");
        }
    }

    /* Smoke from damaged buildings — every ~0.4 s, any building below
     * 50 % HP coughs up a smoke puff at its position. Slow rising
     * dark circle that fades out. Pure visual signal, no gameplay
     * effect. */
    gs->smoke_acc += dt;
    if (gs->smoke_acc >= 0.4f) {
        gs->smoke_acc = 0.0f;
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *uu = &gs->units.units[i];
            if (!uu->alive) continue;
            if (!unit_type_is_stationary(uu->unit_type)) continue;
            if (uu->hp_max <= 0) continue;
            if (uu->hp * 2 >= uu->hp_max) continue; /* > 50 % */
            for (int e = 0; e < MAX_EFFECTS; ++e) {
                if (gs->effects[e].active) continue;
                gs->effects[e].active   = true;
                gs->effects[e].kind     = EFFECT_KIND_SMOKE;
                gs->effects[e].age      = 0.0f;
                gs->effects[e].duration = 1.4f;
                /* For 2×2 buildings, plume from the centre of the
                 * footprint rather than the NW anchor tile. */
                bool big = (uu->unit_type == UNIT_TYPE_FACTORY ||
                            uu->unit_type == UNIT_TYPE_REFINERY);
                float cx = (uu->tile_x + (big ? 1.0f : 0.5f)) * (float)TILE_SIZE;
                float cy = (uu->tile_y + (big ? 0.9f : 0.4f)) * (float)TILE_SIZE;
                int jitter = (int)((unsigned)gs->sim_tick * 1664525u + i) & 0x1F;
                gs->effects[e].world_x  = cx + (float)((jitter & 0xF) - 8);
                gs->effects[e].world_y  = cy;
                break;
            }
        }
    }

    /* Building flames at sub-33 % HP — every ~0.18 s, any damaged
     * building emits a small orange flicker around its centre. Sits
     * over the smoke layer for additional combat readability. */
    gs->flame_acc += dt;
    if (gs->flame_acc >= 0.18f) {
        gs->flame_acc = 0.0f;
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *uu = &gs->units.units[i];
            if (!uu->alive) continue;
            if (!unit_type_is_stationary(uu->unit_type)) continue;
            if (uu->hp_max <= 0) continue;
            if (uu->hp * 3 >= uu->hp_max) continue;  /* > 33 % */
            for (int e = 0; e < MAX_EFFECTS; ++e) {
                if (gs->effects[e].active) continue;
                gs->effects[e].active   = true;
                gs->effects[e].kind     = EFFECT_KIND_FLAME;
                gs->effects[e].age      = 0.0f;
                gs->effects[e].duration = 0.55f;
                bool big = (uu->unit_type == UNIT_TYPE_FACTORY ||
                            uu->unit_type == UNIT_TYPE_REFINERY);
                float cx = (uu->tile_x + (big ? 1.0f : 0.5f)) * (float)TILE_SIZE;
                float cy = (uu->tile_y + (big ? 0.85f : 0.45f)) * (float)TILE_SIZE;
                int jitter = (int)((unsigned)gs->sim_tick * 2654435761u + i) & 0x3F;
                gs->effects[e].world_x = cx + (float)((jitter & 0x1F) - 16);
                gs->effects[e].world_y = cy + (float)(((jitter >> 5) & 0x7) - 4);
                break;
            }
        }
    }

    /* Dust trail under moving vehicles. Every ~0.25 s, each moving
     * vehicle (harvester, flame truck) drops a sand-coloured puff at
     * its current screen position. Skips infantry and stationary
     * units; skipped over grass tiles where a dust kick wouldn't make
     * sense. */
    gs->dust_acc += dt;
    if (gs->dust_acc >= 0.25f) {
        gs->dust_acc = 0.0f;
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *uu = &gs->units.units[i];
            if (!uu->alive) continue;
            if (uu->unit_type != 0 && uu->unit_type != 1) continue; /* harv, ftrk */
            bool moving = (uu->next_x != uu->tile_x) ||
                          (uu->next_y != uu->tile_y);
            if (!moving) continue;
            int tx = uu->tile_x, ty = uu->tile_y;
            if (tx < 0 || tx >= MAP_WIDTH || ty < 0 || ty >= MAP_HEIGHT) continue;
            unsigned char tile = gs->map.tiles[ty][tx];
            if (tile != TILE_DIRT && tile != TILE_ORE) continue;
            for (int e = 0; e < MAX_EFFECTS; ++e) {
                if (gs->effects[e].active) continue;
                gs->effects[e].active   = true;
                gs->effects[e].kind     = EFFECT_KIND_DUST;
                gs->effects[e].age      = 0.0f;
                gs->effects[e].duration = 0.6f;
                gs->effects[e].world_x  = (uu->tile_x + 0.5f) * (float)TILE_SIZE;
                gs->effects[e].world_y  = (uu->tile_y + 0.7f) * (float)TILE_SIZE;
                break;
            }
        }
    }

    /* Ore regrowth: every 60 s, one dirt tile adjacent to an existing
     * ore tile becomes ore. Keeps the economy from running dry mid-game.
     * Picks a deterministic candidate via sim_tick. */
    gs->ore_regrow_acc += dt;
    if (gs->ore_regrow_acc >= 60.0f) {
        gs->ore_regrow_acc = 0.0f;
        static const int dx8[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
        static const int dy8[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
        static int cand_x[256];
        static int cand_y[256];
        int n = 0;
        for (int y = 0; y < MAP_HEIGHT && n < 256; ++y) {
            for (int x = 0; x < MAP_WIDTH && n < 256; ++x) {
                if (gs->map.tiles[y][x] != TILE_DIRT) continue;
                if (gs->map.blocked[y][x]) continue;
                for (int d = 0; d < 8; ++d) {
                    int nx = x + dx8[d], ny = y + dy8[d];
                    if (nx < 0 || ny < 0 ||
                        nx >= MAP_WIDTH || ny >= MAP_HEIGHT) continue;
                    if (gs->map.tiles[ny][nx] == TILE_ORE) {
                        cand_x[n] = x; cand_y[n] = y; n++; break;
                    }
                }
            }
        }
        if (n > 0) {
            int pick = (int)((unsigned)gs->sim_tick % (unsigned)n);
            int x = cand_x[pick], y = cand_y[pick];
            gs->map.tiles[y][x] = TILE_ORE;
            tilemap_repaint_tile(&gs->map, x, y);
            debug_log("ore_regrow at=(%d,%d) candidates=%d", x, y, n);
        }
    }

    /* Visibility recomputes ~5 Hz — gold units don't move fast enough to
     * need every-frame fog updates, and the O(N · r²) scan is wasted
     * work otherwise. */
    gs->visibility_acc += dt;
    if (gs->visibility_acc >= 0.2f) {
        gs->visibility_acc = 0.0f;
        recompute_visibility(gs);
    }

    /* AI build: every ~3s, the red faction enqueues whatever it can
     * afford. When the base is in distress (recent damage), bias hard
     * toward turrets to shore up defences. Otherwise round-robin
     * through types so it doesn't end up an infantry-only deathball. */
    gs->ai_build_acc += dt;
    if (gs->ai_build_acc >= 3.0f) {
        gs->ai_build_acc = 0.0f;
        FactionState *fs = &gs->factions[FACTION_RED];
        if (fs->queue_count < MAX_BUILD_QUEUE) {
            bool in_distress = gs->ai_distress_until > gs->sim_tick;
            if (in_distress) {
                /* Try turret first; if can't afford, fall back to cheap
                 * infantry to plug the hole, then truck. */
                static const int defense_priority[] = { 4, 2, 1 };
                bool enqueued = false;
                for (int k = 0; k < 3; ++k) {
                    if (try_enqueue_build(gs, FACTION_RED,
                                           defense_priority[k])) {
                        enqueued = true; break;
                    }
                }
                (void)enqueued;
            } else {
                /* Order: infantry, turret, flame truck, refinery,
                 * harvester. Round-robin start position. */
                static const int ai_priority[BUILDABLE_COUNT] = {
                    7, 2, 4, 1, 8, 5, 6, 0
                };
                int start = (int)((gs->sim_tick / 25) % BUILDABLE_COUNT);
                for (int k = 0; k < BUILDABLE_COUNT; ++k) {
                    int idx = (start + k) % BUILDABLE_COUNT;
                    if (try_enqueue_build(gs, FACTION_RED, ai_priority[idx]))
                        break;
                }
            }
        }
    }

    /* --- spawn death + muzzle effects, then tick existing effects --- */
    for (int i = 0; i < gs->units.count; ++i) {
        Unit *unit = &gs->units.units[i];

        /* Death explosion — once per dead unit. */
        if (!unit->alive && !unit->death_acked) {
            /* Free the (factory or refinery) footprint so units can
             * walk over the ruins. */
            if (unit_type_is_stationary(unit->unit_type) &&
                unit->unit_type != UNIT_TYPE_TURRET) {
                tilemap_set_blocked(&gs->map, unit->tile_x, unit->tile_y,
                                    2, 2, false);
            }
            /* Credit kill / loss. */
            if (unit->faction < UNIT_FACTION_COUNT)
                gs->factions[unit->faction].losses++;
            if (unit->killer_id >= 0 && unit->killer_id < gs->units.count) {
                const Unit *k = &gs->units.units[unit->killer_id];
                if (k->faction < UNIT_FACTION_COUNT)
                    gs->factions[k->faction].kills++;
            }
            unit->death_acked = true;
            if (gs->audio_loaded) PlaySound(gs->sound_explosion);
            bool big = (unit->unit_type == UNIT_TYPE_FACTORY ||
                        unit->unit_type == UNIT_TYPE_REFINERY);
            float cx = (unit->tile_x + (big ? 1.0f : 0.5f)) * (float)TILE_SIZE;
            float cy = (unit->tile_y + (big ? 1.0f : 0.5f)) * (float)TILE_SIZE;
            if (gs->effect_sprite_loaded[EFFECT_KIND_EXPLOSION]) {
                for (int e = 0; e < MAX_EFFECTS; ++e) {
                    if (gs->effects[e].active) continue;
                    gs->effects[e].active   = true;
                    gs->effects[e].kind     = EFFECT_KIND_EXPLOSION;
                    gs->effects[e].age      = 0.0f;
                    gs->effects[e].duration = 0.45f;
                    gs->effects[e].world_x  = cx;
                    gs->effects[e].world_y  = cy;
                    break;
                }
            }
            /* Lingering scorch / crater. value field encodes the
             * radius so big buildings leave bigger craters. */
            for (int e = 0; e < MAX_EFFECTS; ++e) {
                if (gs->effects[e].active) continue;
                gs->effects[e].active   = true;
                gs->effects[e].kind     = EFFECT_KIND_CRATER;
                gs->effects[e].age      = 0.0f;
                gs->effects[e].duration = 30.0f;
                gs->effects[e].world_x  = cx;
                gs->effects[e].world_y  = cy;
                gs->effects[e].value    = big ? 24 : 12;
                break;
            }
            if (unit->faction == FACTION_GOLD)
                push_status(gs, "Unit lost", 2);
        }

        /* Floating damage number — fires when a hit lands, cleared
         * after spawn. */
        if (unit->damage_pending > 0) {
            int amount = unit->damage_pending;
            unit->damage_pending = 0;

            /* If a red unit took the hit and it's near the red base,
             * raise a distress signal so idle reds rally to defend. */
            if (unit->faction == FACTION_RED) {
                int dxh = unit->tile_x - gs->factions[FACTION_RED].spawn_x;
                int dyh = unit->tile_y - gs->factions[FACTION_RED].spawn_y;
                if (dxh * dxh + dyh * dyh < 144) {  /* 12-tile radius */
                    gs->ai_distress_x     = unit->tile_x;
                    gs->ai_distress_y     = unit->tile_y;
                    gs->ai_distress_until = gs->sim_tick + 25 * 8;  /* 8 s */
                }
            }
            /* Mirror for the player: gold unit hit near home plays an
             * alarm sound, throttled to every 5 s. Status banner
             * piggy-backs on the same throttle so the player gets a
             * paired audio + visual cue. */
            if (unit->faction == FACTION_GOLD) {
                int dxh = unit->tile_x - gs->factions[FACTION_GOLD].spawn_x;
                int dyh = unit->tile_y - gs->factions[FACTION_GOLD].spawn_y;
                if (dxh * dxh + dyh * dyh < 144 &&
                    gs->sim_tick - gs->last_alarm_tick > 25 * 5) {
                    gs->last_alarm_tick = gs->sim_tick;
                    if (gs->audio_loaded) PlaySound(gs->sound_alarm);
                    push_status(gs, "Base under attack", 2);
                    debug_log("alarm at=(%d,%d)", unit->tile_x, unit->tile_y);
                }
            }
            for (int e = 0; e < MAX_EFFECTS; ++e) {
                if (gs->effects[e].active) continue;
                gs->effects[e].active   = true;
                gs->effects[e].kind     = EFFECT_KIND_DAMAGE_TEXT;
                gs->effects[e].age      = 0.0f;
                gs->effects[e].duration = 0.7f;
                gs->effects[e].world_x  = (unit->tile_x + 0.5f) * (float)TILE_SIZE;
                gs->effects[e].world_y  = (unit->tile_y + 0.3f) * (float)TILE_SIZE;
                gs->effects[e].value    = amount;
                break;
            }
        }

        /* Promotion popup — fires once per rank-up, cleared after
         * spawn. We piggy-back on the floating-text infrastructure. */
        if (unit->alive && unit->just_promoted) {
            int rank = (int)unit->just_promoted;
            unit->just_promoted = 0;
            for (int e = 0; e < MAX_EFFECTS; ++e) {
                if (gs->effects[e].active) continue;
                gs->effects[e].active   = true;
                gs->effects[e].kind     = EFFECT_KIND_PROMOTION;
                gs->effects[e].age      = 0.0f;
                gs->effects[e].duration = 1.4f;
                gs->effects[e].world_x  = (unit->tile_x + 0.5f) * (float)TILE_SIZE;
                gs->effects[e].world_y  = (unit->tile_y + 0.2f) * (float)TILE_SIZE;
                gs->effects[e].value    = rank;
                break;
            }
            if (unit->faction == FACTION_GOLD) {
                push_status(gs,
                    rank >= 2 ? "Unit promoted: ELITE" : "Unit promoted: VETERAN",
                    1);
            }
        }

        /* Muzzle flash — fires once per shot, cleared after spawn. */
        if (unit->alive && unit->just_fired) {
            unit->just_fired = false;
            float mx = (unit->tile_x + 0.5f) * (float)TILE_SIZE;
            float my = (unit->tile_y + 0.5f) * (float)TILE_SIZE;
            if (gs->effect_sprite_loaded[EFFECT_KIND_MUZZLE]) {
                for (int e = 0; e < MAX_EFFECTS; ++e) {
                    if (gs->effects[e].active) continue;
                    gs->effects[e].active   = true;
                    gs->effects[e].kind     = EFFECT_KIND_MUZZLE;
                    gs->effects[e].age      = 0.0f;
                    gs->effects[e].duration = 0.10f;
                    gs->effects[e].world_x  = mx;
                    gs->effects[e].world_y  = my;
                    break;
                }
            }
            /* Tracer line shooter→target. target_id can have been
             * cleared in the same tick by units_tick if the target
             * died, so guard. */
            if (unit->target_id >= 0 && unit->target_id < gs->units.count) {
                const Unit *t = &gs->units.units[unit->target_id];
                if (t->alive) {
                    for (int e = 0; e < MAX_EFFECTS; ++e) {
                        if (gs->effects[e].active) continue;
                        gs->effects[e].active   = true;
                        gs->effects[e].kind     = EFFECT_KIND_TRACER;
                        gs->effects[e].age      = 0.0f;
                        gs->effects[e].duration = 0.08f;
                        gs->effects[e].world_x  = mx;
                        gs->effects[e].world_y  = my;
                        gs->effects[e].world_x2 =
                            (t->tile_x + 0.5f) * (float)TILE_SIZE;
                        gs->effects[e].world_y2 =
                            (t->tile_y + 0.5f) * (float)TILE_SIZE;
                        break;
                    }
                }
            }
        }
    }

    for (int e = 0; e < MAX_EFFECTS; ++e) {
        if (!gs->effects[e].active) continue;
        gs->effects[e].age += dt;
        if (gs->effects[e].age >= gs->effects[e].duration) {
            gs->effects[e].active = false;
        }
    }
    for (int s = 0; s < MAX_STATUS_MSGS; ++s) {
        if (!gs->status_msgs[s].active) continue;
        gs->status_msgs[s].age += dt;
        if (gs->status_msgs[s].age >= gs->status_msgs[s].duration)
            gs->status_msgs[s].active = false;
    }

    /* AI wave dispatch: every second, count idle red combat units. If
     * the muster reaches the current wave threshold, send them all
     * together at one target (the gold factory if alive, else the
     * nearest non-harvester gold unit). The threshold grows over the
     * match so late-game waves are more dangerous. */
    gs->ai_acc += dt;
    if (gs->ai_acc >= 1.0f) {
        gs->ai_acc = 0.0f;

        int factory_id = -1;
        for (int j = 0; j < gs->units.count; ++j) {
            const Unit *o = &gs->units.units[j];
            if (o->alive && o->faction == FACTION_GOLD &&
                o->unit_type == UNIT_TYPE_FACTORY) {
                factory_id = j; break;
            }
        }

        /* Gather idle red combat units. */
        int idle_ids[MAX_UNITS];
        int idle_n = 0;
        for (int i = 0; i < gs->units.count; ++i) {
            Unit *me = &gs->units.units[i];
            if (!me->alive || me->faction != FACTION_RED) continue;
            if (unit_type_is_stationary(me->unit_type)) continue;
            if (me->unit_type == 0)   continue;  /* harvesters auto-mine */
            if (me->path_len > 0)     continue;
            if (me->target_id >= 0)   continue;
            idle_ids[idle_n++] = i;
        }

        /* Wave threshold: 4 at start, +1 every 60 s, capped at 8. Drops
         * to 2 while a distress alarm is active so reds rally fast. */
        int wave_size = 4 + (int)((unsigned)gs->sim_tick / (25u * 60u));
        if (wave_size > 8) wave_size = 8;
        bool defending = gs->ai_distress_until > gs->sim_tick;
        if (defending) wave_size = 2;

        if (idle_n >= wave_size) {
            /* Pick a single target for the whole wave. */
            int wave_target = -1;
            if (factory_id >= 0) {
                wave_target = factory_id;
            } else {
                int best = -1;
                int best_d2 = 1 << 30;
                /* Use first idle unit's position as the search origin. */
                int from_x = gs->units.units[idle_ids[0]].tile_x;
                int from_y = gs->units.units[idle_ids[0]].tile_y;
                for (int j = 0; j < gs->units.count; ++j) {
                    const Unit *o = &gs->units.units[j];
                    if (!o->alive || o->faction == FACTION_RED) continue;
                    if (o->unit_type == 0) continue;  /* skip harvesters */
                    int dx = o->tile_x - from_x;
                    int dy = o->tile_y - from_y;
                    int d2 = dx * dx + dy * dy;
                    if (d2 < best_d2) { best_d2 = d2; best = j; }
                }
                wave_target = best;
            }
            int wx = -1, wy = -1;
            if (defending) {
                /* Rally to the distress location. */
                wx = gs->ai_distress_x;
                wy = gs->ai_distress_y;
                nearest_passable_tile(&gs->map, wx, wy, &wx, &wy);
                debug_log("ai_defend size=%d at=(%d,%d)", idle_n, wx, wy);
            } else if (wave_target >= 0) {
                const Unit *t = &gs->units.units[wave_target];
                wx = t->tile_x; wy = t->tile_y;
                nearest_passable_tile(&gs->map, wx, wy, &wx, &wy);
                debug_log("ai_wave size=%d target=%d at=(%d,%d) approach=(%d,%d)",
                          idle_n, wave_target, t->tile_x, t->tile_y, wx, wy);
            }
            if (wx >= 0) {
                for (int k = 0; k < idle_n; ++k) {
                    units_order_move_one(&gs->units, &gs->map,
                                         idle_ids[k], wx, wy);
                }
            }
        }
    }

    gs->debug_log_acc += dt;
    if (debug_log_enabled() && gs->debug_log_acc >= 0.5f) {
        gs->debug_log_acc = 0.0f;
        debug_log("state camera=(%.1f,%.1f) zoom=%.2f fps=%d screen=%dx%d "
                  "sim_tick=%lld units=%d selected=%d",
                  gs->camera.target.x, gs->camera.target.y,
                  gs->camera.zoom, GetFPS(),
                  GetScreenWidth(), GetScreenHeight(),
                  gs->sim_tick, gs->units.count,
                  units_selected_count(&gs->units));
    }
}

/* Sprite browser view — black background, alternating-grey checker
 * to make transparent pixels obvious, sprite drawn 4× scale, frame
 * info on screen. Standalone; doesn't render the game world. */
static void draw_sprite_browser(GameState *gs) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    ClearBackground((Color){ 18, 18, 22, 255 });

    /* Checker pattern centred on screen — 32 px squares so any
     * transparent pixel against this background is unmistakable. */
    int chk = 32;
    int center_x = sw / 2;
    int center_y = sh / 2;
    int box_w = sw - 200;
    int box_h = sh - 200;
    int box_x = center_x - box_w / 2;
    int box_y = center_y - box_h / 2;
    DrawRectangleLinesEx(
        (Rectangle){ (float)(box_x - 2), (float)(box_y - 2),
                     (float)(box_w + 4), (float)(box_h + 4) },
        2.0f, (Color){ 80, 80, 90, 255 });
    for (int yy = 0; yy < box_h; yy += chk) {
        for (int xx = 0; xx < box_w; xx += chk) {
            int dark = ((xx / chk) + (yy / chk)) & 1;
            Color c = dark ? (Color){ 50, 50, 55, 255 }
                           : (Color){ 80, 80, 85, 255 };
            int dw = chk; int dh = chk;
            if (xx + dw > box_w) dw = box_w - xx;
            if (yy + dh > box_h) dh = box_h - yy;
            DrawRectangle(box_x + xx, box_y + yy, dw, dh, c);
        }
    }

    /* Look up the sprite. */
    int t = gs->sprite_browser_type;
    int f = gs->sprite_browser_faction;
    if (t < 0 || t >= UNIT_SHP_COUNT) t = 0;
    if (f < 0 || f >= UNIT_FACTION_COUNT) f = 0;
    const ShpSprite *sp =
        &gs->unit_sprites[t * UNIT_FACTION_COUNT + f];

    int frame = gs->sprite_browser_frame;
    if (sp->frame_count <= 0 || !sp->frames) {
        DrawText("(sprite not loaded)", center_x - 80, center_y - 8, 16,
                 (Color){ 240, 80, 80, 255 });
    } else {
        if (frame < 0) frame = 0;
        if (frame >= sp->frame_count) frame = sp->frame_count - 1;
        Texture2D tex = sp->frames[frame];
        if (tex.id != 0) {
            float scale = 4.0f;
            float dw = sp->width  * scale;
            float dh = sp->height * scale;
            Rectangle src = { 0, 0, (float)sp->width, (float)sp->height };
            Rectangle dst = {
                center_x - dw * 0.5f,
                center_y - dh * 0.5f,
                dw, dh,
            };
            /* Use point-filter scaling so pixel art is crisp. raylib
             * defaults to point filter for textures, but be explicit. */
            DrawTexturePro(tex, src, dst, (Vector2){ 0, 0 }, 0, WHITE);
            /* Outline of the actual sprite frame extent. */
            DrawRectangleLinesEx(dst, 1.0f, (Color){ 240, 200, 80, 200 });
        } else {
            DrawText("(frame is blank)", center_x - 70, center_y - 8, 16,
                     (Color){ 240, 200, 80, 255 });
        }
    }

    /* Header info. */
    static const char *type_names[UNIT_SHP_COUNT] = {
        "harv", "ftrk", "e6/inf", "fact", "sam2/tur", "weap3/ref"
    };
    char buf[256];
    snprintf(buf, sizeof(buf),
             "SPRITE BROWSER  type=%d %s  faction=%s  frame=%d/%d  size=%dx%d",
             t, (t < UNIT_SHP_COUNT ? type_names[t] : "?"),
             (f == 0 ? "GOLD" : "RED"),
             frame, sp->frame_count > 0 ? sp->frame_count - 1 : 0,
             sp->width, sp->height);
    DrawText(buf, 10, 10, 16, RAYWHITE);
    DrawText("←/→ frame   ↑/↓ unit type   Tab faction   ESC/F1 exit   F12 screenshot",
             10, 32, 14, (Color){ 200, 200, 220, 220 });

    /* Screenshot trigger (manual F12 or auto). */
    if (gs->screenshot_at > 0.0 && GetTime() >= gs->screenshot_at) {
        TakeScreenshot("screenshot.png");
        debug_log("screenshot_saved at=%.2fs path=screenshot.png exit=%d",
                  GetTime() - gs->start_time, gs->screenshot_exit ? 1 : 0);
        gs->screenshot_at = 0.0;
        if (gs->screenshot_exit) gs->should_quit = true;
    }
}

void game_draw(GameState *gs) {
    if (gs->sprite_browser) {
        draw_sprite_browser(gs);
        return;
    }
    float sim_alpha = gs->sim_acc / SIM_DT;
    if (sim_alpha > 1.0f) sim_alpha = 1.0f;

    BeginMode2D(gs->camera);
        tilemap_draw(&gs->map, gs->camera);
        draw_decorations(gs);
        draw_structures(gs);
        /* Crater decals — drawn AFTER terrain but BEFORE units so a
         * unit walking through a crater renders on top of it. Slow
         * fade over 30 s. */
        for (int e = 0; e < MAX_EFFECTS; ++e) {
            const Effect *ef = &gs->effects[e];
            if (!ef->active) continue;
            if (ef->kind != EFFECT_KIND_CRATER) continue;
            float t = ef->age / ef->duration;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            unsigned char alpha = (unsigned char)(140.0f * (1.0f - t));
            float r = (float)(ef->value > 0 ? ef->value : 12);
            DrawEllipse((int)ef->world_x, (int)ef->world_y, r, r * 0.6f,
                        (Color){ 30, 22, 18, alpha });
            DrawEllipse((int)ef->world_x, (int)ef->world_y,
                        r * 0.6f, r * 0.36f,
                        (Color){ 14, 10, 8, alpha });
        }
        units_draw(&gs->units, sim_alpha, gs->unit_sprites,
                   g_unit_sprite_info, UNIT_SHP_COUNT);

        /* Selling-building overlay — pulsing "$" so it's obvious the
         * countdown is in progress and the player can mash Backspace
         * again on something else. Repair gets a "+" in green. */
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *uu = &gs->units.units[i];
            if (!uu->alive) continue;
            if (!uu->selling && !uu->repairing) continue;
            bool big = (uu->unit_type == UNIT_TYPE_FACTORY ||
                        uu->unit_type == UNIT_TYPE_REFINERY);
            float cx = (uu->tile_x + (big ? 1.0f : 0.5f)) * (float)TILE_SIZE;
            float cy = (uu->tile_y + (big ? 1.0f : 0.5f)) * (float)TILE_SIZE;
            float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 8.0f);
            unsigned char a = (unsigned char)(160.0f + 95.0f * pulse);
            const char *icon = uu->selling ? "$" : "+";
            Color col = uu->selling
                ? (Color){ 240, 220, 130, a }
                : (Color){ 100, 240, 120, a };
            int tw = MeasureText(icon, 36);
            DrawText(icon, (int)(cx - tw * 0.5f), (int)(cy - 22), 36, col);
        }

        /* Amber targeting line from each selected unit to its current
         * target — makes it obvious what your selection is attacking. */
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *uu = &gs->units.units[i];
            if (!uu->alive || !uu->selected) continue;
            if (uu->target_id < 0) continue;
            const Unit *tt = &gs->units.units[uu->target_id];
            if (!tt->alive) continue;
            Vector2 src = {
                (uu->tile_x + 0.5f) * (float)TILE_SIZE,
                (uu->tile_y + 0.5f) * (float)TILE_SIZE,
            };
            Vector2 dst = {
                (tt->tile_x + 0.5f) * (float)TILE_SIZE,
                (tt->tile_y + 0.5f) * (float)TILE_SIZE,
            };
            DrawLineEx(src, dst, 1.0f, (Color){ 240, 200, 80, 180 });
        }

        for (int e = 0; e < MAX_EFFECTS; ++e) {
            const Effect *ef = &gs->effects[e];
            if (!ef->active) continue;
            if (ef->kind == EFFECT_KIND_CRATER) continue; /* drawn earlier */

            if (ef->kind == EFFECT_KIND_SMOKE) {
                float t = ef->age / ef->duration;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                float y_off = t * 36.0f;
                float radius = 4.0f + 4.0f * t;
                unsigned char alpha = (unsigned char)(180.0f * (1.0f - t));
                DrawCircleV(
                    (Vector2){ ef->world_x, ef->world_y - y_off },
                    radius,
                    (Color){ 60, 60, 65, alpha });
                continue;
            }

            if (ef->kind == EFFECT_KIND_FLAME) {
                /* Two-stop colour ramp: bright yellow core, orange
                 * outer, fading to red as it dies. Rises slightly. */
                float t = ef->age / ef->duration;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                float y_off = t * 10.0f;
                float r_outer = 5.0f - 3.0f * t;
                float r_inner = r_outer * 0.55f;
                if (r_outer < 0.5f) r_outer = 0.5f;
                if (r_inner < 0.3f) r_inner = 0.3f;
                unsigned char a = (unsigned char)(220.0f * (1.0f - t));
                Vector2 p = { ef->world_x, ef->world_y - y_off };
                /* Mix orange→red as we age. */
                Color outer = (Color){
                    240,
                    (unsigned char)(140.0f * (1.0f - t * 0.6f)),
                    40, a
                };
                Color inner = (Color){ 255, 230, 120, a };
                DrawCircleV(p, r_outer, outer);
                DrawCircleV(p, r_inner, inner);
                continue;
            }

            if (ef->kind == EFFECT_KIND_DUST) {
                /* Same shape as smoke, smaller and sand-coloured. Lower
                 * peak alpha so it doesn't fight with sprites. */
                float t = ef->age / ef->duration;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                float y_off = t * 8.0f;
                float radius = 3.0f + 3.0f * t;
                unsigned char alpha = (unsigned char)(120.0f * (1.0f - t));
                DrawCircleV(
                    (Vector2){ ef->world_x, ef->world_y - y_off },
                    radius,
                    (Color){ 196, 168, 120, alpha });
                continue;
            }

            if (ef->kind == EFFECT_KIND_MOVE_MARK ||
                ef->kind == EFFECT_KIND_ATTACK_MARK) {
                /* Pulsing X with an expanding ring. The ring grows
                 * from a tile to ~1.5 tiles while fading out. */
                float t = ef->age / ef->duration;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                float r = 6.0f + t * 18.0f;
                unsigned char alpha = (unsigned char)(220.0f * (1.0f - t));
                Color col = (ef->kind == EFFECT_KIND_ATTACK_MARK)
                    ? (Color){ 240,  60,  60, alpha }
                    : (Color){  90, 240,  90, alpha };
                Vector2 c = { ef->world_x, ef->world_y };
                DrawCircleLines((int)c.x, (int)c.y, r, col);
                /* X arms: 8 px each arm, drawn full-strength early then
                 * fading with the ring. */
                float arm = 7.0f;
                DrawLineEx((Vector2){ c.x - arm, c.y - arm },
                           (Vector2){ c.x + arm, c.y + arm },
                           2.0f, col);
                DrawLineEx((Vector2){ c.x - arm, c.y + arm },
                           (Vector2){ c.x + arm, c.y - arm },
                           2.0f, col);
                continue;
            }

            if (ef->kind == EFFECT_KIND_TRACER) {
                /* Bright tracer line that fades quickly. We pull both
                 * endpoints inward by ~10 px so the line starts at the
                 * shooter's muzzle position and ends just shy of the
                 * target sprite, not at their tile centres. */
                float t = ef->age / ef->duration;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                unsigned char alpha = (unsigned char)(220.0f * (1.0f - t));
                Vector2 a = { ef->world_x,  ef->world_y };
                Vector2 b = { ef->world_x2, ef->world_y2 };
                float dx = b.x - a.x, dy = b.y - a.y;
                float len = sqrtf(dx * dx + dy * dy);
                if (len > 24.0f) {
                    float ux = dx / len, uy = dy / len;
                    a.x += ux * 10.0f; a.y += uy * 10.0f;
                    b.x -= ux * 10.0f; b.y -= uy * 10.0f;
                }
                DrawLineEx(a, b, 2.0f,
                           (Color){ 255, 230, 140, alpha });
                continue;
            }

            if (ef->kind == EFFECT_KIND_PROMOTION) {
                float t = ef->age / ef->duration;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                float y_off = t * 28.0f;
                /* Hold full alpha for the first 60% of life, then fade. */
                float fade = (t < 0.6f) ? 1.0f : (1.0f - t) / 0.4f;
                if (fade < 0.0f) fade = 0.0f;
                unsigned char alpha = (unsigned char)(255.0f * fade);
                const char *txt = (ef->value >= 2) ? "ELITE!" : "VETERAN!";
                Color col = (ef->value >= 2)
                    ? (Color){ 240,  90,  90, alpha }
                    : (Color){ 240, 220, 100, alpha };
                int tw = MeasureText(txt, 14);
                DrawText(txt,
                         (int)(ef->world_x - tw * 0.5f),
                         (int)(ef->world_y - y_off),
                         14, col);
                continue;
            }

            if (ef->kind == EFFECT_KIND_DAMAGE_TEXT) {
                /* Drawn in world space so it pans with the camera, but
                 * floats up over its lifetime and fades out. */
                float t = ef->age / ef->duration;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                float y_off = t * 24.0f;
                unsigned char alpha = (unsigned char)(255.0f * (1.0f - t));
                char buf[16];
                snprintf(buf, sizeof(buf), "-%d", ef->value);
                int tw = MeasureText(buf, 14);
                DrawText(buf,
                         (int)(ef->world_x - tw * 0.5f),
                         (int)(ef->world_y - y_off),
                         14,
                         (Color){ 250, 80, 80, alpha });
                continue;
            }

            if (ef->kind >= EFFECT_KIND_COUNT) continue;
            if (!gs->effect_sprite_loaded[ef->kind]) continue;
            const ShpSprite *spr = &gs->effect_sprites[ef->kind];
            int frame = (int)((ef->age / ef->duration) * (float)spr->frame_count);
            if (frame < 0) frame = 0;
            if (frame >= spr->frame_count) frame = spr->frame_count - 1;
            Texture2D tex = spr->frames[frame];
            if (tex.id == 0) continue;
            Vector2 origin = {
                ef->world_x - spr->width  * 0.5f,
                ef->world_y - spr->height * 0.5f,
            };
            DrawTextureV(tex, origin, WHITE);
        }

        /* Rally-point flag for the gold faction, only when a building
         * is selected. Drawn as a small green flag plus a dotted line
         * back to the spawn — it's visually distinct from selection
         * brackets and won't clutter when buildings aren't selected. */
        {
            const FactionState *fs = &gs->factions[FACTION_GOLD];
            if (fs->rally_set) {
                bool show = false;
                for (int i = 0; i < gs->units.count; ++i) {
                    const Unit *uu = &gs->units.units[i];
                    if (uu->alive && uu->selected &&
                        uu->faction == FACTION_GOLD &&
                        unit_type_is_stationary(uu->unit_type)) {
                        show = true; break;
                    }
                }
                if (show) {
                    float rx = (fs->rally_x + 0.5f) * (float)TILE_SIZE;
                    float ry = (fs->rally_y + 0.5f) * (float)TILE_SIZE;
                    float sx = (fs->spawn_x + 1.0f) * (float)TILE_SIZE;
                    float sy = (fs->spawn_y + 1.0f) * (float)TILE_SIZE;
                    /* Dashed line from spawn to flag. */
                    float dx = rx - sx, dy = ry - sy;
                    float len = sqrtf(dx * dx + dy * dy);
                    if (len > 1.0f) {
                        float ux = dx / len, uy = dy / len;
                        float seg = 8.0f, gap = 6.0f;
                        for (float t = 0.0f; t < len; t += seg + gap) {
                            float t2 = t + seg;
                            if (t2 > len) t2 = len;
                            DrawLineEx((Vector2){ sx + ux * t,  sy + uy * t },
                                       (Vector2){ sx + ux * t2, sy + uy * t2 },
                                       1.5f, (Color){ 90, 240, 90, 180 });
                        }
                    }
                    /* Pole + triangular flag at the rally point. */
                    DrawLineEx((Vector2){ rx, ry - 18 },
                               (Vector2){ rx, ry + 2 },
                               2.0f, (Color){ 30, 30, 30, 230 });
                    Vector2 a = { rx,        ry - 18 };
                    Vector2 b = { rx + 10,   ry - 14 };
                    Vector2 c2= { rx,        ry - 10 };
                    DrawTriangle(a, c2, b, (Color){ 90, 240, 90, 230 });
                    DrawCircleV((Vector2){ rx, ry }, 2.0f,
                                (Color){ 90, 240, 90, 230 });
                }
            }
        }

        /* Fog of war: tile-resolution overlay over everything in the
         * world view. Hidden = opaque black, Discovered = semi-dark.
         * Skipped for tiles in current sight. Quads are aligned to
         * TILE_SIZE so they line up with everything else. */
        Vector2 tl = GetScreenToWorld2D((Vector2){ 0, 0 }, gs->camera);
        Vector2 br = GetScreenToWorld2D(
            (Vector2){ (float)GetScreenWidth(), (float)GetScreenHeight() },
            gs->camera);
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
                unsigned char v = gs->visibility[y][x];
                if (v == VIS_VISIBLE) continue;
                Color tint = (v == VIS_HIDDEN)
                    ? (Color){   0,   0,   0, 230 }
                    : (Color){   0,   0,   0, 110 };
                DrawRectangle(x * TILE_SIZE, y * TILE_SIZE,
                              TILE_SIZE, TILE_SIZE, tint);
            }
        }
    EndMode2D();

    if (gs->drag_active) {
        Vector2 cur = GetMousePosition();
        Rectangle r;
        r.x = fminf(gs->drag_start_screen.x, cur.x);
        r.y = fminf(gs->drag_start_screen.y, cur.y);
        r.width  = fabsf(cur.x - gs->drag_start_screen.x);
        r.height = fabsf(cur.y - gs->drag_start_screen.y);
        DrawRectangleRec(r, (Color){ 80, 220, 80, 35 });
        DrawRectangleLinesEx(r, 1.5f, (Color){ 80, 220, 80, 200 });
    }

    DrawText("arrows/edge pan  wheel zoom  L select / shift+L add  R move/attack  S stop  P pause  R restart",
             10, 10, 14, RAYWHITE);

    /* Credits + build buttons (top-left below the help line + FPS) */
    char cred_buf[40];
    snprintf(cred_buf, sizeof(cred_buf), "$ %d",
             gs->factions[FACTION_GOLD].credits);
    DrawText(cred_buf, 10, 54, 22, (Color){ 240, 220, 130, 255 });

    Vector2 mp = GetMousePosition();
    for (int slot = 0; slot < BUILDABLE_COUNT; ++slot) {
        int t = g_buildable_types[slot];
        Rectangle br = build_button_rect(slot);
        bool hover = point_in_rect(mp, br);
        bool affordable =
            gs->factions[FACTION_GOLD].credits >=
            g_unit_production_info[t].credit_cost;

        Color bg   = hover ? (Color){ 60, 60, 70, 230 } : (Color){ 35, 35, 45, 220 };
        Color edge = affordable ? (Color){ 200, 200, 220, 220 }
                                : (Color){ 110, 110, 120, 180 };
        DrawRectangleRec(br, bg);
        DrawRectangleLinesEx(br, 1.5f, edge);

        /* Sprite icon: render the unit's gold-faction frame 0 scaled
         * to fit the button. For a vehicle that's facing East; for
         * infantry it's the East-stand frame. Good enough as an
         * identifier without dedicated icon art. */
        const ShpSprite *sprite =
            &gs->unit_sprites[t * UNIT_FACTION_COUNT + FACTION_GOLD];
        if (sprite->frame_count > 0 && sprite->frames &&
            sprite->frames[0].id != 0) {
            Texture2D tex = sprite->frames[0];
            float pad = 6.0f;
            float text_band = 18.0f;
            float dst_w = br.width  - 2.0f * pad;
            float dst_h = br.height - text_band - pad;
            float scale = dst_w / (float)sprite->width;
            float h_at_scale = (float)sprite->height * scale;
            if (h_at_scale > dst_h) scale = dst_h / (float)sprite->height;
            Rectangle src = { 0, 0, (float)sprite->width, (float)sprite->height };
            float draw_w = (float)sprite->width  * scale;
            float draw_h = (float)sprite->height * scale;
            Rectangle dst = {
                br.x + (br.width - draw_w) * 0.5f,
                br.y + pad,
                draw_w, draw_h,
            };
            Color tint = affordable ? WHITE : (Color){ 160, 160, 160, 200 };
            DrawTexturePro(tex, src, dst, (Vector2){ 0, 0 }, 0, tint);
        }

        /* Cost text along the bottom of the button. */
        char label[32];
        snprintf(label, sizeof(label), "$%d",
                 g_unit_production_info[t].credit_cost);
        int tw = MeasureText(label, 14);
        DrawText(label,
                 (int)(br.x + (br.width - tw) * 0.5f),
                 (int)(br.y + br.height - 17), 14,
                 affordable ? RAYWHITE : (Color){ 160, 160, 160, 255 });

        int queued = 0;
        for (int q = 0; q < gs->factions[FACTION_GOLD].queue_count; ++q)
            if (gs->factions[FACTION_GOLD].queue[q].unit_type == t) queued++;
        if (queued > 0) {
            char qbuf[16];
            snprintf(qbuf, sizeof(qbuf), "x%d", queued);
            DrawText(qbuf, (int)(br.x + br.width - 22),
                     (int)br.y + 4, 14,
                     (Color){ 240, 220, 130, 255 });
        }
        /* Hotkey letter in the top-left of the button. */
        static const char *hotkey_label[BUILDABLE_COUNT] = {
            "Q", "W", "E", "T", "Y", "G", "A", "Z"
        };
        DrawText(hotkey_label[slot], (int)br.x + 4, (int)br.y + 2, 12,
                 (Color){ 200, 200, 200, 200 });
        const FactionState *fs = &gs->factions[FACTION_GOLD];
        if (fs->queue_count > 0 && fs->queue[0].unit_type == t) {
            float bt = g_unit_production_info[t].build_time_s;
            float p  = fs->queue[0].elapsed_s / bt;
            if (p > 1.0f) p = 1.0f;
            int pw = (int)(br.width * p);
            DrawRectangle((int)br.x, (int)(br.y + br.height - 3),
                          pw, 3, (Color){ 90, 220, 110, 255 });
        }
    }

    int gold = 0, red = 0;
    for (int i = 0; i < gs->units.count; ++i) {
        if (!gs->units.units[i].alive) continue;
        if (gs->units.units[i].faction == FACTION_GOLD) gold++;
        else if (gs->units.units[i].faction == FACTION_RED) red++;
    }

    char buf[160];
    snprintf(buf, sizeof(buf),
             "%d FPS  zoom %.2fx  selected %d  tick %lld%s",
             GetFPS(), gs->camera.zoom,
             units_selected_count(&gs->units),
             gs->sim_tick,
             debug_log_enabled() ? "  [debug log on]" : "");
    DrawText(buf, 10, 32, 16, (Color){ 200, 200, 200, 255 });

    char score[96];
    snprintf(score, sizeof(score),
             "GOLD %d  vs  RED %d", gold, red);
    int sw = MeasureText(score, 22);
    DrawText(score, GetScreenWidth() - sw - 14, 12, 22,
             (Color){ 240, 220, 130, 255 });

    char kdr[96];
    snprintf(kdr, sizeof(kdr),
             "K %d / L %d   |   K %d / L %d",
             gs->factions[FACTION_GOLD].kills,
             gs->factions[FACTION_GOLD].losses,
             gs->factions[FACTION_RED].kills,
             gs->factions[FACTION_RED].losses);
    int kw = MeasureText(kdr, 14);
    DrawText(kdr, GetScreenWidth() - kw - 14, 38, 14,
             (Color){ 200, 200, 220, 220 });

    /* Elapsed clock — wall-clock since game_init, paused while paused. */
    int elapsed_s = (int)(GetTime() - gs->start_time);
    char clock_buf[16];
    snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d",
             elapsed_s / 60, elapsed_s % 60);
    int cw = MeasureText(clock_buf, 16);
    DrawText(clock_buf, GetScreenWidth() - cw - 14, 56, 16,
             (Color){ 220, 220, 240, 220 });

    if (gs->paused) {
        const char *msg = "PAUSED";
        int mw = MeasureText(msg, 64);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                      (Color){ 0, 0, 0, 110 });
        DrawText(msg, (GetScreenWidth() - mw) / 2,
                 GetScreenHeight() / 2 - 32, 64, RAYWHITE);
        DrawText("press P to resume",
                 (GetScreenWidth() - MeasureText("press P to resume", 20)) / 2,
                 GetScreenHeight() / 2 + 40, 20,
                 (Color){ 200, 200, 200, 255 });
    }

    if (gold == 0 && red > 0) {
        const char *msg = "DEFEAT";
        int mw = MeasureText(msg, 64);
        DrawText(msg, (GetScreenWidth() - mw) / 2,
                 GetScreenHeight() / 2 - 32, 64,
                 (Color){ 230, 80, 80, 255 });
        DrawText("press R to restart",
                 (GetScreenWidth() - MeasureText("press R to restart", 20)) / 2,
                 GetScreenHeight() / 2 + 40, 20, RAYWHITE);
    } else if (red == 0 && gold > 0) {
        const char *msg = "VICTORY";
        int mw = MeasureText(msg, 64);
        DrawText(msg, (GetScreenWidth() - mw) / 2,
                 GetScreenHeight() / 2 - 32, 64,
                 (Color){ 90, 230, 110, 255 });
        DrawText("press R to restart",
                 (GetScreenWidth() - MeasureText("press R to restart", 20)) / 2,
                 GetScreenHeight() / 2 + 40, 20, RAYWHITE);
    }

    /* --- minimap --- */
    {
        Rectangle mr = minimap_rect();
        DrawRectangleRec((Rectangle){ mr.x - 2, mr.y - 2, mr.width + 4, mr.height + 4 },
                         (Color){ 30, 30, 35, 230 });
        if (gs->map.rendered_built) {
            Rectangle src = { 0, 0,
                              (float)(MAP_WIDTH  * TILE_SIZE),
                              (float)(MAP_HEIGHT * TILE_SIZE) };
            DrawTexturePro(gs->map.rendered, src, mr, (Vector2){ 0, 0 }, 0, WHITE);
        }

        float sx = mr.width  / (float)(MAP_WIDTH  * TILE_SIZE);
        float sy = mr.height / (float)(MAP_HEIGHT * TILE_SIZE);

        /* Minimap fog overlay — must come BEFORE dots so player dots
         * stay visible. Per-tile darkening matches the world view. */
        float tile_mw = mr.width  / (float)MAP_WIDTH;
        float tile_mh = mr.height / (float)MAP_HEIGHT;
        for (int y = 0; y < MAP_HEIGHT; ++y) {
            for (int x = 0; x < MAP_WIDTH; ++x) {
                unsigned char v = gs->visibility[y][x];
                if (v == VIS_VISIBLE) continue;
                Color tint = (v == VIS_HIDDEN)
                    ? (Color){ 0, 0, 0, 220 }
                    : (Color){ 0, 0, 0, 110 };
                DrawRectangle(
                    (int)(mr.x + (float)x * tile_mw),
                    (int)(mr.y + (float)y * tile_mh),
                    (int)tile_mw + 1, (int)tile_mh + 1, tint);
            }
        }

        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *unit = &gs->units.units[i];
            if (!unit->alive) continue;
            /* Hide enemy dots that are in fog — same scouting model as
             * the world view. Friendly dots are always shown. */
            if (unit->faction == FACTION_RED &&
                gs->visibility[unit->tile_y][unit->tile_x] != VIS_VISIBLE)
                continue;
            float ux = (unit->tile_x + 0.5f) * (float)TILE_SIZE;
            float uy = (unit->tile_y + 0.5f) * (float)TILE_SIZE;
            Color dot = (unit->faction == FACTION_RED)
                        ? (Color){ 240, 80, 80, 255 }
                        : (Color){ 240, 220, 130, 255 };
            DrawCircle((int)(mr.x + ux * sx), (int)(mr.y + uy * sy), 2.0f, dot);
        }

        /* Camera viewport rectangle. */
        Vector2 tl = GetScreenToWorld2D((Vector2){ 0, 0 }, gs->camera);
        Vector2 br = GetScreenToWorld2D(
            (Vector2){ (float)GetScreenWidth(), (float)GetScreenHeight() },
            gs->camera);
        Rectangle vr = {
            mr.x + tl.x * sx,
            mr.y + tl.y * sy,
            (br.x - tl.x) * sx,
            (br.y - tl.y) * sy,
        };
        DrawRectangleLinesEx(vr, 1.5f, (Color){ 240, 240, 240, 220 });
        DrawRectangleLinesEx(mr, 1.5f, (Color){ 0, 0, 0, 255 });
    }

    /* Selection info bar — bottom-center, shows type breakdown and
     * total HP for the current selection. Hidden when nothing's
     * selected so it doesn't clutter the empty state. */
    {
        int counts[UNIT_SHP_COUNT] = { 0 };
        int total_hp = 0, total_max = 0, sel_n = 0;
        for (int i = 0; i < gs->units.count; ++i) {
            const Unit *u = &gs->units.units[i];
            if (!u->alive || !u->selected) continue;
            if (u->unit_type < UNIT_SHP_COUNT) counts[u->unit_type]++;
            total_hp  += u->hp;
            total_max += u->hp_max;
            sel_n++;
        }
        if (sel_n > 0) {
            char buf[256] = "";
            for (int t = 0; t < UNIT_SHP_COUNT; ++t) {
                if (counts[t] <= 0) continue;
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "  %s x%d",
                         g_unit_short_names[t], counts[t]);
                if (strlen(buf) + strlen(tmp) < sizeof(buf) - 1)
                    strcat(buf, tmp);
            }
            char hp_buf[48];
            snprintf(hp_buf, sizeof(hp_buf), "   HP %d/%d", total_hp, total_max);
            if (strlen(buf) + strlen(hp_buf) < sizeof(buf) - 1)
                strcat(buf, hp_buf);

            int tw = MeasureText(buf, 18);
            int x = (GetScreenWidth() - tw) / 2;
            int y = GetScreenHeight() - 36;
            DrawRectangle(x - 12, y - 6, tw + 24, 30, (Color){ 0, 0, 0, 200 });
            DrawRectangleLinesEx(
                (Rectangle){ (float)(x - 12), (float)(y - 6),
                             (float)(tw + 24), 30.0f },
                1.0f, (Color){ 200, 200, 220, 200 });
            DrawText(buf, x, y, 18, RAYWHITE);
        }
    }

    /* Screenshot trigger moved to game_post_draw() — must run after
     * EndDrawing so all queued draws are flushed to the framebuffer. */

    /* Status messages — top-centre stack, newest on top. Fades out
     * over the last second of life. */
    {
        int y = 56;
        for (int s = 0; s < MAX_STATUS_MSGS; ++s) {
            const StatusMsg *m = &gs->status_msgs[s];
            if (!m->active) continue;
            float t = m->age / m->duration;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            float fade = (t > 0.7f) ? (1.0f - t) / 0.3f : 1.0f;
            if (fade < 0.0f) fade = 0.0f;
            unsigned char alpha = (unsigned char)(230.0f * fade);
            Color text_col = (m->tone == 1)
                ? (Color){ 140, 240, 140, alpha }
                : (m->tone == 2)
                    ? (Color){ 240, 100, 100, alpha }
                    : (Color){ 230, 230, 230, alpha };
            int tw = MeasureText(m->text, 18);
            int sw = GetScreenWidth();
            int x = sw / 2 - tw / 2;
            DrawRectangle(x - 10, y - 4, tw + 20, 26,
                          (Color){ 0, 0, 0, (unsigned char)(160.0f * fade) });
            DrawText(m->text, x, y, 18, text_col);
            y += 30;
        }
    }

    /* Game-over banner — drawn over everything except the cursor. */
    if (gs->game_over != 0) {
        const char *line1 = (gs->game_over == 1) ? "VICTORY" : "DEFEAT";
        const char *line2 = "press R to restart";
        Color line1_col   = (gs->game_over == 1)
            ? (Color){ 240, 220, 100, 255 }
            : (Color){ 240,  80,  80, 255 };
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        DrawRectangle(0, 0, sw, sh, (Color){ 0, 0, 0, 110 });
        int w1 = MeasureText(line1, 64);
        int w2 = MeasureText(line2, 20);
        int cx = sw / 2;
        int cy = sh / 2;
        DrawText(line1, cx - w1 / 2, cy - 56, 64, line1_col);
        DrawText(line2, cx - w2 / 2, cy + 16, 20, RAYWHITE);
    }

    /* Custom mouse cursor — `mods/common-content/cursor.png` is a 10×16
     * arrow. We draw it at 2× scale so it's visible at modern DPIs.
     * Hot-spot is the top-left corner.
     *
     * Context overlay: with units selected, mouse over an enemy →
     * red attack reticle; mouse over passable empty terrain → green
     * move reticle. Drawn UNDER the arrow so the arrow stays the
     * authoritative pointer. */
    if (gs->cursor_loaded) {
        Vector2 m = GetMousePosition();

        int cursor_kind = 0; /* 0 default, 1 move, 2 attack */
        if (units_selected_count(&gs->units) > 0 && !gs->sprite_browser) {
            /* Skip if mouse is over a UI element. */
            bool over_ui = false;
            for (int i = 0; i < BUILDABLE_COUNT && !over_ui; ++i) {
                if (point_in_rect(m, build_button_rect(i))) over_ui = true;
            }
            if (point_in_rect(m, minimap_rect())) over_ui = true;

            if (!over_ui) {
                Vector2 mw = GetScreenToWorld2D(m, gs->camera);
                int mtx = (int)floorf(mw.x / TILE_SIZE);
                int mty = (int)floorf(mw.y / TILE_SIZE);
                if (mtx >= 0 && mtx < MAP_WIDTH &&
                    mty >= 0 && mty < MAP_HEIGHT) {
                    int enemy = units_find_at_tile(&gs->units, mtx, mty,
                                                   FACTION_RED);
                    if (enemy >= 0) {
                        /* Only consider visible enemies. */
                        if (gs->visibility[mty][mtx] == VIS_VISIBLE)
                            cursor_kind = 2;
                    } else if (tile_passable(&gs->map, mtx, mty)) {
                        cursor_kind = 1;
                    }
                }
            }
        }
        if (cursor_kind != 0) {
            Color col = (cursor_kind == 2)
                ? (Color){ 240,  60,  60, 230 }
                : (Color){  90, 240,  90, 230 };
            float r = 12.0f;
            DrawCircleLines((int)m.x, (int)m.y, r, col);
            DrawCircleLines((int)m.x, (int)m.y, r - 1, col);
            if (cursor_kind == 2) {
                /* Crosshair lines through reticle for attack. */
                DrawLineEx((Vector2){ m.x - r - 4, m.y },
                           (Vector2){ m.x - r + 2, m.y }, 2.0f, col);
                DrawLineEx((Vector2){ m.x + r - 2, m.y },
                           (Vector2){ m.x + r + 4, m.y }, 2.0f, col);
                DrawLineEx((Vector2){ m.x, m.y - r - 4 },
                           (Vector2){ m.x, m.y - r + 2 }, 2.0f, col);
                DrawLineEx((Vector2){ m.x, m.y + r - 2 },
                           (Vector2){ m.x, m.y + r + 4 }, 2.0f, col);
            }
        }

        float scale = 2.0f;
        Rectangle src = { 0, 0,
                          (float)gs->cursor_tex.width,
                          (float)gs->cursor_tex.height };
        Rectangle dst = { m.x, m.y,
                          gs->cursor_tex.width  * scale,
                          gs->cursor_tex.height * scale };
        DrawTexturePro(gs->cursor_tex, src, dst, (Vector2){ 0, 0 }, 0, WHITE);
    }
}
