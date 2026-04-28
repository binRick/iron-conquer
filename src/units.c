#include "units.h"
#include "debug_log.h"

#include <math.h>
#include <string.h>

#define MOVE_PROGRESS_PER_TICK 0.2f
#define WALK_FRAME_DURATION    0.10f  /* seconds per walk-cycle frame */

static const float facing_angle_rad[8] = {
    -1.5707963f,   /* 0 N  */
    -0.7853981f,   /* 1 NE */
     0.0f,         /* 2 E  */
     0.7853981f,   /* 3 SE */
     1.5707963f,   /* 4 S  */
     2.3561944f,   /* 5 SW */
     3.1415926f,   /* 6 W  */
    -2.3561944f,   /* 7 NW */
};

void units_init(UnitArray *u) {
    memset(u, 0, sizeof(*u));
}

int units_spawn(UnitArray *u, int tx, int ty, unsigned char unit_type,
                unsigned char faction) {
    if (u->count >= MAX_UNITS) return -1;
    int id = u->count++;
    Unit *unit = &u->units[id];
    memset(unit, 0, sizeof(*unit));
    unit->id        = id;
    unit->tile_x    = tx; unit->tile_y = ty;
    unit->next_x    = tx; unit->next_y = ty;
    unit->facing    = 4; /* face south by default */
    unit->alive     = true;
    unit->unit_type = unit_type;
    unit->faction   = faction;
    unit->target_id = -1;
    unit->killer_id = -1;
    unit->harvester_home_x = (short)tx;
    unit->harvester_home_y = (short)ty;
    /* Default HP — overridden by units_tick from UnitCombatInfo. We set
     * something nonzero now so a unit isn't instantly dead before its
     * first tick. */
    unit->hp        = 100;
    unit->hp_max    = 100;
    if (faction == FACTION_RED)
        unit->color = (Color){ 220, 60, 60, 255 };
    else
        unit->color = (Color){ 220, 200, 120, 255 };
    debug_log("unit_spawn id=%d tile=(%d,%d) type=%d faction=%d",
              id, tx, ty, unit_type, faction);
    return id;
}

static int dir_to_facing(int dx, int dy) {
    if (dx == 0 && dy == 0) return -1;
    static const int facings[3][3] = {
        /* dx -1, 0, 1 ; outer index = dy+1 */
        { 7, 0, 1 },  /* dy = -1 */
        { 6, -1, 2 }, /* dy =  0 */
        { 5, 4, 3 },  /* dy = +1 */
    };
    int ix = dx + 1, iy = dy + 1;
    if (ix < 0 || ix > 2 || iy < 0 || iy > 2) return -1;
    return facings[iy][ix];
}

/* True if any other live unit currently claims tile (tx,ty). A unit's
 * claim is its current tile, plus its in-flight destination if it's
 * mid-move. Stationary units claim only their tile. */
static bool tile_claimed_by_other(const UnitArray *u, int self_id,
                                  int tx, int ty) {
    for (int i = 0; i < u->count; ++i) {
        if (i == self_id) continue;
        const Unit *o = &u->units[i];
        if (!o->alive) continue;
        if (o->tile_x == tx && o->tile_y == ty) return true;
        bool o_moving = (o->next_x != o->tile_x) || (o->next_y != o->tile_y);
        if (o_moving && o->next_x == tx && o->next_y == ty) return true;
    }
    return false;
}

/* Try to advance `unit` to the next tile in its path. Returns true if
 * the step was taken; false if the unit has no path or the next tile
 * is currently claimed by another unit (in which case the unit waits
 * one tick and tries again). */
static bool unit_try_advance_path_step(Unit *unit, const UnitArray *u, int self_id) {
    if (unit->path_idx >= unit->path_len) {
        unit->path_len = 0;
        unit->path_idx = 0;
        return false;
    }
    int packed = unit->path[unit->path_idx];
    int nx = packed % MAP_WIDTH;
    int ny = packed / MAP_WIDTH;
    if (tile_claimed_by_other(u, self_id, nx, ny)) {
        return false;
    }
    unit->path_idx++;
    unit->next_x = nx;
    unit->next_y = ny;
    int f = dir_to_facing(nx - unit->tile_x, ny - unit->tile_y);
    if (f >= 0) unit->facing = f;
    return true;
}

