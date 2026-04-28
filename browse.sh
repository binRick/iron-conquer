#!/usr/bin/env bash
# Launch iron-conquer in sprite-browser mode.
#
# Usage:
#   ./browse.sh                   # defaults to weap3 (war factory / refinery)
#   ./browse.sh harv              # harvester
#   ./browse.sh fact              # construction yard
#   ./browse.sh sam2              # sam turret
#   ./browse.sh e6                # infantry
#   ./browse.sh ftrk              # flame truck
#
# In-browser controls:
#   ←/→  step frame
#   ↑/↓  cycle unit type
#   Tab  toggle gold/red faction palette
#   F12  manual screenshot to ./screenshot.png
#   ESC  exit (or F1 toggles back to game)

set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"
TYPE="${1:-weap3}"

case "$TYPE" in
    harv|ftrk|e6|inf|fact|sam2|tur|weap3|ref) ;;
    *)
        echo "error: unknown sprite type '$TYPE'." >&2
        echo "valid: harv, ftrk, e6/inf, fact, sam2/tur, weap3/ref" >&2
        exit 1 ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake is required but was not found." >&2
    exit 1
fi

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo ">> configuring ($BUILD_DIR)"
    cmake -S . -B "$BUILD_DIR"
fi

echo ">> building"
cmake --build "$BUILD_DIR" --parallel

EXE="$BUILD_DIR/iron_conquer"
if [ ! -x "$EXE" ] && [ -x "$EXE.exe" ]; then
    EXE="$EXE.exe"
fi

export IRON_CONQUER_VIEW_SHP="$TYPE"
export IRON_CONQUER_DEBUG_LOG="$PWD/debug.log"
: > "$IRON_CONQUER_DEBUG_LOG"

echo ">> sprite browser: $TYPE"
echo ">> debug log:      $IRON_CONQUER_DEBUG_LOG"
echo ">> launching       $EXE"
exec "$EXE"
