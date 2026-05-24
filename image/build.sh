#!/usr/bin/env bash
# Build dist/image.wasm via emscripten (stb header-only + libwebp).
#
# Deps: emcc + libwebp source. Resolved from one of:
#   1. $AGLET_DEPS_DIR/<hash>
#   2. ../aglet/zig-pkg/<hash>  (sibling aglet checkout)
#   3. ./vendor/zig-pkg/<hash>  (run ./scripts/fetch-deps.sh)

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGINS_ROOT="$(cd "$PLUGIN_DIR/.." && pwd)"
LIBWEBP_HASH="N-V-__8AAGkrewARcKiSyLfJKsaW0ZoqHh4hzi4mTGwekxjk"

if [[ -n "${AGLET_DEPS_DIR:-}" && -d "$AGLET_DEPS_DIR/$LIBWEBP_HASH" ]]; then
  LIBWEBP_ROOT="$AGLET_DEPS_DIR/$LIBWEBP_HASH"
elif [[ -d "$PLUGINS_ROOT/../aglet/zig-pkg/$LIBWEBP_HASH" ]]; then
  LIBWEBP_ROOT="$PLUGINS_ROOT/../aglet/zig-pkg/$LIBWEBP_HASH"
elif [[ -d "$PLUGINS_ROOT/vendor/zig-pkg/$LIBWEBP_HASH" ]]; then
  LIBWEBP_ROOT="$PLUGINS_ROOT/vendor/zig-pkg/$LIBWEBP_HASH"
else
  echo "[image] libwebp source not found. Run ./scripts/fetch-deps.sh." >&2
  exit 1
fi

command -v emcmake >/dev/null || { echo "emscripten not installed"; exit 1; }

BUILD_DIR="$PLUGIN_DIR/build"
DIST_DIR="$PLUGIN_DIR/dist"
mkdir -p "$BUILD_DIR" "$DIST_DIR"
export LIBWEBP_ROOT

cd "$BUILD_DIR"
[ -f CMakeCache.txt ] || emcmake cmake "$PLUGIN_DIR"
cmake --build . -j
cp image.wasm "$DIST_DIR/"
