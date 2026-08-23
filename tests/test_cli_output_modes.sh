#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf 'cli output mode test: %s\n' "$1" >&2
  exit 1
}

assert_agent_text() {
  local label="$1" text="$2" line
  [ -n "$text" ] || fail "$label returned no agent output"
  [[ "$text" != *$'\033['* ]] || fail "$label leaked color into agent output"
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    case "$line" in
      \{*\}|\[*\]) ;;
      *) fail "$label returned a non-JSON line: $line" ;;
    esac
  done <<< "$text"
}

assert_agent() {
  local label="$1"
  shift
  local text
  text="$("$@")" || fail "$label failed"
  assert_agent_text "$label" "$text"
  printf '%s' "$text"
}

assert_agent_failure() {
  local label="$1"
  shift
  local text rc=0
  text="$("$@")" || rc=$?
  [ "$rc" -ne 0 ] || fail "$label unexpectedly succeeded"
  assert_agent_text "$label" "$text"
}

assert_human() {
  local label="$1"
  shift
  local text
  text="$("$@" 2>&1)" || fail "$label failed"
  [[ "$text" == *$'\033['* ]] || fail "$label omitted human color"
}

assert_human_failure() {
  local label="$1"
  shift
  local text rc=0
  text="$("$@" 2>&1)" || rc=$?
  [ "$rc" -ne 0 ] || fail "$label unexpectedly succeeded"
  [[ "$text" == *$'\033['* ]] || fail "$label omitted human color"
}

assert_rejected() {
  local label="$1"
  shift
  local rc=0
  "$@" >/dev/null 2>&1 || rc=$?
  [ "$rc" -ne 0 ] || fail "$label was not rejected"
}

mkdir -p workspace

assert_agent "actions default" ./bin/fclaw actions >/dev/null
assert_agent "actions -a" ./bin/fclaw actions -a >/dev/null
assert_agent "adapters default" ./bin/fclaw adapters >/dev/null
assert_agent "adapters -a" ./bin/fclaw adapters -a >/dev/null
assert_agent "floops default" ./bin/fclaw floops >/dev/null
assert_agent "floops -a" ./bin/fclaw floops -a >/dev/null
assert_agent "action list default" ./bin/fclaw action list >/dev/null
assert_agent "action list -a" ./bin/fclaw action list -a >/dev/null
assert_agent "setup default" ./bin/fclaw setup >/dev/null
assert_agent "setup -a" ./bin/fclaw setup -a >/dev/null
assert_agent "version default" ./bin/fclaw --version >/dev/null
assert_agent "version -a" ./bin/fclaw --version -a >/dev/null
assert_agent "empty affair list default" ./bin/fclaw affair list >/dev/null
assert_agent_failure "stopped gateway status default" ./bin/fclaw gateway status
assert_agent_failure "stopped gateway status -a" ./bin/fclaw gateway status -a
assert_agent_failure "operation complete missing capability default" \
  ./bin/fclaw operation complete --status succeeded
assert_agent_failure "operation complete missing capability -a" \
  ./bin/fclaw operation complete -a --status succeeded
assert_agent_failure "operation complete unknown flag -a" \
  ./bin/fclaw operation complete -a --nonsense

run_json="$(assert_agent "run default" ./bin/fclaw run --text hello)"
run_id="$(printf '%s\n' "$run_json" |
  sed -n 's/^{"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$run_id" ] || fail "run default omitted run_id"
assert_agent "run -a" ./bin/fclaw run -a --text hello >/dev/null
assert_agent "view default" ./bin/fclaw view "$run_id" >/dev/null
assert_agent "view -a" ./bin/fclaw view -a "$run_id" >/dev/null
assert_agent "replay default" ./bin/fclaw replay \
  "workspace/runs/$run_id/event_log.jsonl" >/dev/null
assert_agent "replay -a" ./bin/fclaw replay -a \
  "workspace/runs/$run_id/event_log.jsonl" >/dev/null
assert_agent "usage default" ./bin/fclaw usage >/dev/null
assert_agent "usage -a" ./bin/fclaw usage -a >/dev/null

assert_agent "auth set default" ./bin/fclaw auth set openai_key fixture >/dev/null
auth_json="$(assert_agent "auth get default" ./bin/fclaw auth get openai_key)"
[[ "$auth_json" == *'"value":"fixture"'* ]] || fail "auth get omitted value"
assert_agent "auth get -a" ./bin/fclaw auth get -a openai_key >/dev/null
assert_agent "auth header default" ./bin/fclaw auth header openai_key >/dev/null
assert_agent "auth delete default" ./bin/fclaw auth delete openai_key >/dev/null

