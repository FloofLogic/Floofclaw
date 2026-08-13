#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
expected="$(sed -n 's/^#define FCLAW_VERSION "\([^"]*\)"/\1/p' "$root/runtime/version.h")"
actual="$($root/bin/fclaw --version -a)"

printf '%s\n' "$expected" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || {
  echo "version-contract: header version is not plain X.Y.Z: $expected" >&2
  exit 1
}
printf '%s\n' "$actual" | grep -Fq "\"version\":\"$expected\"" || {
  echo "version-contract: binary output does not match header: $actual" >&2
  exit 1
}
if [ -n "${FCLAW_RELEASE_TAG:-}" ] && [ "$FCLAW_RELEASE_TAG" != "v$expected" ]; then
  echo "version-contract: tag $FCLAW_RELEASE_TAG does not match v$expected" >&2
  exit 1
fi

echo "version-contract: PASS v$expected"
