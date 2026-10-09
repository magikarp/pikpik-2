#!/bin/bash
# build_mac.sh — configure and build the macOS app.
#
#   ./build_mac.sh              # build into build/
#   ./build_mac.sh -c           # wipe the build directory first
#   ./build_mac.sh -DP2_UBSAN=ON        # extra CMake args pass straight through
#   BUILD_DIR=build-rel BUILD_TYPE=RelWithDebInfo ./build_mac.sh
#
# Deliberately thin. The interesting configuration lives in CMakeLists.txt and
# cmake/; this exists so "how do I build it" has one answer.
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$(pwd)"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

# The product's own name. Deliberately not the name of the game — see
# docs/LEGAL.md section 6. Changing it renames the .app.
APP_NAME="${APP_NAME:-pikpik-2}"
APP_ID="${APP_ID:-local.pikpik2.app}"

# Where your extracted disc lives. Overridable at runtime with
# P2_DISC_DIRECTORY, which is usually easier than rebuilding.
DISC_DIR="${DISC_DIR:-$ROOT/disc}"

if [ "${1:-}" = "-c" ] || [ "${1:-}" = "--clean" ]; then
    echo "==> removing $BUILD_DIR"; rm -rf "$BUILD_DIR"; shift
fi
if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then sed -n '2,10p' "$0"; exit 0; fi

for tool in cmake ninja python3; do
    command -v "$tool" >/dev/null || { echo "$tool not found — brew install $tool" >&2; exit 1; }
done
if [ ! -d vendor/pikmin2 ] || [ ! -d vendor/aurora ]; then
    echo "vendor/ is missing — run ./setup.sh first" >&2; exit 1
fi

echo "==> configuring $BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DP2_BUILD_RENDERER=ON \
    -DP2_DECOMP_DIR="$ROOT/vendor/pikmin2" \
    -DP2_AURORA_SOURCE="$ROOT/vendor/aurora" \
    -DP2_APP_TARGET=p2_story_bringup \
    -DP2_APP_NAME="$APP_NAME" \
    -DP2_APP_ID="$APP_ID" \
    -DP2_APP_DISC_ROOT="$DISC_DIR" \
    -DP2_DISC_DIR="$DISC_DIR" \
    "$@"

echo "==> building (first build fetches and compiles Aurora's dependencies)"
cmake --build "$BUILD_DIR" --target p2_story_bringup -j"$(sysctl -n hw.ncpu)"

APP="$BUILD_DIR/$APP_NAME.app"
echo
echo "built: $APP"
echo "run:   open $APP"
if [ ! -d "$DISC_DIR" ]; then
    echo
    echo "NOTE: no extracted disc at $DISC_DIR."
    echo "      The app needs one to start. See docs/GAME_DATA.md."
fi
