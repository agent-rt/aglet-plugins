#!/usr/bin/env bash
# Vendor C/C++ deps into vendor/zig-pkg/<hash>/ so build.sh can run without
# a sibling aglet checkout. Hashes match aglet/build.zig.zon.
#
#   ./scripts/fetch-deps.sh         # all
#   ./scripts/fetch-deps.sh zxing   # one

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$ROOT/vendor/zig-pkg"
mkdir -p "$VENDOR"

ZXING_HASH="N-V-__8AALGrMwDZ4Ry7UDuLwgq1EZDZbRbLN75PkGihtghn"
ZXING_URL="https://github.com/zxing-cpp/zxing-cpp/archive/refs/tags/v3.0.2.tar.gz"

WEBP_HASH="N-V-__8AAGkrewARcKiSyLfJKsaW0ZoqHh4hzi4mTGwekxjk"
WEBP_URL="https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz"

fetch_one() {
  local name="$1" hash="$2" url="$3"
  local target="$VENDOR/$hash"
  if [ -d "$target" ] && [ -n "$(ls -A "$target" 2>/dev/null)" ]; then
    echo "[$name] cached"
    return 0
  fi
  echo "[$name] fetching"
  mkdir -p "$target"
  curl -fsSL "$url" | tar -xz --strip-components=1 -C "$target"
}

dispatch() {
  case "$1" in
    zxing) fetch_one zxing "$ZXING_HASH" "$ZXING_URL" ;;
    webp)  fetch_one webp  "$WEBP_HASH"  "$WEBP_URL" ;;
    *) echo "unknown dep: $1 (known: zxing webp)" >&2; exit 1 ;;
  esac
}

if [ $# -eq 0 ]; then
  dispatch zxing
  dispatch webp
else
  for name in "$@"; do dispatch "$name"; done
fi