static int find_nearest_enemy_in_range(const UnitArray *u, int self_id,
                                       float range_tiles) {
    const Unit *me = &u->units[self_id];
    int best = -1;
    int best_d2 = (int)(range_tiles * range_tiles) + 1;
    for (int j = 0; j < u->count; ++j) {
        if (j == self_id) continue;
        const Unit *o = &u->units[j];
        if (!o->alive || o->faction == me->faction) continue;
        int dx = o->tile_x - me->tile_x;
        int dy = o->tile_y - me->tile_y;
        int d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = j;
        }
    }
    return best;
}

/* Veterancy thresholds and damage multipliers. Rookie 0 kills, 1.0×;
 * veteran 2 kills, 1.2×; elite 5 kills, 1.4×. Promotion happens
 * inside apply_damage at the moment the attacker lands a killing
 * blow. */
static int rank_for_kills(unsigned char kills) {
    if (kills >= 5) return 2;
    if (kills >= 2) return 1;
    return 0;
}
static float rank_damage_mult(int rank) {
    if (rank >= 2) return 1.4f;
    if (rank >= 1) return 1.2f;
    return 1.0f;
}

static void apply_damage(Unit *target, int amount, int attacker_id) {
    if (!target->alive) return;
    /* Attackers may have ranked up; scale outgoing damage. attacker_id
     * here is the unit array index; the caller passes -1 for ambient
     * damage (none today, but defensive). UnitArray pointer is not in
     * scope here so we use a global hook — the only callsite knows the
     * attacker, so it scales before calling instead. We just receive
     * the final amount. */
    target->hp -= (short)amount;
    target->damage_flash   = 0.12f;
    target->damage_pending = (short)amount;
    if (target->hp <= 0) {
        target->alive = false;
        target->path_len  = 0;
        target->path_idx  = 0;
        target->killer_id = (short)attacker_id;
        debug_log("unit_killed id=%d by=%d", target->id, attacker_id);
    }
}