publish_json="$(assert_agent "bus publish default" ./bin/fclaw bus publish --text mode-test)"
[[ "$publish_json" == *'"event_id":'* ]] || fail "bus publish omitted event_id"
assert_agent "bus log default" ./bin/fclaw bus log -n 1 >/dev/null
assert_agent "bus log -a" ./bin/fclaw bus log -a -n 1 >/dev/null
assert_agent "bus reserve default" ./bin/fclaw bus reserve >/dev/null
assert_agent "bus reserve -a" ./bin/fclaw bus reserve -a >/dev/null
typed_json="$(assert_agent "bus publish --type default" ./bin/fclaw bus publish \
  --type mode_test_kind --channel mode_test --context-id mode \
  --payload '{"probe":true}')"
[[ "$typed_json" == *'"type":"mode_test_kind"'* ]] ||
  fail "typed bus publish omitted type"
assert_agent_failure "bus publish reserved type" \
  ./bin/fclaw bus publish -a --type work_forged --payload '{}'
assert_agent_failure "bus publish non-object payload" \
  ./bin/fclaw bus publish -a --type mode_test_kind --payload '[]'
assert_human "bus publish --type -h" ./bin/fclaw bus publish -h \
  --type mode_test_kind --channel mode_test --payload '{"probe":true}'
assert_human "bus reserve -h" ./bin/fclaw bus reserve -h
assert_human_failure "bus publish reserved type -h" \
  ./bin/fclaw bus publish -h --type work_forged --payload '{}'

affair_json="$(assert_agent "affair create default" ./bin/fclaw affair create \
  --context cli-mode --manage verify-output)"
affair_id="$(printf '%s\n' "$affair_json" |
  sed -n 's/^{"affair_id":"\([^"]*\)".*/\1/p')"
[ -n "$affair_id" ] || fail "affair create omitted affair_id"
assert_agent "affair list default" ./bin/fclaw affair list >/dev/null
assert_agent "affair list -a" ./bin/fclaw affair list -a >/dev/null
assert_agent "affair pause default" ./bin/fclaw affair pause "$affair_id" >/dev/null
assert_agent "affair resume default" ./bin/fclaw affair resume "$affair_id" >/dev/null
assert_agent "affair schedule default" ./bin/fclaw affair schedule \
  "$affair_id" --in 1m >/dev/null
assert_agent "affair review default" ./bin/fclaw affair review "$affair_id" >/dev/null
assert_agent "affair close default" ./bin/fclaw affair close "$affair_id" >/dev/null

assert_agent "action auth default" ./bin/fclaw action auth \
  --name gcal -- client list >/dev/null
assert_agent "action auth -a" ./bin/fclaw action auth -a \
  --name gcal -- client list >/dev/null

./bin/fclaw channel ws -a </dev/null || fail "channel ws rejected -a"
./bin/fclaw channel irc -a </dev/null || fail "channel irc rejected -a"
./bin/fclaw channel ws -h </dev/null || fail "channel ws rejected -h"
./bin/fclaw channel irc -h </dev/null || fail "channel irc rejected -h"

assert_human_failure "top-level -h" ./bin/fclaw -h
assert_human "run -h" ./bin/fclaw run -h --text hello
assert_human "view -h" ./bin/fclaw view -h "$run_id"
assert_human "replay -h" ./bin/fclaw replay -h \
  "workspace/runs/$run_id/event_log.jsonl"
assert_human "usage -h" ./bin/fclaw usage -h
assert_human "actions -h" ./bin/fclaw actions -h
assert_human "adapters -h" ./bin/fclaw adapters -h
assert_human "floops -h" ./bin/fclaw floops -h
assert_human "action list -h" ./bin/fclaw action list -h
assert_human "action auth -h" ./bin/fclaw action auth -h \
  --name gcal -- client list
assert_human "setup -h" ./bin/fclaw setup -h
assert_human "version -h" ./bin/fclaw --version -h
assert_human "affair list -h" ./bin/fclaw affair list -h
assert_human "bus log -h" ./bin/fclaw bus log -h -n 1
assert_human_failure "gateway status -h" ./bin/fclaw gateway status -h

assert_agent_failure "conflicting output modes" ./bin/fclaw setup -a -h
assert_rejected "legacy view -c" ./bin/fclaw view -c "$run_id"
assert_rejected "legacy usage -c" ./bin/fclaw usage -c
assert_rejected "legacy cacheview -c" ./bin/fclaw cacheview -c --agent missing
assert_rejected "legacy gateway --json" ./bin/fclaw gateway status --json

assert_human "clear -h" ./bin/fclaw clear -h --yes
assert_agent "clear -a" ./bin/fclaw clear -a --yes >/dev/null

printf 'cli output mode test passed\n'
