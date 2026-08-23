#!/usr/bin/env bash
# Typed-event ingress through the real binary. The C integration tests own
# the runtime contract; this owns the operator-facing command surface: stdin
# payloads, exit codes, the reserve/republish handshake, and the promise
# that a refused publication leaves nothing behind.
set -euo pipefail

fail() { printf 'bus typed ingress: %s\n' "$1" >&2; exit 1; }

inbox_count() {
  local n=0
  shopt -s nullglob
  local f
  for f in workspace/bus/inbox/bus_*.json; do n=$((n + 1)); done
  shopt -u nullglob
  printf '%s' "$n"
}

# Run the CLI, capture stdout and the exact exit code.
run_cli() {
  CLI_OUT=""
  CLI_RC=0
  CLI_OUT="$("$@" 2>/dev/null)" || CLI_RC=$?
}

expect_rc() {
  local want="$1" label="$2"
  [ "$CLI_RC" = "$want" ] ||
    fail "$label: expected exit $want, got $CLI_RC (output: $CLI_OUT)"
}

expect_json_line() {
  local label="$1"
  case "$CLI_OUT" in
    \{*\}) ;;
    *) fail "$label emitted a non-JSON agent line: $CLI_OUT" ;;
  esac
  case "$CLI_OUT" in
    *$'\033['*) fail "$label leaked color into agent output" ;;
  esac
}

mkdir -p workspace

# --- stdin is the required payload source ---------------------------------
printf '%s' '{"scope":"installed_applications","nested":{"list":[1,true,null]}}' \
  > workspace/payload.json
run_cli ./bin/fclaw bus publish -a \
  --type device_scan_requested \
  --channel local_product \
  --adapter-id local_product_android \
  --context-id device \
  --ref '{"session_id":"session_123"}' \
  --payload-stdin < workspace/payload.json
expect_rc 0 "stdin publication"
expect_json_line "stdin publication"
[[ "$CLI_OUT" == *'"event_id":"bus_000001"'* ]] || fail "stdin publication omitted event_id"
[[ "$CLI_OUT" == *'"type":"device_scan_requested"'* ]] || fail "stdin publication omitted type"
[[ "$CLI_OUT" == *'"channel":"local_product"'* ]] || fail "stdin publication omitted channel"
[[ "$CLI_OUT" == *'"context_id":"device"'* ]] || fail "stdin publication omitted context_id"

envelope="$(cat workspace/bus/inbox/bus_000001.json)"
[[ "$envelope" == *'"routing":{"adapter_id":"local_product_android","context_id":"device","ref":{"session_id":"session_123"}}'* ]] ||
  fail "routing did not land in its own envelope block: $envelope"
[[ "$envelope" == *'"payload":{"scope":"installed_applications","nested":{"list":[1,true,null]}}'* ]] ||
  fail "payload was not preserved verbatim: $envelope"

# --- human mode is for people, agent mode is for parsers ------------------
run_cli ./bin/fclaw bus publish -h --type device_scan_requested \
  --channel local_product --payload '{"scope":"human"}'
expect_rc 0 "human publication"
[[ "$CLI_OUT" == *$'\033['* ]] || fail "human publication omitted color"

