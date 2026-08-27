#!/usr/bin/env bash
# Run the existing integration binary in a small, fixed set of isolated
# shards. Each worker goes through run_in_isolated_copy.sh, so tests that
# rewrite fixtures never share a workspace or touch the checkout.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"

check_sanitizer_log() {
  local log="$1"
  local label="${2:-integration log}"
  local pattern='runtime error:|AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer'
  if LC_ALL=C grep -Eq "$pattern" "$log"; then
    printf '%s emitted a sanitizer diagnostic\n' "$label" >&2
    LC_ALL=C grep -En "$pattern" "$log" >&2 || true
    return 1
  fi
}

if [[ "${1:-}" == "--check-sanitizer-log" ]]; then
  [[ "$#" -eq 2 ]] || {
    printf 'usage: %s --check-sanitizer-log <path>\n' "$0" >&2
    exit 2
  }
  check_sanitizer_log "$2"
  exit 0
fi
[[ "$#" -eq 0 ]] || {
  printf 'usage: %s [--check-sanitizer-log <path>]\n' "$0" >&2
  exit 2
}

SHARDS="${FCLAW_INTEGRATION_SHARDS:-4}"
case "$SHARDS" in
  ''|*[!0-9]*|0) printf 'integration shards must be a positive integer\n' >&2; exit 2 ;;
esac

log_dir="$(mktemp -d "${TMPDIR:-/tmp}/fclaw-integration.XXXXXX")"
pids=()
cleanup() {
  local status=$?
  trap - EXIT HUP INT TERM
  for pid in "${pids[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -rf -- "$log_dir"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

for ((shard = 1; shard <= SHARDS; shard++)); do
  (
    cd "$ROOT"
    FCLAW_TEST_SHARD="$shard/$SHARDS" \
      ./tests/run_in_isolated_copy.sh ./bin/fclaw_integration_tests
  ) >"$log_dir/$shard.log" 2>&1 &
  pids+=("$!")
done

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then status=1; fi
done
pids=()

ran=0
failed=0
for ((shard = 1; shard <= SHARDS; shard++)); do
  log="$log_dir/$shard.log"
  printf '===== integration shard %d/%d =====\n' "$shard" "$SHARDS"
  cat "$log"
  if ! check_sanitizer_log "$log" "integration shard $shard/$SHARDS"; then
    status=1
  fi
  trailer="$(sed -n 's/^\([0-9][0-9]*\) test(s), \([0-9][0-9]*\) failure(s)$/\1 \2/p' "$log" | tail -n 1)"
  if [ -z "$trailer" ]; then
    printf 'integration shard %d emitted no result trailer\n' "$shard" >&2
    status=1
    continue
  fi
  shard_ran="${trailer%% *}"
  shard_failed="${trailer##* }"
  ran=$((ran + shard_ran))
  failed=$((failed + shard_failed))
done

printf '\n%d test(s) across %d isolated shard(s), %d failure(s)\n' \
  "$ran" "$SHARDS" "$failed"
[ "$ran" -gt 0 ] && [ "$failed" -eq 0 ] && [ "$status" -eq 0 ]
