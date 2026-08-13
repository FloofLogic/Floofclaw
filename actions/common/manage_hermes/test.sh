#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/manage-hermes-test.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }
assert_jq() { jq -e "$2" >/dev/null <<<"$1" || fail "$3"; }

# Exported shell function: the action still invokes its real curl boundary,
# while this focused contract test supplies deterministic Hermes replies.
curl() {
  local arg url=""
  for arg in "$@"; do
    case "$arg" in http://*|https://*) url="$arg" ;; esac
  done
  if [[ "$url" == */v1/runs ]]; then
    cat >"$FAKE_HERMES_REQUEST"
    printf '%s\n%s' '{"run_id":"run_contract_fixture","status":"started"}' '202'
    return
  fi
  case "$FAKE_HERMES_REPLY" in
    running) printf '%s\n%s' '{"run_id":"run_contract_fixture","status":"running"}' '200' ;;
    completed) printf '%s\n%s' '{"run_id":"run_contract_fixture","status":"completed","output":"fixture complete","usage":{"input_tokens":4,"output_tokens":2,"total_tokens":6}}' '200' ;;
    failed) printf '%s\n%s' '{"run_id":"run_contract_fixture","status":"failed","error":"fixture boom"}' '200' ;;
    lost) printf '%s\n%s' '{"error":{"message":"Run not found: run_contract_fixture","code":"run_not_found"}}' '404' ;;
    *) return 7 ;;
  esac
}
export -f curl

export FCLAW_WORKSPACE_ROOT="$tmp/workspace"
export HERMES_API_URL="http://127.0.0.1:8642"
export API_SERVER_KEY="fixture-caller-key-not-a-real-secret"
export FAKE_HERMES_REQUEST="$tmp/start-request.json"
export FAKE_HERMES_REPLY="running"
mkdir -p "$FCLAW_WORKSPACE_ROOT"

start_out="$(printf '%s\n' \
  '{"request_id":"actionreq_contract_001","action":"manage_hermes","args":{"op":"start","task":"Return exactly: fixture complete"},"run_id":"run_contract"}' |
  "$here/run.sh")"
assert_jq "$start_out" '.error == null and .result.status == "running"' "start did not return running"
handle="$(jq -r '.result.handle' <<<"$start_out")"
[ -n "$handle" ] || fail "start returned no handle"
[ "$(jq -r '.session_id' "$FAKE_HERMES_REQUEST")" = "floofclaw_$handle" ] ||
  fail "session_id was not derived from the operation handle"
record="$FCLAW_WORKSPACE_ROOT/logs/hermes_ops/$handle.json"
[ "$(jq -r '.run_id' "$record")" = "run_contract_fixture" ] ||
  fail "Hermes run_id was not persisted in the handle record"
[[ "$start_out" != *"$API_SERVER_KEY"* ]] || fail "caller credential leaked into output"

poll() {
  printf '{"request_id":"actionreq_contract_poll","action":"manage_hermes","args":{"op":"poll","op_id":"%s"},"run_id":"run_contract"}\n' "$handle" |
    "$here/run.sh"
}

running_out="$(poll)"
assert_jq "$running_out" '.result.status == "running" and .result.hermes_status == "running"' "running poll mapping failed"

FAKE_HERMES_REPLY="completed"; export FAKE_HERMES_REPLY
completed_out="$(poll)"
assert_jq "$completed_out" '.result.status == "finished" and .result.state == "succeeded" and .result.text == "fixture complete" and .result.usage.total_tokens == 6' "completed poll mapping failed"

FAKE_HERMES_REPLY="failed"; export FAKE_HERMES_REPLY
failed_out="$(poll)"
assert_jq "$failed_out" '.result.status == "finished" and .result.state == "failed" and .result.reason == "hermes_run_failed" and (.result.text | contains("fixture boom"))' "failed poll mapping failed"

FAKE_HERMES_REPLY="lost"; export FAKE_HERMES_REPLY
lost_out="$(poll)"
assert_jq "$lost_out" '.result.status == "finished" and .result.state == "failed" and .result.reason == "hermes_run_lost"' "restart/404 mapping failed"

printf 'PASS manage_hermes contract: start, running, completed, failed, restart/404\n'