# --- reserve, then publish that identity twice ----------------------------
run_cli ./bin/fclaw bus reserve -a
expect_rc 0 "reserve"
expect_json_line "reserve"
reserved="$(printf '%s' "$CLI_OUT" | sed -n 's/.*"event_id":"\([^"]*\)".*/\1/p')"
[ -n "$reserved" ] || fail "reserve omitted event_id"
before="$(inbox_count)"

run_cli ./bin/fclaw bus publish -a --type build_observation_changed \
  --channel local_product --context-id builds --event-id "$reserved" \
  --payload '{"status":"green"}'
expect_rc 0 "stable publication"
run_cli ./bin/fclaw bus publish -a --type build_observation_changed \
  --channel local_product --context-id builds --event-id "$reserved" \
  --payload '{"status":"green"}'
expect_rc 0 "identical republication"
[ "$(inbox_count)" = "$((before + 1))" ] ||
  fail "identical republication created a second envelope"

run_cli ./bin/fclaw bus publish -a --type build_observation_changed \
  --channel local_product --context-id builds --event-id "$reserved" \
  --payload '{"status":"red"}'
expect_rc 3 "conflicting payload"
expect_json_line "conflicting payload"
run_cli ./bin/fclaw bus publish -a --type build_observation_changed \
  --channel local_product --context-id elsewhere --event-id "$reserved" \
  --payload '{"status":"green"}'
expect_rc 3 "conflicting routing"
[ "$(inbox_count)" = "$((before + 1))" ] ||
  fail "a conflict disturbed the committed envelope"

# --- every refusal is a validation exit that publishes nothing ------------
settled="$(inbox_count)"
reject() {
  local label="$1"; shift
  run_cli "$@"
  expect_rc 1 "$label"
  expect_json_line "$label"
  [ "$(inbox_count)" = "$settled" ] || fail "$label left a partial envelope"
}

reject "reserved type"    ./bin/fclaw bus publish -a --type work_started --payload '{}'
reject "reserved prefix"  ./bin/fclaw bus publish -a --type user_note --payload '{}'
reject "bad token"        ./bin/fclaw bus publish -a --type '9nines' --payload '{}'
reject "spaced token"     ./bin/fclaw bus publish -a --type 'has space' --payload '{}'
reject "array payload"    ./bin/fclaw bus publish -a --type probe_kind --payload '[1,2]'
reject "malformed json"   ./bin/fclaw bus publish -a --type probe_kind --payload '{"a":'
reject "no payload"       ./bin/fclaw bus publish -a --type probe_kind
reject "runtime context"  ./bin/fclaw bus publish -a --type probe_kind \
                            --context-id 'action:cli:x' --payload '{}'
reject "unreserved id"    ./bin/fclaw bus publish -a --type probe_kind \
                            --event-id 'bus_abc' --payload '{}'
reject "mixed modes"      ./bin/fclaw bus publish -a --text hi --type probe_kind

# Two payload sources, and an oversized stdin payload, both refused.
run_cli ./bin/fclaw bus publish -a --type probe_kind --payload '{}' --payload-stdin \
  < /dev/null
expect_rc 1 "two payload sources"
head -c 20000 /dev/zero | tr '\0' 'x' > workspace/oversized.txt
{ printf '{"a":"'; cat workspace/oversized.txt; printf '"}'; } > workspace/oversized.json
run_cli ./bin/fclaw bus publish -a --type probe_kind --payload-stdin \
  < workspace/oversized.json
expect_rc 1 "oversized stdin payload"
[ "$(inbox_count)" = "$settled" ] || fail "a refused payload left an envelope"

# --- the legacy text shortcut is untouched --------------------------------
run_cli ./bin/fclaw bus publish -a --channel cli --text "hello"
expect_rc 0 "legacy text publication"
legacy_id="$(printf '%s' "$CLI_OUT" | sed -n 's/.*"event_id":"\([^"]*\)".*/\1/p')"
[ "$CLI_OUT" = "{\"event_id\":\"$legacy_id\",\"channel\":\"cli\"}" ] ||
  fail "legacy text output changed: $CLI_OUT"
legacy="$(cat "workspace/bus/inbox/$legacy_id.json")"
[[ "$legacy" == *'"type":"user_message"'* ]] || fail "legacy text stopped publishing user_message"
[[ "$legacy" == *'"payload":{"text":"hello"}'* ]] || fail "legacy text payload shape changed"
[[ "$legacy" != *'"routing"'* ]] || fail "legacy text envelope grew a routing block"

# --- the committed envelope is inspectable by type ------------------------
run_cli ./bin/fclaw bus log -a --type device_scan_requested
expect_rc 0 "bus log by type"
[[ "$CLI_OUT" == *'"event_id":"bus_000001"'* ]] || fail "bus log --type lost the custom envelope"
[[ "$CLI_OUT" != *'"type":"user_message"'* ]] || fail "bus log --type leaked other kinds"

printf 'bus typed ingress test passed\n'
