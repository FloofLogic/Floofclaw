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

# The runtime injects the completion capability for managed actions. This
# focused test fakes it, with a no-op fclaw so `internal detach` succeeds
# without spawning a real waiter; the waiter's completion and timeout
# behavior is covered by tests/test_manage_hermes_completion.sh.
export FCLAW_OPERATION_ID="op_actionreq_contract_001"
export FCLAW_COMPLETION_TOKEN="$(printf 'f%.0s' $(seq 64))"
printf '#!/usr/bin/env bash\nexit 0\n' >"$tmp/fake_fclaw.sh"
chmod +x "$tmp/fake_fclaw.sh"
export FCLAW_BIN="$tmp/fake_fclaw.sh"

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

# The polling op is gone from the contract: completion arrives through the
# detached waiter's `fclaw operation complete` claim. op:poll must be
# rejected loudly, not silently emulated.
set +e
reject_out="$(printf '{"request_id":"actionreq_contract_poll","action":"manage_hermes","args":{"op":"poll","op_id":"%s"},"run_id":"run_contract"}\n' "$handle" |
  "$here/run.sh")"
reject_rc=$?
set -e
[ "$reject_rc" -ne 0 ] || fail "op:poll should exit nonzero"
assert_jq "$reject_out" '.error.code == "BAD_ARGS"' "op:poll rejection should be BAD_ARGS"

printf 'PASS manage_hermes contract: start detaches the waiter; op:poll rejected\n'