void units_tick(UnitArray *u, const TileMap *map,
                const UnitCombatInfo *combat, int type_count) {
    (void)map;
    const float TICK_DT = 1.0f / 25.0f;

    for (int i = 0; i < u->count; ++i) {
        Unit *unit = &u->units[i];
        if (!unit->alive) continue;

        if (unit->damage_flash > 0.0f) {
            unit->damage_flash -= TICK_DT;
            if (unit->damage_flash < 0.0f) unit->damage_flash = 0.0f;
        }

        bool stationary = unit_type_is_stationary(unit->unit_type);

        /* --- movement (path stepping + walk anim) — skipped for
         * stationary types so factories and turrets don't drift. */
        bool moving = !stationary &&
                      ((unit->next_x != unit->tile_x) ||
                       (unit->next_y != unit->tile_y));
        if (stationary) goto combat_block;
        if (moving) {
            unit->move_progress += MOVE_PROGRESS_PER_TICK;
            unit->walk_acc += TICK_DT;
            if (unit->walk_acc >= WALK_FRAME_DURATION) {
                unit->walk_acc -= WALK_FRAME_DURATION;
                unit->walk_frame++;
            }
            if (unit->move_progress >= 1.0f) {
                unit->tile_x = unit->next_x;
                unit->tile_y = unit->next_y;
                unit->move_progress = 0.0f;
                if (unit->path_idx < unit->path_len) {
                    unit_try_advance_path_step(unit, u, i);
                } else {
                    unit->path_len = 0;
                    unit->path_idx = 0;
                    unit->walk_frame = 0;
                    unit->walk_acc   = 0.0f;
                    debug_log("unit_arrived id=%d tile=(%d,%d)",
                              unit->id, unit->tile_x, unit->tile_y);
                }
            }
        } else if (unit->path_idx < unit->path_len) {
            unit_try_advance_path_step(unit, u, i);
        }

    combat_block:
        /* --- combat --- */
        if (!combat || unit->unit_type >= type_count) continue;
        const UnitCombatInfo *ci = &combat[unit->unit_type];
        if (ci->attack_damage <= 0) continue;

        if (unit->attack_cd > 0.0f) unit->attack_cd -= TICK_DT;

        /* Re-acquire / validate target. */
        if (unit->target_id >= 0) {
            const Unit *t = &u->units[unit->target_id];
            int dx = t->tile_x - unit->tile_x;
            int dy = t->tile_y - unit->tile_y;
            int d2 = dx * dx + dy * dy;
            float r = ci->attack_range_tiles;
            if (!t->alive || t->faction == unit->faction ||
                (float)d2 > r * r + 0.5f) {
                unit->target_id = -1;
            }
        }
        if (unit->target_id < 0) {
            unit->target_id = find_nearest_enemy_in_range(u, i,
                                                          ci->attack_range_tiles);
            if (unit->target_id >= 0) {
                debug_log("unit_engage id=%d target=%d", unit->id, unit->target_id);
                /* Halt any pending path so the unit stops to attack
                 * rather than walking past its target. */
                unit->path_len = 0;
                unit->path_idx = 0;
            }
        }

        /* Fire if cooled down, target is valid, and we're stationary
         * enough to draw a bead. (Vehicles in RA fire on the move; for
         * simplicity we require the unit not be mid-tile-transition.) */
        if (unit->target_id >= 0 && unit->attack_cd <= 0.0f && !moving) {
            Unit *tgt = &u->units[unit->target_id];
            int dx = tgt->tile_x - unit->tile_x;
            int dy = tgt->tile_y - unit->tile_y;
            int f = dir_to_facing((dx > 0) - (dx < 0), (dy > 0) - (dy < 0));
            if (f >= 0) unit->facing = f;
            float mult = rank_damage_mult(unit->rank);
            int   amt  = (int)((float)ci->attack_damage * mult + 0.5f);
            bool  was_alive = tgt->alive;
            apply_damage(tgt, amt, unit->id);
            if (was_alive && !tgt->alive) {
                if (unit->kills < 255) unit->kills++;
                int new_rank = rank_for_kills(unit->kills);
                if (new_rank > unit->rank) {
                    unit->rank = (unsigned char)new_rank;
                    unit->just_promoted = (unsigned char)new_rank;
                    debug_log("unit_promoted id=%d kills=%d rank=%d",
                              unit->id, unit->kills, unit->rank);
                }
            }
            unit->attack_cd  = ci->attack_cooldown;
            unit->just_fired = true;
        }
    }
}

static Vector2 unit_render_pos(const Unit *u, float sim_alpha) {
    float prog = u->move_progress;
    bool moving = (u->next_x != u->tile_x) || (u->next_y != u->tile_y);
    if (moving) {
        prog += sim_alpha * MOVE_PROGRESS_PER_TICK;
        if (prog > 1.0f) prog = 1.0f;
    }
    float fx = (float)u->tile_x + ((float)u->next_x - (float)u->tile_x) * prog;
    float fy = (float)u->tile_y + ((float)u->next_y - (float)u->tile_y) * prog;
    return (Vector2){ (fx + 0.5f) * (float)TILE_SIZE,
                      (fy + 0.5f) * (float)TILE_SIZE };
}

/* Corner brackets sized to the unit. Thick (2 px) lines and a 1-px
 * dark inner stroke so the brackets read clearly on both light and
 * dark backgrounds — a single-pixel green line gets lost on the gold
 * faction colour or sandy terrain. */
