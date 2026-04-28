#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake is required but was not found." >&2
    case "$(uname -s)" in
        Darwin) echo "  install with: brew install cmake" >&2 ;;
        Linux)  echo "  install with: sudo apt install cmake build-essential" >&2 ;;
        *)      echo "  see https://cmake.org/download/" >&2 ;;
    esac
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

export IRON_CONQUER_DEBUG_LOG="$PWD/debug.log"
: > "$IRON_CONQUER_DEBUG_LOG"
echo ">> debug log: $IRON_CONQUER_DEBUG_LOG"

echo ">> launching $EXE"
exec "$EXE"
