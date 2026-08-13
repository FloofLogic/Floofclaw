#!/usr/bin/env zsh
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <url>" >&2
  exit 1
fi

url="$1"

echo "pluck"
time zig build run -Doptimize=ReleaseFast -- "$url" >/dev/null

if [[ -f scripts/readability_baseline.mjs ]]; then
  echo
  echo "node readability baseline"
  time node scripts/readability_baseline.mjs "$url" >/dev/null
fi