static void draw_selection_brackets(Vector2 c, int half, int arm) {
    float cx = c.x, cy = c.y;
    float s = (float)half, len = (float)arm;
    Color g = (Color){  90, 240,  90, 255 };
    Color k = (Color){   0,   0,   0, 180 };
    /* Corners: TL, TR, BL, BR. Two segments per corner. */
    Vector2 corners[4][2] = {
        { (Vector2){ cx - s, cy - s }, (Vector2){ cx - s, cy - s } },  /* TL */
        { (Vector2){ cx + s, cy - s }, (Vector2){ cx + s, cy - s } },  /* TR */
        { (Vector2){ cx - s, cy + s }, (Vector2){ cx - s, cy + s } },  /* BL */
        { (Vector2){ cx + s, cy + s }, (Vector2){ cx + s, cy + s } },  /* BR */
    };
    Vector2 horiz_end[4] = {
        (Vector2){ cx - s + len, cy - s },
        (Vector2){ cx + s - len, cy - s },
        (Vector2){ cx - s + len, cy + s },
        (Vector2){ cx + s - len, cy + s },
    };
    Vector2 vert_end[4] = {
        (Vector2){ cx - s, cy - s + len },
        (Vector2){ cx + s, cy - s + len },
        (Vector2){ cx - s, cy + s - len },
        (Vector2){ cx + s, cy + s - len },
    };
    for (int i = 0; i < 4; ++i) {
        DrawLineEx(corners[i][0], horiz_end[i], 3.0f, k);
        DrawLineEx(corners[i][0], vert_end[i],  3.0f, k);
        DrawLineEx(corners[i][0], horiz_end[i], 1.5f, g);
        DrawLineEx(corners[i][0], vert_end[i],  1.5f, g);
    }
}

/* Draw a stack of small "^" chevrons just above the HP bar. Yellow for
 * veteran (1), saturated red for elite (2). Skipped for rookies. */
static void draw_rank_chevrons(Vector2 c, int rank) {
    if (rank <= 0) return;
    Color col = (rank >= 2)
        ? (Color){ 240,  60,  60, 255 }
        : (Color){ 240, 220, 100, 255 };
    int n = (rank >= 2) ? 2 : 1;
    int base_y = (int)c.y - 22;
    for (int i = 0; i < n; ++i) {
        int cx = (int)c.x;
        int cy = base_y - i * 4;
        DrawLineEx((Vector2){ (float)(cx - 4), (float)(cy + 2) },
                   (Vector2){ (float)(cx),     (float)(cy - 2) },
                   1.5f, col);
        DrawLineEx((Vector2){ (float)(cx),     (float)(cy - 2) },
                   (Vector2){ (float)(cx + 4), (float)(cy + 2) },
                   1.5f, col);
    }
}

static void draw_hp_bar(Vector2 c, int hp, int hp_max) {
    if (hp_max <= 0) return;
    int bar_w = 20;
    int bar_h = 3;
    int x = (int)c.x - bar_w / 2;
    int y = (int)c.y - 18;
    int filled = (hp * bar_w) / hp_max;
    if (filled < 0) filled = 0;
    if (filled > bar_w) filled = bar_w;
    DrawRectangle(x, y, bar_w, bar_h, (Color){ 0, 0, 0, 180 });
    Color fill;
    if (hp * 3 < hp_max)      fill = (Color){ 220,  60,  60, 255 };
    else if (hp * 3 < hp_max * 2) fill = (Color){ 230, 200,  40, 255 };
    else                       fill = (Color){  80, 220,  80, 255 };
    DrawRectangle(x, y, filled, bar_h, fill);
}

/* Map our 8-direction facing index to the per-state offset in a
 * Westwood SHP, given how many directional frames the SHP uses per
 * animation state.
 *
 * Convention: frame 0 = facing East, frames advance counter-clockwise.
 * Our facing 0 = N, advancing clockwise.
 *   32 facings: facing 0 → 8, step = 4 → (8 - 4*f + 32) % 32
 *    8 facings: facing 0 → 2, step = 1 → (2 - f + 8) % 8 */
static int facing_offset_in_state(int facing, int facings_count) {
    if (facings_count == 32) return (8 - 4 * facing + 32) % 32;
    if (facings_count == 8)  return (2 - facing + 8) % 8;
    return 0;
}

/* If a same-faction harvester is currently unloading at this refinery,
 * return its progress (0..1). Otherwise return -1. weap3.shp frames
 * 0..9 form a progress-bar animation we co-opt as a deposit "filling
 * up" cue; the default frame is 9 (full) so we land back there as soon
 * as the harvester transitions out of HARV_UNLOADING. */
