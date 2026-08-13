#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FCLAW_BIN="${FCLAW_THRASH_BIN:-$ROOT/bin/fclaw}"
DRIVER_BIN="${FCLAW_THRASH_DRIVER_BIN:-$ROOT/bin/fclaw_thrash_driver}"
THRASH_KEEP="${THRASH_KEEP:-0}"
CURRENT_SCRATCH=""

cleanup() {
  local exit_code=$?
  cd "$ROOT"
  if [[ -n "$CURRENT_SCRATCH" && -d "$CURRENT_SCRATCH" ]]; then
    if [[ "$THRASH_KEEP" == "1" && "$exit_code" != "0" ]]; then
      printf 'thrash: retained failure workspace %s\n' "$CURRENT_SCRATCH" >&2
    else
      rm -rf -- "$CURRENT_SCRATCH"
    fi
  fi
}
trap cleanup EXIT INT TERM

new_scratch() {
  local label="$1"
  cd "$ROOT"
  CURRENT_SCRATCH="$(mktemp -d "$ROOT/.thrash-${label}.XXXXXX")"
  ln -s "$ROOT/floops" "$CURRENT_SCRATCH/floops"
  ln -s "$ROOT/actions" "$CURRENT_SCRATCH/actions"
  mkdir -p "$CURRENT_SCRATCH/config"
  ln -s "$ROOT/config/llm_registry.json" \
    "$CURRENT_SCRATCH/config/llm_registry.json"
  ln -s "$ROOT/config/model_profiles.json" \
    "$CURRENT_SCRATCH/config/model_profiles.json"
  cp "$ROOT/tests/thrash/floofclaw_config.json" \
    "$CURRENT_SCRATCH/config/floofclaw_config.json"
  mkdir -p "$CURRENT_SCRATCH/workspace/logs"
  cd "$CURRENT_SCRATCH"
}

remove_scratch() {
  local doomed="$CURRENT_SCRATCH"
  cd "$ROOT"
  [[ -n "$doomed" && -d "$doomed" ]] || return 1
  rm -rf -- "$doomed"
  CURRENT_SCRATCH=""
}

cd "$ROOT"
./tests/run_in_isolated_copy.sh \
  ./bin/fclaw_integration_tests capacity

new_scratch active-cap
"$DRIVER_BIN" active-cap "$FCLAW_BIN"
remove_scratch

new_scratch load
"$DRIVER_BIN" load "$FCLAW_BIN"

# Keep the loaded scratch workspace in place until the post-thrash suite is
# green. The isolated test runner excludes it, so 30k+ run artifacts are not
# recursively copied into each test workspace.
cd "$ROOT"
make test

printf 'thrash: cap boundaries, load, same-worktree post-test clean\n'
