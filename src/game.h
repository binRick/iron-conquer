#ifndef IRON_GAME_H
#define IRON_GAME_H

#include "raylib.h"
#include "tilemap.h"
#include "units.h"
#include "palette.h"
#include "shp.h"
#include "tmp.h"

#define SIM_HZ 25
#define SIM_DT (1.0f / (float)SIM_HZ)

#define DECORATION_TMP_COUNT 8
#define MAX_DECORATIONS      80   /* sparse — a C&C map isn't littered */
#define BUILDING_SHP_COUNT   5
#define MAX_STRUCTURES       6    /* small civilian outposts */
#define UNIT_SHP_COUNT       9   /* harv, ftrk, e6, fact, sam2 (turret), silo (refinery), 4tnk (mammoth), e1 (rifle), gtnk (grizzly) */
#define MAX_EFFECTS          64
#define MAX_BUILD_QUEUE      6
#define VISION_RADIUS_TILES  7

typedef enum {
    VIS_HIDDEN     = 0,  /* never seen — drawn black */
    VIS_DISCOVERED = 1,  /* previously seen, not now — drawn dim, structures retain */
    VIS_VISIBLE    = 2,  /* in line-of-sight of a gold unit right now */
} VisibilityState;

typedef struct {
    unsigned char unit_type;
    float         elapsed_s;
} BuildOrder;

typedef struct {
    int        credits;
    int        spawn_x, spawn_y;
    BuildOrder queue[MAX_BUILD_QUEUE];
    int        queue_count;
    int        kills;        /* enemy units this faction has destroyed */
    int        losses;       /* own units this faction has lost */
    int        rally_x, rally_y;
    bool       rally_set;
} FactionState;

typedef enum {
    EFFECT_KIND_EXPLOSION    = 0,
    EFFECT_KIND_MUZZLE       = 1,
    EFFECT_KIND_DAMAGE_TEXT  = 2,   /* not a sprite — drawn by game.c */
    EFFECT_KIND_SMOKE        = 3,   /* primitive draw, rising puff */
    EFFECT_KIND_TRACER       = 4,   /* primitive draw, shot tracer line */
    EFFECT_KIND_DUST         = 5,   /* primitive draw, sand-coloured puff */
    EFFECT_KIND_MOVE_MARK    = 6,   /* primitive draw, green pulse + X */
    EFFECT_KIND_ATTACK_MARK  = 7,   /* primitive draw, red pulse + X */
    EFFECT_KIND_CRATER       = 8,   /* primitive draw, lingering scorch */
    EFFECT_KIND_PROMOTION    = 9,   /* "VETERAN!" / "ELITE!" floating text */
    EFFECT_KIND_FLAME        = 10,  /* primitive draw, orange flicker */
    EFFECT_KIND_COUNT,
} EffectKind;

#define MAX_STATUS_MSGS 6
#define STATUS_MSG_LEN  64

typedef struct {
    char  text[STATUS_MSG_LEN];
    float age;
    float duration;
    bool  active;
    unsigned char tone;   /* 0 = neutral, 1 = good, 2 = warning */
} StatusMsg;

typedef struct {
    bool          active;
    unsigned char kind;
    float         world_x;
    float         world_y;
    float         world_x2;         /* used by EFFECT_KIND_TRACER as endpoint */
    float         world_y2;
    float         age;
    float         duration;
    int           value;            /* used by EFFECT_KIND_DAMAGE_TEXT */
} Effect;

typedef struct {
    int           tile_x, tile_y;
    unsigned char tmp_index;
    unsigned char frame_index;
    signed char   jitter_x;
    signed char   jitter_y;
} Decoration;

typedef struct {
    int           tile_x, tile_y;
    unsigned char shp_index;
} Structure;

