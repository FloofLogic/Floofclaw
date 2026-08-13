#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf 'action cli test: %s\n' "$1" >&2
  if [[ -d workspace/runs ]]; then
    find workspace/runs -maxdepth 2 \
      \( -name event_log.jsonl -o -name runstate.json \) \
      -exec sh -c 'printf "=== %s\n" "$1"; sed -n "1,160p" "$1"' _ {} \; \
      >&2 || true
    find workspace/runs -maxdepth 3 -path '*/action_calls/*' -type f \
      -exec sh -c 'printf "=== %s\n" "$1"; sed -n "1,240p" "$1"' _ {} \; \
      >&2 || true
  fi
  if [[ -f workspace/logs/rejected_events.jsonl ]]; then
    printf '%s\n' '=== rejected events' >&2
    sed -n '1,160p' workspace/logs/rejected_events.jsonl >&2 || true
  fi
  if [[ -f workspace/action-cli-gateway.out ]]; then
    printf '%s\n' '=== gateway output' >&2
    sed -n '1,240p' workspace/action-cli-gateway.out >&2 || true
  fi
  exit 1
}

gateway_pid=""
cleanup() {
  if [ -n "$gateway_pid" ]; then
    kill "$gateway_pid" 2>/dev/null || true
    wait "$gateway_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p workspace
printf 'managed action cli fixture\n' > workspace/action-cli-fixture.txt

./bin/fclaw action list -a > workspace/action-catalog.json
grep -q '"name":"read_file"' workspace/action-catalog.json ||
  fail 'shared catalog omitted read_file'
grep -q '"name":"working_memory_append"' workspace/action-catalog.json ||
  fail 'shared catalog omitted working_memory_append'
grep -q '"description":"Read UTF-8 text content' workspace/action-catalog.json ||
  fail 'shared catalog omitted manifest description'
grep -q '"args":{"type":"object"' workspace/action-catalog.json ||
  fail 'shared catalog omitted manifest schema'
grep -q 'Call this to create, list, search, inspect, update, and delete Google Calendar events' \
  workspace/action-catalog.json ||
  fail 'gcal catalog omitted its capability summary'

export FCLAW_CODEX_BIN=tests/fixtures/smoke/fake_codex.sh
export FCLAW_CODEX_START_SYNC_MS=500
export FCLAW_VERIFY_ACTION_CLI=1
export FCLAW_ACTION_EXEC_TIMEOUT_MS="${FCLAW_ACTION_EXEC_TIMEOUT_MS:-10000}"
export THRASH_CAP_HOLD_SECONDS=60
export PATH="$(pwd -P)/tests/fixtures/gcal:$PATH"
export FCLAW_HOME="$(pwd -P)/workspace/gcal-secret-home"
export FAKE_GOG_ARGV_LOG="$(pwd -P)/workspace/fake-gog-argv.log"

run_action() {
  local output="$1"
  local name="$2"
  local args="$3"
  local action_pid=""
  if [ -z "$gateway_pid" ]; then
    ./bin/fclaw action exec -a --name "$name" --args "$args" > "$output" &
    action_pid=$!
    for _ in {1..100}; do
      compgen -G 'workspace/bus/inbox/*.json' >/dev/null && break
      kill -0 "$action_pid" 2>/dev/null ||
        fail "action exec for $name exited before publication"
      sleep 0.01
    done
    compgen -G 'workspace/bus/inbox/*.json' >/dev/null ||
      fail "action exec for $name did not publish"
    ./bin/fclaw run -a --floop thrash_cap --text "hold runtime owner" \
      > workspace/action-cli-gateway.out 2>&1 &
    gateway_pid=$!
    wait "$action_pid" || fail "action exec for $name failed"
    kill -0 "$gateway_pid" 2>/dev/null || fail 'runtime owner exited after startup'
    return
  fi
  ./bin/fclaw action exec -a --name "$name" --args "$args" > "$output" ||
    fail "action exec for $name failed"
}

run_action_failure() {
  local output="$1" name="$2" args="$3"
  if ./bin/fclaw action exec -a --name "$name" --args "$args" > "$output"; then
    fail "action exec for $name unexpectedly succeeded"
  fi
}

run_action workspace/action-cli-read.json read_file \
  '{"op":"start","path":"action-cli-fixture.txt"}'
grep -q 'managed action cli fixture' workspace/action-cli-read.json ||
  fail 'runtime-intrinsic result did not return to cli'

run_action workspace/action-cli-bash.json bash \
  '{"op":"start","cmd":"printf action-cli-subprocess"}'
grep -q 'action-cli-subprocess' workspace/action-cli-bash.json ||
  fail 'subprocess result did not return to cli'

printf '%s\n' '{"web":{"client_id":"wrong-type","client_secret":"must-not-store"}}' \
  > workspace/gcal-wrong-client.json
if ./bin/fclaw action auth -h --name gcal -- client add invalid \
    --credentials workspace/gcal-wrong-client.json \
    > workspace/gcal-invalid-client.out 2>&1; then
  fail 'gcal accepted a Web OAuth client'
fi
./bin/fclaw action auth -h --name gcal -- client list \
  > workspace/gcal-client-list-before.out
grep -q '^invalid' workspace/gcal-client-list-before.out &&
  fail 'malformed OAuth client left partial credential state'

./bin/fclaw action auth -h --name gcal -- client add primary \
  --credentials tests/fixtures/gcal/desktop-client.json \
  > workspace/gcal-client-add.out || fail 'gcal client onboarding failed'
./bin/fclaw action auth -h --name gcal -- account add personal \
  --email person@example.test --client primary \
  > workspace/gcal-account-add.out || fail 'gcal account onboarding failed'
./bin/fclaw action auth -h --name gcal -- authorize personal \
  > workspace/gcal-authorize.out || fail 'gcal browser authorization bridge failed'
./bin/fclaw action auth -h --name gcal -- account add remote \
  --email remote@example.test --client primary \
  > workspace/gcal-remote-account.out || fail 'gcal remote account onboarding failed'
./bin/fclaw action auth -h --name gcal -- authorize remote --remote --step 1 \
  > workspace/gcal-remote-step1.out || fail 'gcal remote step 1 failed'
if printf '%s\n' 'https://callback.example.test/oauth2/callback?code=wrong-private-code&state=wrong' |
    ./bin/fclaw action auth -h --name gcal -- authorize remote --remote --step 2 \
      --redirect-url-stdin > workspace/gcal-remote-wrong-state.out 2>&1; then
  fail 'gcal accepted a remote redirect with the wrong OAuth state'
fi
grep -q 'redirect state did not match' workspace/gcal-remote-wrong-state.out ||
  fail 'gcal wrong-state rejection was not actionable'
printf '%s\n' 'https://callback.example.test/oauth2/callback?code=fixture-private-code&state=fixture' |
  ./bin/fclaw action auth -h --name gcal -- authorize remote --remote --step 2 \
    --redirect-url-stdin > workspace/gcal-remote-step2.out ||
  fail 'gcal remote step 2 failed'
./bin/fclaw action auth -h --name gcal -- account add manual \
  --email manual@example.test --client primary \
  > workspace/gcal-manual-account.out || fail 'gcal manual account onboarding failed'
printf '%s\n' 'https://callback.example.test/oauth2/callback?code=manual-private-code&state=fixture' |
  ./bin/fclaw action auth -h --name gcal -- authorize manual --manual \
    > workspace/gcal-manual-authorize.out || fail 'gcal manual authorization bridge failed'

run_action workspace/action-cli-gcal-accounts.json gcal \
  '{"op":"start","kind":"accounts"}'
grep -q 'manual@example.test' workspace/action-cli-gcal-accounts.json ||
  fail 'gcal accounts omitted sanitized account metadata'
run_action_failure workspace/action-cli-gcal-account-required.json gcal \
  '{"op":"start","kind":"list"}'
grep -q 'ACCOUNT_REQUIRED' workspace/action-cli-gcal-account-required.json ||
  fail 'gcal guessed an account when multiple accounts had no default'

./bin/fclaw action auth -h --name gcal -- account default personal \
  > workspace/gcal-default.out || fail 'gcal default account selection failed'
./bin/fclaw action auth -h --name gcal -- status personal \
  > workspace/gcal-status.out || fail 'gcal status failed'
grep -q $'status\tHEALTHY' workspace/gcal-status.out ||
  fail 'gcal status did not report healthy'
if grep -R -E -q 'fixture-client-secret-never-log|fixture-refresh|fixture-access|fixture-private-code|manual-private-code|wrong-private-code|legacy-client-secret-never-log|legacy-refresh-never-log' \
    workspace/gcal-*.out workspace/fake-gog-argv.log; then
  fail 'gcal operator output or gog argv exposed credential material'
fi

run_action workspace/action-cli-gcal-list.json gcal \
  '{"op":"start","kind":"list","time_min":"2026-08-05T00:00:00-05:00","time_max":"2026-08-06T00:00:00-05:00"}'
grep -q 'Fixture meeting' workspace/action-cli-gcal-list.json ||
  fail 'gcal list did not return fake gog Calendar JSON'
grep -R -q 'rotated-refresh' workspace/gcal-secret-home/secrets ||
  fail 'gcal did not atomically write rotated refresh token back to FloofClaw'

# Existing single-account deployments keep the original credential names.
# That record wins over newer account records and receives token rotation.
printf %s legacy-client-id | ./bin/fclaw auth set-stdin -a action:gcal:client_id
printf %s legacy-client-secret-never-log | ./bin/fclaw auth set-stdin -a action:gcal:client_secret
printf %s legacy-refresh-never-log | ./bin/fclaw auth set-stdin -a action:gcal:refresh_token
printf %s original@example.test | ./bin/fclaw auth set-stdin -a action:gcal:account_email
./bin/fclaw action auth -h --name gcal -- client restore-original \
  --credentials tests/fixtures/gcal/desktop-client.json --yes \
  > workspace/gcal-original-client-restore.out ||
  fail 'gcal could not atomically restore the original Desktop OAuth client'
[ "$(./bin/fclaw auth get action:gcal:client_id | sed -n 's/.*\"value\":\"\([^\"]*\)\".*/\1/p')" = fixture-client-id.apps.googleusercontent.com ] ||
  fail 'gcal original client restore did not replace client_id'
run_action workspace/action-cli-gcal-original.json gcal \
  '{"op":"start","kind":"list","time_min":"2026-08-05T00:00:00-05:00","time_max":"2026-08-06T00:00:00-05:00"}'
grep -q '"account":"original"' workspace/action-cli-gcal-original.json ||
  fail 'gcal did not prefer the original single-account credential names'
run_action workspace/action-cli-gcal-original-explicit.json gcal \
  '{"op":"start","kind":"list","account":"personal","time_min":"2026-08-05T00:00:00-05:00","time_max":"2026-08-06T00:00:00-05:00"}'
grep -q '"account":"original"' workspace/action-cli-gcal-original-explicit.json ||
  fail 'gcal let a newer explicit alias override the original credential names'
[ "$(./bin/fclaw auth get action:gcal:refresh_token | sed -n 's/.*\"value\":\"\([^\"]*\)\".*/\1/p')" = legacy-refresh-never-log ] ||
  fail 'gcal changed the authoritative original refresh_token'
./bin/fclaw action auth -h --name gcal -- reauthorize original --force-consent \
  > workspace/gcal-original-reauthorize.out ||
  fail 'gcal could not reauthorize the reserved original account'
[ "$(./bin/fclaw auth get action:gcal:refresh_token | sed -n 's/.*\"value\":\"\([^\"]*\)\".*/\1/p')" = fixture-refresh ] ||
  fail 'gcal reauthorization did not replace the original refresh_token'
./bin/fclaw auth delete -a action:gcal:client_id
./bin/fclaw auth delete -a action:gcal:client_secret
./bin/fclaw auth delete -a action:gcal:refresh_token
./bin/fclaw auth delete -a action:gcal:account_email

touch workspace/fake-gog-bad-writeback
run_action_failure workspace/action-cli-gcal-writeback-failure.json gcal \
  '{"op":"start","kind":"list","account":"personal"}'
rm workspace/fake-gog-bad-writeback
grep -q 'GOG_PROTOCOL_ERROR' workspace/action-cli-gcal-writeback-failure.json ||
  fail 'gcal did not fail explicitly when token writeback was rejected'
grep -R -q 'rotated-refresh' workspace/gcal-secret-home/secrets ||
  fail 'failed token writeback did not preserve the prior refresh token'
if grep -R -q 'uncommitted-refresh' workspace/gcal-secret-home/secrets; then
  fail 'failed token writeback committed an unvalidated refresh token'
fi

touch workspace/fake-gog-slow
rm -f workspace/fake-gog-entered
./bin/fclaw action exec -a --name gcal \
  --args '{"op":"start","kind":"list","account":"personal"}' \
  > workspace/action-cli-gcal-serialized.json &
serialized_pid=$!
for _ in {1..100}; do
  [ -f workspace/fake-gog-entered ] && break
  kill -0 "$serialized_pid" 2>/dev/null || fail 'serialized gcal action exited early'
  sleep 0.01
done
[ -f workspace/fake-gog-entered ] || fail 'serialized gcal action did not enter fake gog'
./bin/fclaw action auth -h --name gcal -- status personal \
  > workspace/gcal-serialized-status.out || fail 'concurrent gcal status failed'
wait "$serialized_pid" || fail 'serialized gcal action failed'
rm -f workspace/fake-gog-slow workspace/fake-gog-entered
[ ! -e workspace/fake-gog-active ] || fail 'fake gog concurrency guard remained active'
[ ! -e workspace/gcal-secret-home/.fclaw/action-locks/gcal-credentials.lock ] ||
  fail 'gcal credential serialization lock was not released'

touch workspace/fake-gog-revoked
run_action workspace/action-cli-gcal-diagnose.json gcal \
  '{"op":"start","kind":"diagnose","account":"personal"}'
rm workspace/fake-gog-revoked
grep -q 'REAUTHORIZATION_REQUIRED' workspace/action-cli-gcal-diagnose.json ||
  fail 'gcal diagnostics did not classify revoked grant'
grep -q 'fclaw action auth -h --name gcal -- reauthorize personal --force-consent' \
  workspace/action-cli-gcal-diagnose.json ||
  fail 'gcal diagnostics omitted safe reauthorization command'
grep -q 'cd .* && ./bin/fclaw action auth -h --name gcal -- reauthorize personal --force-consent' \
  workspace/action-cli-gcal-diagnose.json ||
  fail 'gcal diagnostics did not return a cwd-independent host command'
grep -q 'Google Calendar authorization is required for person@example.test. Run this exact command on the host:' \
  workspace/action-cli-gcal-diagnose.json ||
  fail 'gcal diagnostic continuation text lost the actionable authorization step'
grep -q 'reply authorized after the terminal reports success' \
  workspace/action-cli-gcal-diagnose.json ||
  fail 'gcal diagnostic continuation text omitted the post-authorization handoff'
touch workspace/fake-gog-api-disabled
run_action workspace/action-cli-gcal-api-disabled.json gcal \
  '{"op":"start","kind":"diagnose","account":"personal"}'
rm workspace/fake-gog-api-disabled
grep -q 'API_DISABLED' workspace/action-cli-gcal-api-disabled.json ||
  fail 'gcal diagnostics did not classify a disabled Calendar API'
grep -q 'https://console.developers.google.com/apis/api/calendar-json.googleapis.com/overview?project=572361359125' \
  workspace/action-cli-gcal-api-disabled.json ||
  fail 'gcal diagnostics omitted the provider Calendar API enable URL'
grep -q 'reply enabled' workspace/action-cli-gcal-api-disabled.json ||
  fail 'gcal diagnostics omitted the post-enable handoff'
if find "${TMPDIR:-/tmp}" -maxdepth 1 -type d -name 'fclaw-gcal-bridge.*' -print -quit | grep -q .; then
  fail 'gcal left a disposable gog home behind'
fi
if grep -R -E -q 'fixture-client-secret-never-log|fixture-refresh|fixture-access|fixture-private-code|manual-private-code|wrong-private-code|uncommitted-refresh|legacy-client-secret-never-log|legacy-refresh-never-log' \
    workspace/runs workspace/logs 2>/dev/null; then
  fail 'gcal credential material leaked into runtime artifacts'
fi

run_action workspace/action-cli-codex.json manage_codex \
  '{"op":"start","task":"Verify the managed Codex action instructions."}'
grep -Eq '"status":"(running|finished)"' workspace/action-cli-codex.json ||
  fail 'manage_codex did not start through action cli'

prompt=""
for _ in {1..100}; do
  for candidate in workspace/logs/codex_ops/*/prompt.txt; do
    if [[ -f "$candidate" ]]; then
      prompt="$candidate"
      break
    fi
  done
  [[ -n "$prompt" ]] && break
  sleep 0.02
done
[[ -n "$prompt" ]] || fail 'managed Codex prompt artifact was not created'
grep -q 'fclaw action list' "$prompt" ||
  fail 'managed Codex prompt omitted action discovery instructions'
grep -q '# Assigned task' "$prompt" ||
  fail 'managed Codex prompt omitted task boundary'
grep -q 'Verify the managed Codex action instructions.' "$prompt" ||
  fail 'managed Codex prompt omitted assigned task'
grep -q '# Shared task working memory' "$prompt" ||
  fail 'managed Codex prompt omitted shared working-memory boundary'
grep -q 'working_memory_append' "$prompt" ||
  fail 'managed Codex prompt omitted working-memory append instructions'

grep -R -q '"type":"action_request".*"action":"read_file"' workspace/runs ||
  fail 'action exec did not create a normal action_request event'
grep -R -q '"type":"action_started".*"action":"bash"' workspace/runs ||
  fail 'subprocess action did not enter the normal action runner'
grep -R -q '"type":"action_succeeded".*"action":"bash"' workspace/runs ||
  fail 'subprocess action did not record a normal terminal event'
if grep -R -q '"type":"operation_result".*"action":"read_file"' workspace/runs; then
  fail 'action cli managed result created an unrelated downstream wake'
fi
for runstate in workspace/runs/*/runstate.json; do
  grep -q '"type":"action_exec"' "$runstate" || continue
  run_dir="${runstate%/runstate.json}"
  if [[ -d "$run_dir/agent_outputs" ]] &&
      find "$run_dir/agent_outputs" -type f -print -quit | grep -q .; then
    fail 'action cli run dispatched a product agent'
  fi
done

printf 'action cli test passed\n'