static float refinery_deposit_progress(const UnitArray *u, const Unit *bld) {
    for (int i = 0; i < u->count; ++i) {
        const Unit *h = &u->units[i];
        if (!h->alive) continue;
        if (h->faction != bld->faction) continue;
        if (h->harvester_state != HARV_UNLOADING) continue;
        if (h->harvester_home_x != bld->tile_x ||
            h->harvester_home_y != bld->tile_y) continue;
        float p = h->harvester_acc;
        if (p < 0.0f) p = 0.0f;
        if (p > 1.0f) p = 1.0f;
        return p;
    }
    return -1.0f;
}

void units_draw(const UnitArray *u, float sim_alpha,
                const ShpSprite *sprites,
                const UnitSpriteInfo *info,
                int type_count) {
    for (int i = 0; i < u->count; ++i) {
        const Unit *unit = &u->units[i];
        if (!unit->alive) continue;
        Vector2 c = unit_render_pos(unit, sim_alpha);

        /* 2×2 buildings (factory, refinery) have their anchor at the
         * NW corner of the footprint. Shift the visual centre to the
         * middle of the 2×2 area so the sprite covers the whole
         * footprint instead of leaving bare ground in the SE strip. */
        if (unit->unit_type == UNIT_TYPE_FACTORY ||
            unit->unit_type == UNIT_TYPE_REFINERY) {
            c.x += (float)TILE_SIZE * 0.5f;
            c.y += (float)TILE_SIZE * 0.5f;
        }

        const ShpSprite *sprite = NULL;
        UnitSpriteInfo si = { 32, 0, 0, 0 };
        if (sprites && unit->unit_type < type_count) {
            int slot = unit->unit_type * UNIT_FACTION_COUNT + unit->faction;
            const ShpSprite *cand = &sprites[slot];
            if (cand->frame_count > 0 && cand->frames) {
                sprite = cand;
                if (info) si = info[unit->unit_type];
            }
        }

        if (sprite) {
            /* Drop shadow under non-stationary units. */
            if (!unit_type_is_stationary(unit->unit_type)) {
                DrawEllipse((int)c.x, (int)c.y + 7,
                            12.0f, 4.0f, (Color){ 0, 0, 0, 90 });
            }
            bool moving = (unit->next_x != unit->tile_x) ||
                          (unit->next_y != unit->tile_y);
            int state_base = 0;
            if (moving && si.walk_frame_count > 0) {
                int wf = unit->walk_frame % si.walk_frame_count;
                state_base = (int)si.walk_anim_offset + wf * si.facings;
            }
            int idx;
            if (si.facings == 1) {
                /* 1-facing buildings: use the per-type default frame so
                 * each building can pick the visually-best static frame
                 * (refinery uses frame 9 of weap3.shp where the
                 * production-bar area is filled, factory uses frame 0). */
                idx = (int)si.default_frame;
                if (unit->unit_type == UNIT_TYPE_REFINERY) {
                    float p = refinery_deposit_progress(u, unit);
                    if (p >= 0.0f) {
                        int n = sprite->frame_count < 10 ?
                                sprite->frame_count : 10;
                        int f = (int)(p * (float)(n - 1) + 0.5f);
                        if (f < 0) f = 0;
                        if (f >= n) f = n - 1;
                        idx = f;
                    }
                }
            } else {
                idx = state_base + facing_offset_in_state(unit->facing, si.facings);
            }
            /* Building damage frame swap: under 33 % HP, switch to last
             * frame (typically the "burning ruin" frame in Westwood
             * building SHPs). */
            if (si.facings == 1 && sprite->frame_count > 1 &&
                unit->hp_max > 0 && unit->hp * 3 < unit->hp_max) {
                idx = sprite->frame_count - 1;
            }
            if (idx < 0 || idx >= sprite->frame_count) idx = 0;
            Texture2D tex = sprite->frames[idx];
            if (tex.id == 0) {
                idx = facing_offset_in_state(unit->facing, si.facings);
                tex = sprite->frames[idx];
            }
            /* For 2×2 buildings, scale the sprite up so it covers the
             * 64×64 footprint. weap3.shp is 72×48 — only 48 px tall,
             * which leaves bare ground showing inside the south + north
             * strips of the footprint. The minimum scale needed is the
             * larger of (64/width) and (64/height); never shrink. */
            float scale = 1.0f;
            if (unit->unit_type == UNIT_TYPE_FACTORY ||
                unit->unit_type == UNIT_TYPE_REFINERY) {
                float target = (float)(2 * TILE_SIZE);
                float sw = target / (float)sprite->width;
                float sh = target / (float)sprite->height;
                scale = sw > sh ? sw : sh;
                if (scale < 1.0f) scale = 1.0f;
            }
            float draw_w = (float)sprite->width  * scale;
            float draw_h = (float)sprite->height * scale;
            Vector2 origin = {
                c.x - draw_w * 0.5f,
                c.y - draw_h * 0.5f,
            };
            Color tint = WHITE;
            /* Sustained damage tint — sprite darkens as HP drops below
             * 60 %. Compounds with the transient red flash on a fresh
             * hit (applied second, so the flash overrides for ~120 ms). */
            if (unit->hp_max > 0) {
                float hp_frac = (float)unit->hp / (float)unit->hp_max;
                if (hp_frac < 0.6f) {
                    float factor = 0.55f + 0.75f * (hp_frac / 0.6f);
                    if (factor > 1.0f) factor = 1.0f;
                    tint.r = (unsigned char)((float)tint.r * factor);
                    tint.g = (unsigned char)((float)tint.g * factor);
                    tint.b = (unsigned char)((float)tint.b * factor);
                }
            }
            if (unit->damage_flash > 0.0f) {
                float t = unit->damage_flash / 0.12f;
                if (t > 1.0f) t = 1.0f;
                tint.r = 255;
                tint.g = (unsigned char)(255 - 200 * t);
                tint.b = (unsigned char)(255 - 200 * t);
            }
            if (scale != 1.0f) {
                Rectangle src = { 0, 0, (float)sprite->width, (float)sprite->height };
                Rectangle dst = { origin.x, origin.y, draw_w, draw_h };
                DrawTexturePro(tex, src, dst, (Vector2){ 0, 0 }, 0.0f, tint);
            } else {
                DrawTextureV(tex, origin, tint);
            }
        } else {
            float r = 10.0f;
            DrawCircleV(c, r, unit->color);
            DrawCircleLines((int)c.x, (int)c.y, r, BLACK);
            float a = facing_angle_rad[unit->facing];
            Vector2 nose = { c.x + cosf(a) * (r + 4.0f),
                             c.y + sinf(a) * (r + 4.0f) };
            DrawLineEx(c, nose, 2.0f, BLACK);
        }

        draw_hp_bar(c, unit->hp, unit->hp_max);
        draw_rank_chevrons(c, unit->rank);
        if (unit->selected) {
            /* Buildings are 2×2 tiles centred on c after the
             * NW-anchor shift above; size the brackets to the
             * footprint so they hug the actual sprite bounds. */
            int half, arm;
            if (unit->unit_type == UNIT_TYPE_FACTORY ||
                unit->unit_type == UNIT_TYPE_REFINERY) {
                half = TILE_SIZE;          /* 32: half of a 2×2 footprint */
                arm  = 12;
            } else {
                half = 14;
                arm  = 6;
            }
            draw_selection_brackets(c, half, arm);
        }
    }
}

