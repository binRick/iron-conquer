# iron-conquer

A clone of Command and Conquer, built using raylib.

Single-executable, portable to macOS, Windows, and WebAssembly. Source is
plain C99; raylib is fetched via CMake `FetchContent` so there are no system
dependencies beyond CMake and a C compiler. The reference codebase is OpenRA,
vendored as a submodule under `third_party/OpenRA/` for reading only.

## Building

Desktop build + run (from project root):

    ./run.sh

The script ensures CMake is present, configures `build/` on first run,
compiles, and launches the game in the foreground.

## Debug log (Claude reads this)

When the game is launched via `./run.sh`, the script sets
`IRON_CONQUER_DEBUG_LOG=$PWD/debug.log` and truncates the file. The C code
checks this env var on startup (`debug_log_init` in `src/debug_log.c`); if
set, every significant event and a periodic state snapshot are appended,
flushed per line.

**This file is the primary channel for Claude to observe game state during
user testing.** When the user reports a bug or describes what they did, read
`debug.log` first instead of asking them to describe it.

Format: `[T+SECONDS] tag key=value key=value ...`

Tags currently emitted:
- `init` — once at startup; map size, tile size, screen, initial camera
- `state` — every 0.5s; camera target, zoom, fps, screen size
- `input` — edge events: `key_down=W`, `key_up=W`, `wheel=+0.10 zoom=1.10`,
  `mouse_left screen=(x,y) world=(x,y) tile=(x,y)`
