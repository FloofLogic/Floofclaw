#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

tmp_parent="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
case "$tmp_parent/" in
  "$ROOT/"*)
    printf 'isolated test: TMPDIR must be outside the repository: %s\n' \
      "$tmp_parent" >&2
    exit 2
    ;;
esac

tmpdir="$(mktemp -d "$tmp_parent/fclaw-test.XXXXXX")"
child_pid=""
cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
    kill "$child_pid" 2>/dev/null || true
    for _ in {1..50}; do
      kill -0 "$child_pid" 2>/dev/null || break
      sleep 0.01
    done
    if kill -0 "$child_pid" 2>/dev/null; then
      kill -KILL "$child_pid" 2>/dev/null || true
    fi
    wait "$child_pid" 2>/dev/null || true
  fi
  cd "$ROOT" || status=1
  rm -rf -- "$tmpdir" || status=1
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# secrets/ is excluded deliberately. The store is resolved from FCLAW_HOME,
# which defaults to the working directory, so copying it would hand the
# operator's real credentials to every action the suite launches — an action
# that reads them would then reach the live outside world from a test run.
# Tests that need a secret build their own store under a scratch FCLAW_HOME.
rsync -a --exclude '.git' --exclude '.fclaw' --exclude 'workspace' \
  --exclude 'secrets' \
  --exclude 'tmp' \
  --exclude '.fclaw-test-isolated' \
  --exclude '.thrash-*' ./ "$tmpdir"/
cd "$tmpdir"
printf '%s\n' "$ROOT" > .fclaw-test-isolated

# Resolvable but deliberately invalid provider credentials. Startup
# preconditions check that a configured provider's key RESOLVES, so a
# deployment config naming a real provider must not require the operator's
# key to be present — a fresh clone has no secrets/ at all. These values
# satisfy resolution and fail immediately if anything ever tries to spend
# them, which is the safe direction. Real keys are excluded above; do not
# substitute them here.
export GEMINI_API_KEY="${GEMINI_API_KEY_TEST_OVERRIDE:-invalid-test-key-do-not-spend}"
export OPENROUTER_API_KEY="${OPENROUTER_API_KEY_TEST_OVERRIDE:-invalid-test-key-do-not-spend}"

export FCLAW_TEST_ISOLATED=1
export FCLAW_TEST_SOURCE_ROOT="$ROOT"
export FCLAW_TEST_ISOLATED_ROOT="$(pwd -P)"
if [[ -n "${FCLAW_TEST_ISOLATION_CHILD_PID_FILE:-}" ]]; then
  if [[ "${FCLAW_TEST_ISOLATION_CHILD_PID_FILE:0:1}" != "/" ]]; then
    printf 'isolated test: child PID file must be absolute\n' >&2
    exit 2
  fi
  "$@" &
  child_pid=$!
  printf '%s\n' "$child_pid" > "$FCLAW_TEST_ISOLATION_CHILD_PID_FILE"
  set +e
  wait "$child_pid"
  child_status=$?
  set -e
  child_pid=""
  exit "$child_status"
fi
"$@"