typedef struct {
    TileMap   map;
    Camera2D  camera;
    float     pan_speed;
    float     debug_log_acc;

    UnitArray units;
    float     sim_acc;
    long long sim_tick;

    bool      drag_button_down;
    bool      drag_active;
    Vector2   drag_start_screen;
    Vector2   drag_start_world;

    Palette    palette;            /* base palette for terrain/decorations/structures */
    bool       palette_loaded;
    Palette    team_palettes[UNIT_FACTION_COUNT];
    ShpSprite  unit_sprites[UNIT_SHP_COUNT * UNIT_FACTION_COUNT]; /* [type][faction] */

    TmpSprite  decoration_tmps[DECORATION_TMP_COUNT];
    bool       decoration_tmp_loaded[DECORATION_TMP_COUNT];
    Decoration decorations[MAX_DECORATIONS];
    int        decoration_count;

    ShpSprite  building_shps[BUILDING_SHP_COUNT];
    bool       building_shp_loaded[BUILDING_SHP_COUNT];
    Structure  structures[MAX_STRUCTURES];
    int        structure_count;

    ShpSprite  effect_sprites[EFFECT_KIND_COUNT];
    bool       effect_sprite_loaded[EFFECT_KIND_COUNT];
    Effect     effects[MAX_EFFECTS];
    StatusMsg  status_msgs[MAX_STATUS_MSGS];
    float      status_distress_acc;     /* throttle "base under attack" */

    Texture2D  cursor_tex;
    bool       cursor_loaded;

    Sound      sound_select;
    Sound      sound_build_done;
    Sound      sound_explosion;
    Sound      sound_deposit;
    Sound      sound_alarm;
    bool       audio_loaded;
    long long  last_alarm_tick;    /* throttle for base-under-attack alarm */

    FactionState factions[UNIT_FACTION_COUNT];
    float        credits_acc;       /* fractional credit accumulator */

    unsigned char visibility[MAP_HEIGHT][MAP_WIDTH]; /* VisibilityState */
    float         visibility_acc;   /* throttle: recompute periodically */

    int        screen_w;
    int        screen_h;
    bool       paused;
    /* 0 = playing, 1 = gold (player) won, 2 = red won. Latched on
     * the first frame a faction's construction yard is destroyed —
     * sim keeps running but a banner is drawn on top. */
    int        game_over;
    double     start_time;         /* GetTime() at game_init for elapsed clock */

    /* Headless-screenshot automation. When IRON_CONQUER_AUTO_SCREENSHOT
     * is set, the game runs for N seconds, captures `screenshot.png`,
     * and quits. F12 also captures any time. */
    double     screenshot_at;      /* 0 = disabled, else target GetTime() */
    bool       screenshot_exit;    /* true if the auto-shot should quit */
    bool       should_quit;        /* main loop exit signal */

    /* Sprite browser mode — shows a single SHP frame at 4× on a
     * checkered background so transparent pixels are obvious. Used
     * for visually verifying what's actually in the sprite. */
    bool       sprite_browser;
    int        sprite_browser_type;     /* unit_type index */
    int        sprite_browser_frame;
    int        sprite_browser_faction;  /* 0 = gold, 1 = red */
    float      ai_acc;
    float      ai_build_acc;
    float      ore_regrow_acc;     /* time accumulator for ore regrowth */
    float      smoke_acc;          /* throttle for damaged-building smoke */
    float      flame_acc;          /* throttle for low-HP-building flames */
    float      dust_acc;           /* throttle for moving-vehicle dust */
    int        ai_distress_x;      /* set when a red unit takes damage near base */
    int        ai_distress_y;
    long long  ai_distress_until;  /* sim_tick at which distress expires */

    /* Control groups: bit i set = unit i is in this group. Up to 9 groups
     * keyed to numbers 1..9; assigned with Ctrl+N, recalled with N. */
    unsigned long long groups[9];
} GameState;

void game_init(GameState *gs, int screen_w, int screen_h);
void game_update(GameState *gs, float dt);
void game_draw(GameState *gs);
/* Called from main.c AFTER EndDrawing — fires the screenshot trigger
 * so the framebuffer is fully composed before TakeScreenshot reads it. */
void game_post_draw(GameState *gs);
void game_shutdown(GameState *gs);

#endif