- `shutdown` — clean exit only (signal kills won't write this)

When adding new game systems, add corresponding `debug_log(...)` calls so
their state is observable from the log without rebuilding.

## Architecture

**Sim/render split** (`src/game.c`). The simulation runs at a fixed `SIM_HZ`
(currently 25 Hz, OpenRA-style). `game_update` accumulates real time into
`sim_acc` and runs `units_tick` in a fixed-step `while` loop with a safety
cap of 8 sub-ticks per frame (prevents spiral-of-death on hitches).
`game_draw` extrapolates unit positions using `sim_alpha = sim_acc/SIM_DT`
so movement looks smooth at 60 Hz even though the sim only ticks 25 times
per second. Sim logic must stay deterministic (integer tile coords, no
`GetFrameTime`); only render reads `sim_alpha`.

**Subsystems**
- `tilemap.{c,h}` — 64×64 procedural tile grid, four tile types
- `pathfind.{c,h}` — A* on the tile grid with octile heuristic; cardinal
  cost 10, diagonal 14, no corner-cutting through walls. `tile_passable`
  is the single source of truth for what blocks units.
- `units.{c,h}` — `Unit` has integer `tile_x/tile_y` (sim authority) plus
  `next_x/next_y` and `move_progress` for the in-flight tile transition.
  Path is stored as packed `y*W+x` indices. Facing is 0..7 (N=0, clockwise).
- `game.{c,h}` — owns `GameState`, drives sim tick, handles input
  (WASD/arrow camera, mouse-wheel zoom, L-click/drag select, R-click move),
  draws everything.

**Input model (OpenRA-style)**
- Left-click on a gold unit's tile → select that unit (replaces selection).
  Enemy units cannot be selected; clicking an enemy deselects.
- Left-click on empty tile → clear selection
- Left-click and drag → rectangle select all gold units inside the rect
- Right-click on empty tile with selection → move order (BFS-spread goals)
- Right-click on enemy unit with selection → **attack order**: each
  selected unit's `target_id` is set, and they're routed toward the
  enemy's tile. Auto-engage halts movement once in range.

When adding a new movement-affecting tile type, update `tile_passable` in
`pathfind.c` — that's the only place the pathability rule lives.

## Unit occupancy and movement

**Tiles are exclusive.** Each live unit claims its current tile, plus its
in-flight destination tile if it's mid-move. `tile_claimed_by_other` in
`units.c` is the single source of truth. Pathfinding itself doesn't
consult occupancy — instead, `unit_try_advance_path_step` checks at the
moment a unit picks its next path tile, and waits one sim tick if the
tile is currently claimed. This produces correct chains (unit B follows
unit A with a one-tick gap) without static-deadlock fragility, and
costs one O(N) scan per path step (negligible at N≤64).

If two units want the same free tile in the same tick, the lower-id
unit wins (sim processes units in id order). The other waits.

**Multi-unit move orders spread.** When the user right-clicks with N
units selected, `units_order_move` BFS-floods from the click tile and
collects the first N passable tiles, then assigns one goal per unit in
selection order. Each unit pathfinds independently. This avoids the
"6 units all aim for the same square and deadlock" failure mode.

## Factions and palette remapping

Two factions: `FACTION_GOLD` (player/allies) and `FACTION_RED` (soviets).
Every unit has a `faction` byte. The base palette stays neutral; per-team
palettes are built at init by `palette_remap_team`, which substitutes
indices 80..95 (the Westwood "remap colours") with a 16-shade ramp. We
ship a gold ramp and a red ramp in `game.c:g_team_ramps[]`.

Each unit SHP is loaded once **per faction**, producing a 2D sprite array
laid out as `unit_sprites[type * UNIT_FACTION_COUNT + faction]`. This
doubles texture memory but means rendering is a flat lookup with no
per-frame palette work. Decorations and structures stay neutral (no
team colour).

## Combat

Per-unit-type combat stats (`UnitCombatInfo` in `units.h`,
`g_unit_combat_info[]` in `game.c`):
- HP max
- Attack damage
- Attack range (tiles)
- Attack cooldown (seconds)

Harvesters have damage 0 — tough but defenceless, like in C&C/RA.
Flame trucks short-range high-damage, infantry longer-range light
damage.

`units_tick` drives combat in the same loop as movement:
1. If a unit has a target, validate it (alive, hostile, in range);
   drop it otherwise.
2. If no target and the unit isn't moving, scan all units for the
   nearest enemy in range and acquire it.
3. If target acquired and `attack_cd` ≤ 0, face the target, deal
   `attack_damage`, reset cooldown.
4. When HP reaches 0: `alive = false`, paths cleared. The renderer
   skips dead units.

HP bars draw above every unit (`draw_hp_bar` in `units.c`) — green
> 66 % HP, yellow ≥ 33 %, red below.

## AI

The 1 Hz "AI tick" in `game_update` does **wave dispatching** plus a
**defensive rally** override.

- **Waves.** Idle red combat units (excluding harvesters / stationary
  buildings) are gathered into a list. When the list reaches the
  current `wave_size` threshold, the whole wave is dispatched to a
  single attack point in one tick. Threshold = 4, +1 every 60 s,
  capped at 8 — so late-game waves are bigger and more dangerous.
- **Defense.** Whenever a red unit takes damage within 12 tiles of
  the red factory, `ai_distress_{x,y}` is set and `ai_distress_until`
  is bumped 8 s into the future. While distress is active, wave size
  drops to 2 and the dispatch target becomes the distress location
  rather than the gold base — reds rally to defend.
- **Target selection.** Without distress, the wave attacks the gold
  factory if alive; otherwise the nearest non-harvester gold unit.
- **Approach handling.** The target tile is run through
  `nearest_passable_tile` so attacks against a building (whose anchor
  tile is blocked) become "march to the closest passable neighbour",
  which is what the gameplay actually needs.

## Effects, polish, UI

**Effect system.** `Effect[MAX_EFFECTS]` is a fixed-size pool; each
slot has a `kind` byte indicating how it's drawn:
- `EFFECT_KIND_EXPLOSION` (`napalm1.shp`, 14 frames @ 25×25, 0.45 s) —
  spawned once per unit transition from `alive=true` to `alive=false`,
  gated by `Unit.death_acked`.
- `EFFECT_KIND_MUZZLE` (`gunfire2.shp`, 5 frames @ 16×16, 0.10 s) —
  spawned once per shot, gated by `Unit.just_fired`.
- `EFFECT_KIND_DAMAGE_TEXT` — no sprite; draws `-N` floating up over
  0.7 s, fading. Spawned from `Unit.damage_pending`.
- `EFFECT_KIND_SMOKE` — no sprite; primitive dark circle that rises
  36 px and grows 4 → 8 px while fading from α 180 over 1.4 s.
  Spawned every 0.4 s from any stationary unit below 50 % HP.

Sprite-backed kinds step the frame as
`frame = (age/duration) × frame_count`. Drawn after units, before fog.

## Visual damage feedback

Three layers stack:
- **Sustained damage tint** — sprites darken to 55 % at 0 HP via a
  linear ramp below 60 % HP. Done in `units_draw`.
- **Transient damage flash** — `Unit.damage_flash` set to 120 ms on
  every hit; lerps the tint toward saturated red.
- **Building smoke** — see above.

Combined with the persistent HP bar, floating "-N" damage numbers,
muzzle flashes, and audio cues, every level of combat is communicated.

## Kill / loss tracking

Each `FactionState` carries `kills` and `losses` counters, displayed
top-right beneath the unit count. Credited from the death-detection
pass in `game_update`: when a unit goes from alive to dead and its
`killer_id` is valid, the killer's faction gets +1 kill and the
target's faction gets +1 loss. `Unit.killer_id` is set in
`apply_damage` at the moment HP reaches 0.

**Damage flash.** `Unit.damage_flash` is set to 0.12 s whenever
`apply_damage` lands; `units_tick` decays it; `units_draw` lerps the
sprite tint toward saturated red while it's > 0. Makes hits visible at
a glance.

**Restart.** `R` re-runs `game_init` in place, preserving the cursor
texture (cheap re-init: tilemap rebake, asset reload, RNG re-seed →
same map every time, so retries are deterministic).

**Mouse cursor.** `mods/common-content/cursor.png` is a 10×16 arrow.
Loaded once, system cursor hidden via `HideCursor()`, drawn at 2×
scale at the mouse position each frame. Hot-spot at top-left.

**Minimap.** Bottom-left, 256×256 with a 2 px black border. Drawn
last (over the world view but below the cursor). Background is the
baked tilemap texture downscaled in one `DrawTexturePro` call; unit
positions are drawn as 2 px circles (gold/red); the camera viewport
is a white outlined rectangle on top. Click or drag in the minimap
pans the camera to that point in world space — input handler
short-circuits selection/order paths so you don't deselect when
clicking the map.

## Economy and production

**Per-faction state.** `FactionState` (in `game.h`) tracks `credits`,
`spawn_x/spawn_y` (rally tile for new units), and a `queue` of build
orders. Both factions start with `STARTING_CREDITS = 600` and earn
`CREDITS_PER_SECOND = 6` passively (no harvester collection yet — that
would need ore patches and a refinery).

**Build pipeline.** `g_unit_production_info[]` defines per-type
`{ credit_cost, build_time_s }`:
- harvester: $800 / 22 s
- flame truck: $600 / 16 s
- infantry: $100 / 6 s

`try_enqueue_build` deducts the cost up-front and pushes onto the
queue. `update_economy` advances head's `elapsed_s` each frame; when
it reaches `build_time_s`, it spawns the unit at the rally tile (or
nearest free passable tile via `find_free_tile_near`), pops the head.
Cost is non-refundable on cancel — there's no cancel UI yet.

**Player UI.** Three rectangular buttons top-left under the credits
display, one per unit type. Click to enqueue. Hover highlights;
unaffordable buttons render dim. Active head's progress bar runs
across the button bottom. Queue length shown as "x N" badge in the
button corner. The input handler short-circuits build-button clicks
so they don't fall through to selection/order logic.

**Red AI build loop.** Every 3 s, the red AI tries to enqueue cheap
infantry first, then flame truck, then harvester — produces a steady
stream of attackers without burying the player.

When extending: every new unit type needs entries in
`g_unit_sprite_paths`, `g_unit_sprite_info`, `g_unit_combat_info`,
`g_unit_production_info`, and `g_unit_short_names` (parallel arrays
of size `UNIT_SHP_COUNT`). If the new type is buildable, also extend
`g_buildable_types[]` and bump `BUILDABLE_COUNT`.

## Refinery and ore regrowth

Each faction also spawns with one **refinery** (`UNIT_TYPE_REFINERY = 5`,
sprite `mods/ra/bits/weap3.shp`, 10 frames @ 72×48) at a fixed offset
from the factory (4 tiles W of gold's, 4 tiles E of red's). Refineries
share the 2×2 footprint + blocked-tile machinery with factories.

Harvesters now route to the **nearest friendly deposit** (refinery
preferred, factory as fallback) at the GATHERING → RETURNING
transition. `find_nearest_deposit_tile` walks the 8 perimeter tiles
of every alive friendly factory/refinery and picks the closest
passable one — that becomes the harvester's `harvester_home_{x,y}`
for that round trip. If the deposit dies mid-cycle, the next round
trip retargets automatically; if both are dead, the harvester goes
IDLE permanently.

**Ore regrowth.** Every 60 s, one `TILE_DIRT` tile that has at least
one `TILE_ORE` neighbour gets converted to `TILE_ORE` and the texture
patched via `tilemap_repaint_tile`. Slow growth, scales with surviving
patches — abandoned patches eventually run dry but active patches
slowly creep outward.

## Stationary unit types (factory + turret)

`unit_type_is_stationary` (in `units.h`, inline) flags `UNIT_TYPE_FACTORY`
and `UNIT_TYPE_TURRET`. In `units_tick`, stationary units `goto
combat_block` past the movement code — paths and walk anim never run
for them. The combat block still gates on `attack_damage > 0`, so
factories (damage 0) skip combat too while turrets (damage 14, range
6 tiles) participate normally. Both can take damage from any attacker.

**Turret** uses `mods/ra/bits/sam2.shp` (68 frames @ 48×24, treated as
a 32-facing static SHP). HP 220, $700 to build, 12 s build time. The
initial spawn cluster cycles through buildable types so each faction
starts with one turret already deployed; the player can queue more
from the build buttons.

The simple AI also rotates through `g_buildable_types`, biased by a
`sim_tick`-derived rotation so the red base eventually invests in
turrets when credits permit, instead of being purely an infantry
deathball.

## Fog of war

Per-tile visibility state (`gs->visibility[y][x]`):
- `VIS_HIDDEN` (0)     — never seen; rendered with a 90 % black overlay
- `VIS_DISCOVERED` (1) — seen at some point but not now; rendered with
  a 43 % black overlay (terrain + structures still readable)
- `VIS_VISIBLE` (2)    — currently in the line-of-sight of a live gold
  unit; no overlay

Recomputed every 200 ms (5 Hz) — sufficient at our movement speeds and
saves the O(units · radius²) scan from running every frame. The
recompute first demotes all `VIS_VISIBLE` tiles to `VIS_DISCOVERED`,
then re-promotes anything within `VISION_RADIUS_TILES` (= 7) of any
live gold unit using a circle test.

The fog overlay is drawn inside the world `BeginMode2D` block, last
(after units, structures, decorations, effects). This means red units
in fog are physically covered by the overlay rectangle — no separate
"hide enemies" path needed. HP bars + selection brackets are also
covered, so a red unit walking through your old territory is
genuinely invisible until a gold unit gets close.

The minimap currently ignores fog (always shows everything). Adding
fog there is on the list.

## Harvester economy

Five `TILE_ORE` patches scattered around the map (painted in
`paint_ore_patches` after the base terrain is generated). Tiles are
passable for all units; the bright yellow-green pixel-noise treatment
in the baked tilemap makes them visually distinct.

Every harvester (`unit_type == 0`) runs an FSM in `harvester_tick`:
`HARV_IDLE` → `HARV_GOING_TO_ORE` (path to nearest unclaimed ore tile)
→ `HARV_GATHERING` (3 s on the ore tile, depletes the tile to dirt) →
`HARV_RETURNING` (path back to its spawn-time `harvester_home_{x,y}`)
→ `HARV_UNLOADING` (1 s within 2 tiles of home) → +50 credits → IDLE.

The ore-tile selection rules out tiles already claimed by other
harvesters in `GOING_TO_ORE` or `GATHERING`, so two harvesters never
fight for the same ore square (a previous version deadlocked when both
gold harvesters claimed (16,21)).

Ore depletion is visible: when gathering completes, the tile flips
from `TILE_ORE` to `TILE_DIRT` and `tilemap_repaint_tile` patches the
baked tilemap texture in-place via `UpdateTextureRec` — patches shrink
as harvesters work them.

Manual orders (right-click move, or S = stop) clear paths and reset
the harvester to `HARV_IDLE`; on the next tick the FSM re-acquires a
free ore patch and resumes harvesting. So manually directing a
harvester is a "detour" by default. To stop a harvester from
auto-resuming, you'd need to add a `manual_hold` flag — not done yet.

## Audio

raylib's audio device is initialised once on first `game_init` and
preserved across restarts (`saved_audio_loaded` etc.). All sounds are
**synthesised at runtime** — no audio assets are vendored — by
`synth_beep` (sine + linear-decay envelope, optional pitch sweep,
mild saturation for chip-tune flavor) and `synth_noise` (LCG noise
through a one-pole low-pass, decay envelope) producing PCM samples
straight into a `Wave` struct, then `LoadSoundFromWave` to a `Sound`.

Three sounds:
- `sound_select`     — 740 Hz blip, 60 ms, played when the player's
  selection grows (single click that hits a gold unit, or drag-rect
  that adds at least one unit). Suppressed if the click deselected
  everything.
- `sound_build_done` — 440→880 Hz sweep, 180 ms, played only when
  the **gold** faction completes a build (silent for AI builds).
- `sound_explosion`  — noise burst, 400 ms, played on every unit
  death (both factions).

Adding new sounds: keep them short (≤ 250 ms) and let the
`PlaySound` calls live near the events that trigger them; raylib's
audio mixer handles concurrent playback fine.

## Construction Yards (factories)

Each faction spawns with one `UNIT_TYPE_FACTORY` (= 3) at its rally
tile. The sprite is `mods/ra/bits/fact.shp` (52 frames @ 72×72) loaded
once per faction palette. Factories are units in the existing
`Unit[]` array but flagged stationary in `units_tick` —
`if (unit->unit_type == UNIT_TYPE_FACTORY) continue;` skips the
movement block entirely. Their `attack_damage = 0` keeps them out of
the combat target-acquisition path.

Their HP = 800. Other units' auto-engage and the simple AI's "march
toward nearest gold" logic both target factories like any other
unit, so the factory becomes a real strategic objective. When a
faction's factory dies, `faction_has_factory` returns false →
`try_enqueue_build` rejects further builds → that faction can't
replenish losses. AI auto-build silently fails too. Eventually the
remaining units die, and `VICTORY` / `DEFEAT` triggers via the
existing 0-units-alive check.

`g_buildable_types[] = { 0, 1, 2 }` excludes factories from build
buttons / AI auto-build. `BUILDABLE_COUNT` (3) drives both
`build_button_rect` slot indexing and `spawn_cluster`'s round-robin
unit-type assignment so initial clusters don't accidentally include
factories.

## Buildings as obstacles

Factories occupy a **2×2 footprint** of blocked tiles. `TileMap.blocked[][]`
is an overlay consulted by `tile_passable` (alongside the tile-type
check). At spawn we flip the four corner tiles to dirt first so a
chance water/rock underneath doesn't strand the building, then call
`tilemap_set_blocked(map, anchor_x, anchor_y, 2, 2, true)`. Pathfind
naturally routes around the footprint; `spawn_cluster` skips it via
`tile_passable`. When a factory dies (`death_acked` transition), the
2×2 is unblocked so units can walk across the rubble.

## RTS controls

- Arrow keys / edge-of-screen scroll → camera pan
- Mouse wheel → zoom
- Left-click on gold unit / drag-rect → select (gold only)
- `Shift`+L-click / `Shift`+drag → add to selection (toggle on click)
- Right-click on empty tile → move selected units (BFS spread goals)
- Right-click on enemy unit → attack-target selected units
- `S` → stop selected units (clears path, target, harvester state)
- `Ctrl+1`..`Ctrl+9` → assign current selection to control group N
- `1`..`9` → recall control group N
- `H` → snap camera to gold home base
- `F` → snap camera to centroid of current selection
- `P` → toggle pause (sim/economy/AI/harvesters frozen; input still live)
- `R` → restart battle (deterministic seed, same map)
- Click in minimap → pan camera there
- L-click build button → enqueue
- R-click build button → cancel last queued of that type, 50% refund

Infantry SHPs lay out frames as `[stand × 8] [walk1 × 8] [walk2 × 8] …`.
A `UnitSpriteInfo` per sprite-type tells the renderer how many facings
(8 or 32), how many walk-cycle frames exist, and at what frame offset
they start. While `Unit.next != Unit.tile` (i.e. moving), `units_tick`
accumulates `walk_acc` and bumps `walk_frame` every
`WALK_FRAME_DURATION` (100 ms = ~10 fps walk cycle). When idle,
`walk_frame` resets to 0 so the unit lands on the stand pose. Vehicles
have `walk_frame_count = 0` and ignore the walk path entirely — they
animate purely through facing rotation.

## Westwood asset pipeline

iron-conquer reads OpenRA's vendored sprite assets directly from the
`third_party/OpenRA/` submodule — no external preprocessing step. The
formats are EA's original Westwood Studios formats from C&C / Red Alert,
released as freeware in 2007–2008 (non-commercial use only; not
redistributable in a paid product).

**Reference implementations** (read-only, do not edit):
- `third_party/OpenRA/OpenRA.Mods.Cnc/SpriteLoaders/ShpTDLoader.cs`
- `third_party/OpenRA/OpenRA.Mods.Cnc/FileFormats/LCWCompression.cs`
- `third_party/OpenRA/OpenRA.Mods.Cnc/FileFormats/XORDeltaCompression.cs`

When porting a new format, read the OpenRA C# loader first — it's the
ground truth.

**`palette.{c,h}`** — loads a 768-byte `.pal` file (256 RGB triples in
6-bit-per-channel form, range 0..63). Expands each component to 8-bit by
replicating the high bits into the low (`(x<<2) | (x>>4)`). Index 0 is
the colour key — the loader marks it transparent so sprites composite
cleanly.

**`shp.{c,h}`** — Westwood SHP_TD format. Header is 14 bytes
(`u16 imageCount; 4 zero bytes; u16 width; u16 height; 4 zero bytes`),
then `imageCount` 8-byte frame headers (`u32 packed: low 24 = file
offset, high 8 = format`; `u16 refOffset; u16 refFormat`), then 16 bytes
of EOF + zero header, then the frame data. Three frame formats:
`0x80` = LCW-compressed pixels (Format80); `0x40` = LCW-decompressed
pixels XOR-applied against another frame referenced by `refOffset`;
`0x20` = XOR-applied against the immediately preceding frame.
`shp_load` decompresses every frame to an indexed bitmap, palette-maps
to RGBA, and uploads each as a `Texture2D`.

Asset paths are resolved relative to the working directory, which
`run.sh` sets to the project root via `cd "$(dirname "$0")"`. Running
`./build/iron_conquer` directly from another directory will fail to
load assets — use `run.sh` or `cd` to the project root first.

**Unit sprite mapping.** Westwood SHP convention: frame 0 = facing East,
frames advance counter-clockwise. Each unit type needs its facing count
passed explicitly (`g_unit_sprite_facings` in `game.c`):
- **Vehicles use 32 facings.** Our facing 0 (N) maps to frame 8 →
  `(8 - 4*facing + 32) % 32`. Vehicle SHPs typically have many more
  frames than 32 (e.g. `harv.shp` is 111 because there are 32 facings ×
  3 cargo states + dump animation), so frame count alone is not a
  reliable signal.
- **Infantry use 8 facings.** Facing 0 (N) maps to frame 2 →
  `(2 - facing + 8) % 8`. The first 8 frames are stand-facings; the
  remaining hundreds of frames are walk/attack/death animations we
  don't yet drive (always rendered standing).

When adding a new unit SHP, also extend `g_unit_sprite_facings[]` with
its facing count (32 for vehicles, 8 for infantry). Mismatched values
give nonsense rotations, not crashes.

**`tmp.{c,h}`** — TmpRA terrain template parser. Format reference:
`OpenRA.Mods.Cnc/SpriteLoaders/TmpRALoader.cs`. A TmpRA file is a
collection of `count` sub-tiles (each `width × height`, raw indexed
pixels — no compression). Magic: `u32 == 0` at offset 20, `u16 == 0x2C73`
at offset 26. Index bytes of `0xFF` denote blank slots — those frames
are skipped (their `Texture2D.id` stays 0; callers must check).

Note: files with the `.tem` extension in `mods/cnc/bits/` are actually
**SHP_TD format**, not TmpTD/TmpRA — load them with `shp_load`. Files in
`mods/ra/bits/*.tem` are TmpRA format. OpenRA's `mod.yaml`
`SpriteFormats: ShpTD, TmpTD, ShpTS, TmpRA` confirms loaders are tried
in turn until one matches.

## Map rendering

`tilemap_build_texture` bakes the entire 2048×2048 map into a single
RGBA `Texture2D` once at init. Each pixel is generated by
`pixel_color_at` which composes:

- **Base tile colour** + per-tile RGB jitter (each tile gets ±16 RGB
  offset so adjacent grass tiles don't all read identical).
- **Three octaves of noise**:
  - high-frequency `hash2`-based grit (±16 per pixel)
  - mid-frequency bilerp noise at 8-px grid (~±42)
  - low-frequency bilerp noise at 32-px grid (~±25)
  combined to produce structure at multiple scales — the map looks
  like terrain rather than uniform fill.
- **Radial darkening** toward each tile's edges (subtle vignette).
- **Tile-type-specific speckle**:
  - DIRT: dark cracks + bright sand flecks
  - ROCK: dark fissures + crystalline highlights
  - GRASS: bright tufts (cluster centres driven by mid-freq noise so
    they form patches, not random pixels) + occasional dark shadows
  - WATER: foam/wave highlights
  - ORE: dense bright crystalline flecks
- **Soft edge blending** at grass↔dirt and ore↔grass/dirt boundaries
  with per-pixel jitter so the seam doesn't read as a clean line.
  Water and rock keep hard edges (shoreline / cliff feel).

`tilemap_draw` blits the baked texture in a single call.
`tilemap_repaint_tile` patches a single tile in place via
`UpdateTextureRec` (used after ore depletion / regrowth so the visual
keeps up with the data).

Decorations (`Decoration[]` in `GameState`) are placed once at init
using a deterministic xorshift32, restricted to grass/dirt tiles, with
a small no-spawn radius around the unit cluster. Each decoration is
just `(tile_x, tile_y, tmp_index, frame_index, jitter_x, jitter_y)` —
the actual `Texture2D` is looked up at draw time from the loaded
`TmpSprite` array. Drawn after the baked tilemap and before units, so
units always render on top of decorations.
