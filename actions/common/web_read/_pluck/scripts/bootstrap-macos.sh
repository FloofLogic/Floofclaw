#!/usr/bin/env zsh
set -euo pipefail

if command -v zig >/dev/null 2>&1; then
  zig version
  exit 0
fi

if command -v brew >/dev/null 2>&1; then
  brew install zig
  zig version
  exit 0
fi

echo "Install Zig 0.15.2 from https://ziglang.org/download/ and rerun this script." >&2
exit 1
