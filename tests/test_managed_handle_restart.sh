#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

if [[ "${FCLAW_TEST_ISOLATED:-0}" != "1" || ! -f .fclaw-test-isolated ]]; then
  printf '%s\n' 'managed-handle restart: refusing to run outside an isolated copy' >&2
  exit 2
fi

gateway_running=0
stop_gateway() {
  if [[ "$gateway_running" == "1" ]]; then
    ./bin/fclaw gateway stop -a >/dev/null 2>&1 || true
    gateway_running=0
  fi
}
cleanup() {
  local status=$?
  trap - EXIT
  stop_gateway
  exit "$status"
}
trap cleanup EXIT

export FCLAW_MODEL_PROFILES="$PWD/tests/fixtures/smoke/model_profiles_mock.json"
export FCLAW_GATEWAY_POLL_MS=5
export FCLAW_GATEWAY_DISABLE_CHANNELS=1
export LLM_MOCK_RESPONSE_PATHS="$PWD/tests/fixtures/smoke/managed_handle_restart_start.json:$PWD/tests/fixtures/smoke/managed_handle_restart_done.json"

./bin/fclaw clear -a --yes >/dev/null

run_one_process() {
  local prompt="$1"
  local out
  ./bin/fclaw gateway start -a --floop openclaw >/dev/null
  gateway_running=1
  out="$(./bin/fclaw run --floop openclaw --text "$prompt")"
  printf '%s\n' "$out"
  grep -q 'MANAGED_HANDLE_RESTART_OK' <<<"$out"
  stop_gateway
}

run_one_process 'first managed action before restart'
run_one_process 'second managed action after restart'

if grep -Rqs 'managed_handle_correlation_conflict' workspace/runs workspace/logs; then
  printf '%s\n' 'managed-handle restart: correlation conflict after cold restart' >&2
  exit 1
fi

request_ids="$({
  grep -Rh '"type":"action_request"' workspace/runs/*/event_log.jsonl || true
} | grep '"action":"action_list"' | sed -E 's/.*"request_id":"([^"]+)".*/\1/')"

test "$(printf '%s\n' "$request_ids" | sed '/^$/d' | wc -l | tr -d ' ')" -eq 2
test "$(printf '%s\n' "$request_ids" | sed '/^$/d' | sort -u | wc -l | tr -d ' ')" -eq 2
test -f workspace/memory/state/operations.json

while IFS= read -r request_id; do
  [[ -n "$request_id" ]] || continue
  grep -Fq "\"handle\":\"op_$request_id\"" workspace/memory/state/operations.json
done <<<"$request_ids"

if grep -q '"handle":"intr_' workspace/memory/state/operations.json; then
  printf '%s\n' 'managed-handle restart: found process-local intrinsic handle' >&2
  exit 1
fi

printf '%s\n' 'managed-handle cold restart ok'
