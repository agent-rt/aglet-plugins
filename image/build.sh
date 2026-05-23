#!/usr/bin/env bash
# 编 image.wasm —— stb（header-only）+ libwebp（vendored via zig fetch）。
#
# 用法：
#   cd aglet-plugins/image
#   ./build.sh
#
# 依赖：emcc + libwebp 源（zig fetch 拉到 zig-pkg/）

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGINS_ROOT="$(cd "$PLUGIN_DIR/.." && pwd)"

# 依赖源路径优先级：
#   1. $AGLET_DEPS_DIR (显式指定的 zig-pkg 目录)
#   2. ../aglet/zig-pkg (sibling aglet checkout，dev 常用)
#   3. ./vendor/zig-pkg (本仓 own vendored，CI 走这条)
# libwebp v1.6.0 hash N-V-__8AAGkrew...（如改版本同步更新）
LIBWEBP_HASH="N-V-__8AAGkrewARcKiSyLfJKsaW0ZoqHh4hzi4mTGwekxjk"

if [[ -n "${AGLET_DEPS_DIR:-}" && -d "$AGLET_DEPS_DIR/$LIBWEBP_HASH" ]]; then
    LIBWEBP_ROOT="$AGLET_DEPS_DIR/$LIBWEBP_HASH"
elif [[ -d "$PLUGINS_ROOT/../aglet/zig-pkg/$LIBWEBP_HASH" ]]; then
    LIBWEBP_ROOT="$PLUGINS_ROOT/../aglet/zig-pkg/$LIBWEBP_HASH"
elif [[ -d "$PLUGINS_ROOT/vendor/zig-pkg/$LIBWEBP_HASH" ]]; then
    LIBWEBP_ROOT="$PLUGINS_ROOT/vendor/zig-pkg/$LIBWEBP_HASH"
else
    echo "[image] libwebp 源没找到。试以下任一：" >&2
    echo "  1. clone aglet sibling repo + 跑 'just build-zig' 触发 zig fetch" >&2
    echo "  2. 设 \$AGLET_DEPS_DIR=/path/to/zig-pkg" >&2
    echo "  3. CI 路径：./scripts/fetch-deps.sh (TODO)" >&2
    exit 1
fi

if ! command -v emcmake >/dev/null 2>&1; then
    echo "[image] emscripten 没装；brew install emscripten" >&2
    exit 1
fi

BUILD_DIR="$PLUGIN_DIR/build"
DIST_DIR="$PLUGIN_DIR/dist"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

export LIBWEBP_ROOT
cd "$BUILD_DIR"

if [[ ! -f CMakeCache.txt ]]; then
    emcmake cmake "$PLUGIN_DIR"
fi
cmake --build . -j

cp image.wasm "$DIST_DIR/image.wasm"
echo "[image] ✓ dist/image.wasm $(wc -c < "$DIST_DIR/image.wasm") bytes"
