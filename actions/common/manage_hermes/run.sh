#!/usr/bin/env bash
# manage_hermes — run a Hermes Agent job as a managed external operation.
#
# op:start POSTs the job to the Hermes server, then detaches a local
# completion waiter (via `fclaw internal detach`) and returns
# running(worker_handle). The waiter polls the Hermes HTTP API at its own
# cadence — leaf-process polling, invisible to the FloofClaw control
# plane — and submits exactly one completion claim through
# `fclaw operation complete` when the run reaches a terminal state. The
# runtime's operation deadline bounds a waiter that dies or a server
# that never answers.
#
# The completion capability (FCLAW_OPERATION_ID / FCLAW_COMPLETION_TOKEN)
# and the callback binary (FCLAW_BIN) arrive through the trusted child
# environment; they are never read from action args.

# ---- detached waiter entry (argv, no stdin envelope) ----------------
if [ "${1:-}" = "__waiter" ]; then
  record="${2:-}"
  [ -n "$record" ] && [ -r "$record" ] || exit 1
  log="$record.waiter.log"
  exec </dev/null >>"$log" 2>&1
  set -u
  api_key="${API_SERVER_KEY:-}"
  op_id="${FCLAW_OPERATION_ID:-}"
  op_token="${FCLAW_COMPLETION_TOKEN:-}"
  fclaw_bin="${FCLAW_BIN:-fclaw}"
  run_id="$(jq -r '.run_id // ""' "$record")"
  api_url="$(jq -r '.api_url // ""' "$record")"
  if [ -z "$api_key" ] || [ -z "$op_id" ] || [ -z "$op_token" ] ||
     [ -z "$run_id" ] || [ -z "$api_url" ]; then
    echo "waiter: missing capability or record fields; the operation" \
         "deadline will terminalize this run"
    exit 1
  fi
  status="" ; text="" ; interval=2
  # The runtime deadline is the authority on giving up; this local cap
  # only stops an orphaned waiter from spinning forever after timeout.
  for _ in $(seq 1 4000); do
    raw="$(curl --silent --show-error --max-time 20 \
             --header "Authorization: Bearer $api_key" \
             --write-out $'\n%{http_code}' \
             "$api_url/v1/runs/$run_id" 2>>"$log")" || raw=""
    code="${raw##*$'\n'}"; body="${raw%$'\n'*}"
    if [ "$code" = "404" ] &&
       [ "$(jq -r '.error.code // ""' <<<"$body" 2>/dev/null)" = "run_not_found" ]; then
      status="failed"
      text="Hermes operation failed (hermes_run_lost): the gateway no longer has this run; it may have restarted."
      break
    fi
    if [ "$code" = "200" ]; then
      hs="$(jq -r '.status // ""' <<<"$body" 2>/dev/null)" || hs=""
      case "$hs" in
        queued|running|waiting_for_approval|stopping) : ;;
        completed)
          status="succeeded"
          text="$(jq -r '(.output // "") | if type == "string" then . else tostring end' <<<"$body" 2>/dev/null)" || text=""
          [ -n "$text" ] || text="Hermes run completed."
          text="${text:0:1600}"
          break ;;
        failed|cancelled)
          detail="$(jq -r '(.error // "no error detail") | if type == "string" then . else tostring end' <<<"$body" 2>/dev/null)" || detail="no error detail"
          status="failed"
          text="Hermes operation failed (hermes_run_${hs}): ${detail:0:1400}"
          break ;;
        *) : ;;  # unknown status: keep waiting; the deadline bounds us
      esac
    fi
    sleep "$interval"
    [ "$interval" -lt 10 ] && interval=$((interval + 1))
  done
  [ -n "$status" ] || exit 1
  result_file="$record.result.json"
  jq -cn --arg status "$status" --arg text "$text" \
    '{status:$status,text:$text}' >"$result_file" || exit 1
  for _ in 1 2 3 4 5; do
    if "$fclaw_bin" operation complete -a \
         --operation-id "$op_id" --token "$op_token" \
         --result-file "$result_file"; then
      exit 0
    fi
    sleep 2
  done
  echo "waiter: completion claim publication failed after retries"
  exit 1
fi

# ---- ordinary action invocation (stdin envelope) --------------------
set -euo pipefail

input="$(cat)"

emit_error() {
  jq -cn --arg code "$1" --arg message "$2" \
    '{events:[],result:{},error:{code:$code,message:$message}}'
  exit 1
}

emit_result() {
  jq -cn --argjson result "$1" '{events:[],result:$result,error:null}'
}

command -v jq >/dev/null 2>&1 || {
  printf '%s\n' '{"events":[],"result":{},"error":{"code":"DEPENDENCY_MISSING","message":"manage_hermes requires jq"}}'
  exit 1
}
command -v curl >/dev/null 2>&1 || emit_error "DEPENDENCY_MISSING" "manage_hermes requires curl"
jq -e 'type == "object" and (.args | type == "object")' >/dev/null 2>&1 <<<"$input" ||
  emit_error "BAD_ARGS" "expected a FloofClaw action input envelope"

