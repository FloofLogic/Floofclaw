#!/usr/bin/env bash
set -euo pipefail

action_dir="$(cd "$(dirname "$0")" && pwd -P)"
pluck_dir="$action_dir/_pluck"

[ -f "$pluck_dir/build.zig" ] || {
  echo "web_read: pluck source is missing" >&2
  echo "  run: git submodule update --init actions/common/web_read/_pluck" >&2
  exit 1
}
command -v zig >/dev/null 2>&1 || {
  echo "web_read: zig not found on PATH" >&2
  echo "  install Zig 0.15.2 and libxml2, then rerun this command" >&2
  exit 1
}

cd "$pluck_dir"
zig build -Doptimize=ReleaseFast
echo "web_read: built actions/common/web_read/_pluck/zig-out/bin/pluck"
