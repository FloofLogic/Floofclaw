#!/usr/bin/env bash
# Reproducible release metrics. Run after `make build`.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
[ -x "$root/bin/fclaw" ] || {
  echo "metrics: build bin/fclaw first" >&2
  exit 1
}

runtime_lines="$(find "$root/runtime" -type f \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 wc -l | awk 'END {print $1}')"
unit_tests="$(grep -c '^TEST_ENTRY' "$root/tests/test_registry_unit.inc")"
integration_tests="$(grep -c '^TEST_ENTRY' "$root/tests/test_registry_integration.inc")"
scratch="$(mktemp "${TMPDIR:-/tmp}/fclaw-stripped.XXXXXX")"
trap 'rm -f "$scratch"' EXIT
cp "$root/bin/fclaw" "$scratch"
strip "$scratch"
binary_bytes="$(wc -c < "$scratch" | tr -d ' ')"
version="$(sed -n 's/^#define FCLAW_VERSION "\([^"]*\)"/\1/p' "$root/runtime/version.h")"
revision="$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
compiler="$(cc --version 2>/dev/null | sed -n '1p' || true)"
[ -n "$compiler" ] || compiler="$(cc -v 2>&1 | tail -1)"
runtime_metrics="$("$root/tests/run_in_isolated_copy.sh" bash ./scripts/measure_runtime.sh)"

printf 'version=%s\n' "$version"
printf 'revision=%s\n' "$revision"
printf 'measured_at_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
printf 'platform=%s %s\n' "$(uname -s)" "$(uname -m)"
printf 'compiler=%s\n' "$compiler"
printf 'stripped_binary_bytes=%s\n' "$binary_bytes"
printf 'runtime_c_header_lines=%s\n' "$runtime_lines"
printf 'unit_tests=%s\n' "$unit_tests"
printf 'integration_tests=%s\n' "$integration_tests"
printf 'required_core_libraries=2 (OpenSSL and libcurl; mock-only build can omit both)\n'
printf '%s\n' "$runtime_metrics"
