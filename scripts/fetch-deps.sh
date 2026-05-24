#!/usr/bin/env bash
# Fetch upstream C/C++ deps into vendor/zig-pkg/<hash>/，让 build.sh 不依赖 sibling
# aglet checkout 也能跑（CI / fresh clone）。
#
# Hash 跟 aglet/build.zig.zon 对齐；URL 是上游 release tarball。
# 跑完后 build.sh 第三优先级路径 `./vendor/zig-pkg/<hash>` 拿到内容。
#
# 用法：
#   ./scripts/fetch-deps.sh           # 全拉
#   ./scripts/fetch-deps.sh zxing     # 只拉 zxing
#   ./scripts/fetch-deps.sh webp      # 只拉 libwebp

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR="$ROOT/vendor/zig-pkg"
mkdir -p "$VENDOR"

# (name, hash, url)。hash 跟 aglet/build.zig.zon dependencies.* 完全一致。
declare -A DEPS=(
  [zxing]="N-V-__8AALGrMwDZ4Ry7UDuLwgq1EZDZbRbLN75PkGihtghn|https://github.com/zxing-cpp/zxing-cpp/archive/refs/tags/v3.0.2.tar.gz"
  [webp]="N-V-__8AAGkrewARcKiSyLfJKsaW0ZoqHh4hzi4mTGwekxjk|https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz"
)

fetch_one() {
  local name="$1"
  local entry="${DEPS[$name]}"
  local hash="${entry%%|*}"
  local url="${entry##*|}"
  local target="$VENDOR/$hash"

  if [[ -d "$target" ]] && [[ -n "$(ls -A "$target" 2>/dev/null)" ]]; then
    echo "[$name] already vendored at $target"
    return 0
  fi

  echo "[$name] fetching $url → $target"
  mkdir -p "$target"
  curl -fsSL "$url" | tar -xz --strip-components=1 -C "$target"
  echo "[$name] ✓ ($(find "$target" -type f | wc -l | tr -d ' ') files)"
}

if [[ $# -eq 0 ]]; then
  for name in "${!DEPS[@]}"; do
    fetch_one "$name"
  done
else
  for name in "$@"; do
    if [[ -z "${DEPS[$name]:-}" ]]; then
      echo "unknown dep: $name (known: ${!DEPS[*]})" >&2
      exit 1
    fi
    fetch_one "$name"
  done
fi
