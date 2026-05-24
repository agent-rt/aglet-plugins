#!/usr/bin/env bash
# Build dist/barcode.wasm via emscripten.
#
# Deps: emcc + zxing-cpp source. Resolved from one of:
#   1. $AGLET_DEPS_DIR/<hash>
#   2. ../aglet/zig-pkg/<hash>  (sibling aglet checkout)
#   3. ./vendor/zig-pkg/<hash>  (run ./scripts/fetch-deps.sh)

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGINS_ROOT="$(cd "$PLUGIN_DIR/.." && pwd)"
ZXING_HASH="N-V-__8AALGrMwDZ4Ry7UDuLwgq1EZDZbRbLN75PkGihtghn"

if [[ -n "${AGLET_DEPS_DIR:-}" && -d "$AGLET_DEPS_DIR/$ZXING_HASH" ]]; then
  ZXING_CPP_ROOT="$AGLET_DEPS_DIR/$ZXING_HASH"
elif [[ -d "$PLUGINS_ROOT/../aglet/zig-pkg/$ZXING_HASH" ]]; then
  ZXING_CPP_ROOT="$PLUGINS_ROOT/../aglet/zig-pkg/$ZXING_HASH"
elif [[ -d "$PLUGINS_ROOT/vendor/zig-pkg/$ZXING_HASH" ]]; then
  ZXING_CPP_ROOT="$PLUGINS_ROOT/vendor/zig-pkg/$ZXING_HASH"
else
  echo "[barcode] zxing-cpp source not found. Run ./scripts/fetch-deps.sh." >&2
  exit 1
fi

command -v emcmake >/dev/null || { echo "emscripten not installed"; exit 1; }

BUILD_DIR="$PLUGIN_DIR/build"
DIST_DIR="$PLUGIN_DIR/dist"
mkdir -p "$BUILD_DIR" "$DIST_DIR"
export ZXING_CPP_ROOT

cd "$BUILD_DIR"
[ -f CMakeCache.txt ] || emcmake cmake "$PLUGIN_DIR"
cmake --build . -j
cp barcode.wasm "$DIST_DIR/"