op="$(jq -r '.args.op // ""' <<<"$input")"
workspace_root="${FCLAW_WORKSPACE_ROOT:-}"
api_url="${HERMES_API_URL:-http://127.0.0.1:8642}"
api_key="${API_SERVER_KEY:-}"
[ -n "$workspace_root" ] || emit_error "CONFIG_ERROR" "FCLAW_WORKSPACE_ROOT is required"
[ -n "$api_key" ] || emit_error "CONFIG_ERROR" "API_SERVER_KEY is required"
case "$api_url" in http://*|https://*) ;; *) emit_error "CONFIG_ERROR" "HERMES_API_URL must be HTTP or HTTPS" ;; esac
api_url="${api_url%/}"
state_dir="$workspace_root/logs/hermes_ops"
umask 077
mkdir -p "$state_dir" || emit_error "STATE_ERROR" "could not create Hermes handle directory"

curl_json() {
  curl --silent --show-error --max-time 20 --write-out $'\n%{http_code}' \
    --header "Authorization: Bearer $api_key" --header "Content-Type: application/json" "$@"
}

[ "$op" = "start" ] || emit_error "BAD_ARGS" "args.op must be start"
task="$(jq -r '.args.task // ""' <<<"$input")"
request_id="$(jq -r '.request_id // ""' <<<"$input")"
[ -n "$task" ] || emit_error "BAD_ARGS" "args.task is required with op=start"
safe_id="$(printf '%s' "$request_id" | tr -cd '[:alnum:]_-')"
[ -n "$safe_id" ] || emit_error "BAD_ARGS" "request_id is required"
[ -n "${FCLAW_OPERATION_ID:-}" ] && [ -n "${FCLAW_COMPLETION_TOKEN:-}" ] ||
  emit_error "CONFIG_ERROR" "manage_hermes requires the managed-operation completion capability (FCLAW_OPERATION_ID/FCLAW_COMPLETION_TOKEN)"
[ -n "${FCLAW_BIN:-}" ] || emit_error "CONFIG_ERROR" "manage_hermes requires FCLAW_BIN for the completion callback"
handle="hermes_${safe_id:0:80}"
session_id="floofclaw_${handle}"
# HERMES_MODEL (optional, from the gateway environment) selects a
# Hermes model_routes alias for dispatched work — e.g. the demo
# harness sets metis-live in live mode so real questions get real
# answers. Unset (scenario/fixture mode) omits the field entirely
# and Hermes uses its configured default.
payload="$(jq -cn --arg input "$task" --arg session_id "$session_id" \
  --arg model "${HERMES_MODEL:-}" \
  '{input:$input,session_id:$session_id}
   + (if $model == "" then {} else {model:$model} end)')"
raw="$(printf '%s' "$payload" | curl_json --request POST --data-binary @- "$api_url/v1/runs")" ||
  emit_error "HERMES_UNREACHABLE" "POST /v1/runs failed"
code="${raw##*$'\n'}"; body="${raw%$'\n'*}"
[ "$code" = "202" ] || emit_error "HERMES_START_FAILED" "POST /v1/runs returned HTTP $code"
run_id="$(jq -er '.run_id | strings | select(length > 0)' <<<"$body")" ||
  emit_error "HERMES_PROTOCOL_ERROR" "Hermes start response has no run_id"
case "$run_id" in *[!A-Za-z0-9_-]*) emit_error "HERMES_PROTOCOL_ERROR" "Hermes returned an unsafe run_id" ;; esac
record="$state_dir/$handle.json"; tmp="$record.tmp.$$"
jq -cn --arg handle "$handle" --arg session_id "$session_id" --arg run_id "$run_id" \
  --arg api_url "$api_url" \
  '{handle:$handle,session_id:$session_id,run_id:$run_id,api_url:$api_url}' >"$tmp" && mv "$tmp" "$record" ||
  emit_error "STATE_ERROR" "could not persist Hermes run handle"
# Detach the completion waiter: its own session and process group, so it
# survives this action's timeout kill, gateway shutdown, and Ctrl-C. Its
# stdio must never touch this action's piped stdout/stderr.
"$FCLAW_BIN" internal detach -- bash "$0" __waiter "$record" >/dev/null 2>&1 ||
  emit_error "WAITER_SPAWN_FAILED" "could not detach the Hermes completion waiter"
emit_result "$(jq -cn --arg handle "$handle" --arg run_id "$run_id" --arg session_id "$session_id" \
  '{status:"running",handle:$handle,state:"running",hermes_status:"started",run_id:$run_id,session_id:$session_id,text:""}')"
exit 0