bool units_select_single_at_tile(UnitArray *u, int tx, int ty,
                                 int player_faction, bool additive) {
    int hit_id = -1;
    /* First, find the unit that was clicked (if any). */
    for (int i = 0; i < u->count; ++i) {
        Unit *unit = &u->units[i];
        if (!unit->alive) continue;
        bool faction_ok = (player_faction < 0 ||
                           (int)unit->faction == player_faction);
        if (!faction_ok) continue;
        if ((unit->tile_x == tx && unit->tile_y == ty) ||
            (unit->next_x == tx && unit->next_y == ty)) {
            hit_id = i;
            break;
        }
    }

    if (additive) {
        /* shift-click: toggle the clicked unit; leave others unchanged. */
        if (hit_id >= 0) {
            u->units[hit_id].selected = !u->units[hit_id].selected;
            debug_log("select_toggle unit=%d sel=%d",
                      u->units[hit_id].id, u->units[hit_id].selected);
        }
        return hit_id >= 0;
    }

    /* Replace selection: deselect all, then select the hit one. */
    for (int i = 0; i < u->count; ++i) u->units[i].selected = false;
    if (hit_id >= 0) {
        u->units[hit_id].selected = true;
        debug_log("select unit=%d tile=(%d,%d)", u->units[hit_id].id, tx, ty);
    } else {
        debug_log("deselect_all click_tile=(%d,%d)", tx, ty);
    }
    return hit_id >= 0;
}

