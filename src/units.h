#ifndef IRON_UNITS_H
#define IRON_UNITS_H

#include "raylib.h"
#include "tilemap.h"
#include "pathfind.h"
#include "shp.h"

#include <stdbool.h>

#define MAX_UNITS          64
#define UNIT_FACTION_COUNT 2

/* Special unit types referenced by both units.c and game.c logic.
 * Stationary types skip the movement block in units_tick but still
 * participate in combat (turrets) or take damage (factories,
 * refineries). */
#define UNIT_TYPE_FACTORY  3
#define UNIT_TYPE_TURRET   4
#define UNIT_TYPE_REFINERY 5

static inline bool unit_type_is_stationary(unsigned char ut) {
    return ut == UNIT_TYPE_FACTORY ||
           ut == UNIT_TYPE_TURRET  ||
           ut == UNIT_TYPE_REFINERY;
}

/* True for buildings where harvesters can deposit ore. C&C/RA rule:
 * only the refinery accepts deposits — the construction yard is for
 * building, not for ore. If a faction loses every refinery its
 * harvesters go idle until one is rebuilt. */
static inline bool unit_type_is_deposit(unsigned char ut) {
    return ut == UNIT_TYPE_REFINERY;
}

typedef enum {
    FACTION_GOLD = 0,   /* allies / player */
    FACTION_RED  = 1,   /* soviet / enemy */
} UnitFaction;

typedef struct {
    int           id;
    int           tile_x, tile_y;
    int           next_x, next_y;
    float         move_progress;
    int           path[MAX_PATH];
    int           path_len;
    int           path_idx;
    int           facing;
    bool          selected;
    bool          alive;
    Color         color;
    unsigned char unit_type;   /* index into the sprite array passed to units_draw */
    unsigned char faction;     /* UnitFaction: 0 = gold, 1 = red */
    unsigned char walk_frame;  /* current walk-cycle frame (0..walk_frame_count-1) */
    float         walk_acc;    /* accumulator toward next walk frame */

    /* Combat state. */
    short         hp;            /* current HP */
    short         hp_max;
    int           target_id;     /* -1 if none, else index of target unit */
    float         attack_cd;     /* seconds until next attack permitted */
    bool          death_acked;   /* outer code has spawned a death effect */
    bool          just_fired;    /* outer code spawns a muzzle-flash and clears */
    float         damage_flash;  /* seconds remaining of red hit-flash */
    short         damage_pending;/* set by apply_damage; outer code reads + clears to spawn a floating "-N" effect */
    short         killer_id;     /* unit id that landed the killing blow, -1 if none */
    unsigned char kills;         /* enemies this unit has personally killed */
    unsigned char rank;          /* 0=rookie, 1=veteran, 2=elite — derived from kills */
    unsigned char just_promoted; /* outer code reads + clears to spawn a celebration */
    bool          selling;       /* building is being sold; HP drains, credits trickle */
    float         sell_acc;      /* time accumulator for sell drain */
    bool          repairing;     /* building is being repaired; credits drain, HP rises */
    float         repair_acc;    /* fractional credits owed since last whole tick */

    /* Harvester economy state (only used when unit_type == 0). */
    unsigned char harvester_state;     /* HarvesterState */
    short         harvester_home_x;
    short         harvester_home_y;
    short         harvester_ore_x;
    short         harvester_ore_y;
    float         harvester_acc;       /* timer for gather/unload */
} Unit;

typedef enum {
    HARV_IDLE          = 0,
    HARV_GOING_TO_ORE  = 1,
    HARV_GATHERING     = 2,
    HARV_RETURNING     = 3,
    HARV_UNLOADING     = 4,
} HarvesterState;

/* Per-unit-type rendering metadata. */
typedef struct {
    unsigned char  facings;            /* 8 or 32 */
    unsigned char  walk_frame_count;   /* 0 = no walk animation */
    unsigned short walk_anim_offset;
    unsigned short default_frame;      /* 1-facing buildings: which frame to render */
} UnitSpriteInfo;

/* Per-unit-type combat stats. */
typedef struct {
    short hp_max;
    short attack_damage;
    float attack_range_tiles;   /* attacks anything within this many tiles */
    float attack_cooldown;      /* seconds between attacks */
} UnitCombatInfo;

/* Per-unit-type production stats: cost in credits and build time. */
typedef struct {
    int   credit_cost;
    float build_time_s;
} UnitProductionInfo;

typedef struct {
    Unit units[MAX_UNITS];
    int  count;
} UnitArray;

void units_init(UnitArray *u);
int  units_spawn(UnitArray *u, int tx, int ty, unsigned char unit_type,
                 unsigned char faction);

/* Drives both movement and combat. `combat[type_count]` is per-unit-type
 * stats; pass NULL to disable combat (for tests). */
void units_tick(UnitArray *u, const TileMap *map,
                const UnitCombatInfo *combat, int type_count);

/* `sprites` is laid out as [type_count][UNIT_FACTION_COUNT] flattened —
 * the renderer reads `sprites[unit->unit_type * UNIT_FACTION_COUNT +
 * unit->faction]`. `info[type_count]` describes facings and walk
 * animation per unit type. Slots whose `frame_count == 0` fall back to
 * circle rendering. */
void units_draw(const UnitArray *u, float sim_alpha,
                const ShpSprite *sprites,
                const UnitSpriteInfo *info,
                int type_count);

/* `player_faction` restricts what's selectable (pass -1 for no filter,
 * or a faction id to only allow selecting that faction's units).
 *
 * `additive`: false (default) replaces the existing selection; true
 * adds to / toggles the selection so the player can shift-click to
 * grow a group across multiple actions. For drag-rect selection,
 * additive=true never deselects already-selected units, only ORs in
 * units inside the rect. For single-click, additive=true toggles the
 * clicked unit. */
bool units_select_single_at_tile(UnitArray *u, int tx, int ty,
                                 int player_faction, bool additive);
void units_select_in_world_rect(UnitArray *u, Rectangle world_rect,
                                int player_faction, bool additive);
void units_clear_selection(UnitArray *u);
int  units_selected_count(const UnitArray *u);

void units_order_move(UnitArray *u, const TileMap *map, int gx, int gy);

/* Order a single unit to move to (gx, gy). Used by AI / scripted moves;
 * does NOT consult selection state. Returns true on success. */
bool units_order_move_one(UnitArray *u, const TileMap *map, int unit_id,
                          int gx, int gy);

/* Order all selected units to attack `target_id` — move toward it and
 * engage when in range. Targets are dropped if killed or change faction. */
void units_order_attack(UnitArray *u, const TileMap *map, int target_id);

/* Find a live unit whose tile_x/tile_y or next_x/next_y matches the
 * given tile, optionally restricted to a specific faction (-1 = any).
 * Returns -1 if none found. */
int  units_find_at_tile(const UnitArray *u, int tx, int ty, int faction);

#endif
