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
build_profile="unknown"
compiled_adapters="unknown"
if [ -f "$root/bin/fclaw.build" ]; then
  build_profile="$(sed -n 's/^profile=//p' "$root/bin/fclaw.build")"
  compiled_adapters="$(sed -n 's/^adapters=//p' "$root/bin/fclaw.build")"
fi
scratch="$(mktemp "${TMPDIR:-/tmp}/fclaw-stripped.XXXXXX")"
trap 'rm -f "$scratch"' EXIT
if [ "$build_profile" = "release-small" ]; then
  # release-small already strips the distributed artifact; report that exact
  # file rather than asking platform strip to rewrite a copied Mach-O twice.
  binary_bytes="$(wc -c < "$root/bin/fclaw" | tr -d ' ')"
else
  cp "$root/bin/fclaw" "$scratch"
  strip "$scratch"
  binary_bytes="$(wc -c < "$scratch" | tr -d ' ')"
fi
release_small_max_bytes="${FCLAW_RELEASE_SMALL_MAX_BYTES:-921600}"
size_gate="not-applicable"
if [ "$build_profile" = "release-small" ]; then
  size_gate="PASS"
  [ "$binary_bytes" -le "$release_small_max_bytes" ] || size_gate="FAIL"
fi
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
printf 'build_profile=%s\n' "$build_profile"
printf 'compiled_adapters=%s\n' "$compiled_adapters"
printf 'stripped_binary_bytes=%s\n' "$binary_bytes"
printf 'release_small_size_gate=%s (%s-byte ceiling)\n' "$size_gate" "$release_small_max_bytes"
printf 'runtime_c_header_lines=%s\n' "$runtime_lines"
printf 'unit_tests=%s\n' "$unit_tests"
printf 'integration_tests=%s\n' "$integration_tests"
printf 'required_core_libraries=2 (OpenSSL and libcurl; make MOCK_ONLY=1 omits both)\n'
printf '%s\n' "$runtime_metrics"