void units_select_in_world_rect(UnitArray *u, Rectangle r,
                                int player_faction, bool additive) {
    int count = 0;
    for (int i = 0; i < u->count; ++i) {
        Unit *unit = &u->units[i];
        if (!unit->alive) {
            if (!additive) unit->selected = false;
            continue;
        }
        bool faction_ok = (player_faction < 0 ||
                           (int)unit->faction == player_faction);
        Vector2 p = unit_render_pos(unit, 0.0f);
        bool in = faction_ok &&
                  (p.x >= r.x && p.x <= r.x + r.width &&
                   p.y >= r.y && p.y <= r.y + r.height);
        if (additive) {
            if (in) unit->selected = true;
        } else {
            unit->selected = in;
        }
        if (in) count++;
    }
    debug_log("select_box rect=(%.0f,%.0f,%.0f,%.0f) count=%d add=%d",
              r.x, r.y, r.width, r.height, count, additive ? 1 : 0);
}

void units_clear_selection(UnitArray *u) {
    for (int i = 0; i < u->count; ++i) u->units[i].selected = false;
}

int units_selected_count(const UnitArray *u) {
    int n = 0;
    for (int i = 0; i < u->count; ++i)
        if (u->units[i].alive && u->units[i].selected) n++;
    return n;
}

/* BFS from (gx,gy) through passable tiles, collecting up to `count`
 * tiles into out_xs/out_ys. Used to spread N units' goal tiles around
 * a single clicked target so they don't all aim for the same square. */
static int bfs_collect_passable(const TileMap *map, int gx, int gy,
                                int count, int *out_xs, int *out_ys) {
    static bool visited[MAP_HEIGHT][MAP_WIDTH];
    static int  qx[MAP_WIDTH * MAP_HEIGHT];
    static int  qy[MAP_WIDTH * MAP_HEIGHT];

    for (int y = 0; y < MAP_HEIGHT; ++y)
        for (int x = 0; x < MAP_WIDTH; ++x)
            visited[y][x] = false;

    int qh = 0, qt = 0;
    if (gx < 0 || gy < 0 || gx >= MAP_WIDTH || gy >= MAP_HEIGHT) return 0;
    qx[qt] = gx; qy[qt] = gy; qt++;
    visited[gy][gx] = true;

    int placed = 0;
    static const int dx4[] = { 1, -1, 0, 0 };
    static const int dy4[] = { 0, 0, 1, -1 };

    while (qh < qt && placed < count) {
        int cx = qx[qh], cy = qy[qh]; qh++;
        if (tile_passable(map, cx, cy)) {
            out_xs[placed] = cx;
            out_ys[placed] = cy;
            placed++;
        }
        for (int d = 0; d < 4; ++d) {
            int nx = cx + dx4[d], ny = cy + dy4[d];
            if (nx < 0 || ny < 0 || nx >= MAP_WIDTH || ny >= MAP_HEIGHT) continue;
            if (visited[ny][nx]) continue;
            visited[ny][nx] = true;
            qx[qt] = nx; qy[qt] = ny; qt++;
        }
    }
    return placed;
}

int units_find_at_tile(const UnitArray *u, int tx, int ty, int faction) {
    for (int i = 0; i < u->count; ++i) {
        const Unit *o = &u->units[i];
        if (!o->alive) continue;
        if (faction >= 0 && (int)o->faction != faction) continue;
        bool match = (o->tile_x == tx && o->tile_y == ty) ||
                     (o->next_x == tx && o->next_y == ty);
        if (match) return i;
    }
    return -1;
}

