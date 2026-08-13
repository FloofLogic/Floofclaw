#!/usr/bin/env bash
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

if [ "$op" = "start" ]; then
  task="$(jq -r '.args.task // ""' <<<"$input")"
  request_id="$(jq -r '.request_id // ""' <<<"$input")"
  [ -n "$task" ] || emit_error "BAD_ARGS" "args.task is required with op=start"
  safe_id="$(printf '%s' "$request_id" | tr -cd '[:alnum:]_-')"
  [ -n "$safe_id" ] || emit_error "BAD_ARGS" "request_id is required"
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
  emit_result "$(jq -cn --arg handle "$handle" --arg run_id "$run_id" --arg session_id "$session_id" \
    '{status:"running",handle:$handle,state:"running",hermes_status:"started",run_id:$run_id,session_id:$session_id,text:""}')"
  exit 0
fi

[ "$op" = "poll" ] || emit_error "BAD_ARGS" "args.op must be start or poll"
handle="$(jq -r '.args.op_id // ""' <<<"$input")"
[ -n "$handle" ] || emit_error "BAD_ARGS" "args.op_id is required with op=poll"
case "$handle" in *[!A-Za-z0-9_-]*) emit_error "BAD_ARGS" "args.op_id is not a valid Hermes handle" ;; esac
record="$state_dir/$handle.json"
if [ ! -r "$record" ]; then
  emit_result "$(jq -cn --arg handle "$handle" \
    '{status:"finished",handle:$handle,state:"failed",reason:"hermes_handle_lost",text:"Hermes operation failed (hermes_handle_lost): the local handle record is missing."}')"
  exit 0
fi
run_id="$(jq -r '.run_id // ""' "$record")"; session_id="$(jq -r '.session_id // ""' "$record")"
recorded_url="$(jq -r '.api_url // ""' "$record")"
if [ -z "$run_id" ] || [ -z "$session_id" ] || [ -z "$recorded_url" ]; then
  emit_result "$(jq -cn --arg handle "$handle" \
    '{status:"finished",handle:$handle,state:"failed",reason:"hermes_handle_invalid",text:"Hermes operation failed (hermes_handle_invalid): the local handle record is invalid."}')"
  exit 0
fi
raw="$(curl_json "$recorded_url/v1/runs/$run_id")" ||
  emit_error "HERMES_UNREACHABLE" "GET /v1/runs/{run_id} failed"
code="${raw##*$'\n'}"; body="${raw%$'\n'*}"
if [ "$code" = "404" ] && [ "$(jq -r '.error.code // ""' <<<"$body")" = "run_not_found" ]; then
  emit_result "$(jq -cn --arg handle "$handle" --arg run_id "$run_id" --arg session_id "$session_id" \
    '{status:"finished",handle:$handle,state:"failed",reason:"hermes_run_lost",hermes_status:"not_found",run_id:$run_id,session_id:$session_id,text:"Hermes operation failed (hermes_run_lost): the gateway no longer has this run; it may have restarted."}')"
  exit 0
fi
[ "$code" = "200" ] || emit_error "HERMES_POLL_FAILED" "GET /v1/runs/{run_id} returned HTTP $code"
hermes_status="$(jq -r '.status // ""' <<<"$body")"
case "$hermes_status" in
  queued|running|waiting_for_approval|stopping)
    emit_result "$(jq -cn --arg handle "$handle" --arg run_id "$run_id" --arg session_id "$session_id" --arg hs "$hermes_status" \
      '{status:"running",handle:$handle,state:"running",hermes_status:$hs,run_id:$run_id,session_id:$session_id,text:""}')" ;;
  completed)
    output="$(jq -r '(.output // "") | if type == "string" then . else tostring end' <<<"$body")"
    [ -n "$output" ] || output="Hermes run completed."
    text="${output:0:1600}"; truncated=false; [ ${#output} -le 1600 ] || truncated=true
    usage="$(jq -c '.usage // null' <<<"$body")"
    emit_result "$(jq -cn --arg handle "$handle" --arg run_id "$run_id" --arg session_id "$session_id" \
      --arg text "$text" --argjson usage "$usage" --argjson truncated "$truncated" \
      '{status:"finished",handle:$handle,state:"succeeded",hermes_status:"completed",run_id:$run_id,session_id:$session_id,text:$text,usage:$usage,truncated:$truncated}')" ;;
  failed|cancelled)
    detail="$(jq -r '(.error // "no error detail") | if type == "string" then . else tostring end' <<<"$body")"
    reason="hermes_run_${hermes_status}"; text="Hermes operation failed ($reason): ${detail:0:1400}"
    emit_result "$(jq -cn --arg handle "$handle" --arg run_id "$run_id" --arg session_id "$session_id" \
      --arg reason "$reason" --arg hs "$hermes_status" --arg text "$text" \
      '{status:"finished",handle:$handle,state:"failed",reason:$reason,hermes_status:$hs,run_id:$run_id,session_id:$session_id,text:$text}')" ;;
  *) emit_error "HERMES_PROTOCOL_ERROR" "Hermes poll response has unknown status" ;;
esac
