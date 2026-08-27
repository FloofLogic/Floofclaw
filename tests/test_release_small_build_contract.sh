#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$root"

make release-small

bytes="$(wc -c < bin/fclaw | tr -d ' ')"
[ "$bytes" -le 921600 ] || {
  echo "release-small contract: $bytes bytes exceeds 921600" >&2
  exit 1
}

channels="$(./bin/fclaw adapters -a)"
for adapter in discord irc telegram ws; do
  printf '%s\n' "$channels" | grep -q "\"id\":\"$adapter\"" || {
    echo "release-small contract: missing adapter $adapter" >&2
    exit 1
  }
done

grep -q '^profile=release-small$' bin/fclaw.build
grep -q '^adapters=discord irc telegram ws$' bin/fclaw.build
printf 'release-small build contract: PASS bytes=%s adapters=all\n' "$bytes"