void units_order_attack(UnitArray *u, const TileMap *map, int target_id) {
    if (target_id < 0 || target_id >= u->count) return;
    const Unit *target = &u->units[target_id];
    if (!target->alive) return;

    int issued = 0;
    for (int i = 0; i < u->count; ++i) {
        Unit *unit = &u->units[i];
        if (!unit->alive || !unit->selected) continue;
        if ((int)unit->faction == (int)target->faction) continue;
        unit->target_id = target_id;
        /* Path toward the target's current tile; auto-engage halts the
         * path once we're in range. */
        units_order_move_one(u, map, i, target->tile_x, target->tile_y);
        issued++;
    }
    debug_log("order_attack target=%d issued=%d", target_id, issued);
}

bool units_order_move_one(UnitArray *u, const TileMap *map, int unit_id,
                          int gx, int gy) {
    if (unit_id < 0 || unit_id >= u->count) return false;
    Unit *unit = &u->units[unit_id];
    if (!unit->alive) return false;

    int sx = unit->next_x;
    int sy = unit->next_y;

    static Path tmp;
    if (!pathfind(map, sx, sy, gx, gy, &tmp)) return false;

    int n = tmp.len;
    if (n > MAX_PATH) n = MAX_PATH;
    for (int j = 0; j < n; ++j)
        unit->path[j] = tmp.ys[j] * MAP_WIDTH + tmp.xs[j];
    unit->path_len = n;
    unit->path_idx = 0;

    bool idle = (unit->next_x == unit->tile_x && unit->next_y == unit->tile_y);
    if (idle && n > 0) unit_try_advance_path_step(unit, u, unit_id);
    return true;
}

void units_order_move(UnitArray *u, const TileMap *map, int gx, int gy) {
    /* Collect selected units in id order. */
    int sel_ids[MAX_UNITS];
    int sel_n = 0;
    for (int i = 0; i < u->count; ++i)
        if (u->units[i].alive && u->units[i].selected)
            sel_ids[sel_n++] = i;
    if (sel_n == 0) return;

    /* Spread goals so units don't fight for the same square. */
    int goal_xs[MAX_UNITS], goal_ys[MAX_UNITS];
    int goals_found = bfs_collect_passable(map, gx, gy, sel_n, goal_xs, goal_ys);
    if (goals_found == 0) {
        debug_log("order_move_no_goals click=(%d,%d) selected=%d", gx, gy, sel_n);
        return;
    }

    int issued = 0, failed = 0;
    static Path tmp;
    for (int s = 0; s < sel_n; ++s) {
        Unit *unit = &u->units[sel_ids[s]];

        int g_idx = s < goals_found ? s : (goals_found - 1);
        int dst_x = goal_xs[g_idx];
        int dst_y = goal_ys[g_idx];

        /* If mid-move, plan from the tile we're heading into so the new
         * path stitches cleanly onto our current step. */
        int sx = unit->next_x;
        int sy = unit->next_y;

        if (!pathfind(map, sx, sy, dst_x, dst_y, &tmp)) {
            debug_log("path_fail unit=%d from=(%d,%d) to=(%d,%d)",
                      unit->id, sx, sy, dst_x, dst_y);
            failed++;
            continue;
        }

        int n = tmp.len;
        if (n > MAX_PATH) n = MAX_PATH;
        for (int j = 0; j < n; ++j)
            unit->path[j] = tmp.ys[j] * MAP_WIDTH + tmp.xs[j];
        unit->path_len = n;
        unit->path_idx = 0;

        /* For idle units, attempt the first step now (will block if the
         * tile is occupied — they'll retry on the next sim tick). */
        bool idle = (unit->next_x == unit->tile_x && unit->next_y == unit->tile_y);
        if (idle && n > 0) unit_try_advance_path_step(unit, u, sel_ids[s]);

        issued++;
        debug_log("order_move unit=%d to=(%d,%d) path_len=%d",
                  unit->id, dst_x, dst_y, n);
    }
    debug_log("order_move_summary click=(%d,%d) issued=%d failed=%d goals=%d",
              gx, gy, issued, failed, goals_found);
}
