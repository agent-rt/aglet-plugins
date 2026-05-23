#!/usr/bin/env bash
# 编 barcode.wasm —— standalone wasm（无 embind JS glue）通过 emscripten。
#
# 用法：
#   cd aglet-plugins/barcode
#   ./build.sh           # 默认 Release，出 dist/barcode.wasm
#
# 依赖：
#   - emcc（brew install emscripten）
#   - zxing-cpp 源（zig fetch 拉到 zig-pkg/N-V-__8AALGr...，路径靠 build.zig.zon hash）

set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGINS_ROOT="$(cd "$PLUGIN_DIR/.." && pwd)"

# 依赖源路径优先级：
#   1. $AGLET_DEPS_DIR (显式指定的 zig-pkg 目录)
#   2. ../aglet/zig-pkg (sibling aglet checkout，dev 常用)
#   3. ./vendor/zig-pkg (本仓 own vendored，CI 走这条)
ZXING_HASH="N-V-__8AALGrMwDZ4Ry7UDuLwgq1EZDZbRbLN75PkGihtghn"

if [[ -n "${AGLET_DEPS_DIR:-}" && -d "$AGLET_DEPS_DIR/$ZXING_HASH" ]]; then
    ZXING_CPP_ROOT="$AGLET_DEPS_DIR/$ZXING_HASH"
elif [[ -d "$PLUGINS_ROOT/../aglet/zig-pkg/$ZXING_HASH" ]]; then
    ZXING_CPP_ROOT="$PLUGINS_ROOT/../aglet/zig-pkg/$ZXING_HASH"
elif [[ -d "$PLUGINS_ROOT/vendor/zig-pkg/$ZXING_HASH" ]]; then
    ZXING_CPP_ROOT="$PLUGINS_ROOT/vendor/zig-pkg/$ZXING_HASH"
else
    echo "[barcode] zxing-cpp 源没找到。试以下任一：" >&2
    echo "  1. clone aglet sibling repo + 跑 'just build-zig' 触发 zig fetch" >&2
    echo "  2. 设 \$AGLET_DEPS_DIR=/path/to/zig-pkg" >&2
    echo "  3. CI 路径：./scripts/fetch-deps.sh (TODO Phase B)" >&2
    exit 1
fi

if ! command -v emcmake >/dev/null 2>&1; then
    echo "[barcode] emscripten 没装；mac 上跑 'brew install emscripten'" >&2
    exit 1
fi

BUILD_DIR="$PLUGIN_DIR/build"
DIST_DIR="$PLUGIN_DIR/dist"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

export ZXING_CPP_ROOT
cd "$BUILD_DIR"

if [[ ! -f CMakeCache.txt ]]; then
    emcmake cmake "$PLUGIN_DIR"
fi
cmake --build . -j

cp barcode.wasm "$DIST_DIR/barcode.wasm"
echo "[barcode] ✓ dist/barcode.wasm $(wc -c < "$DIST_DIR/barcode.wasm") bytes"
