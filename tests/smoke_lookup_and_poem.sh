#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

if [ "${FCLAW_SMOKE_ISOLATED:-0}" != "1" ]; then
  tmp_parent="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
  case "$tmp_parent/" in
    "$ROOT/"*)
      printf 'smoke: TMPDIR must be outside the repository: %s\n' \
        "$tmp_parent" >&2
      exit 2
      ;;
  esac
  tmpdir="$(mktemp -d "$tmp_parent/fclaw-smoke.XXXXXX")"
  cleanup_smoke_copy() {
    local status=$?
    trap - EXIT
    cd "$ROOT" || status=1
    rm -rf -- "$tmpdir" || status=1
    exit "$status"
  }
  trap cleanup_smoke_copy EXIT
  rsync -a --exclude '.git' --exclude '.fclaw' --exclude 'workspace' \
    --exclude '.fclaw-smoke-isolated' ./ "$tmpdir"/
  cd "$tmpdir"
  printf '%s\n' "$ROOT" > .fclaw-smoke-isolated
  FCLAW_SMOKE_ISOLATED=1 \
    FCLAW_SMOKE_SOURCE_ROOT="$ROOT" \
    FCLAW_SMOKE_ISOLATED_ROOT="$(pwd -P)" \
    ./tests/smoke_lookup_and_poem.sh
  exit $?
fi

current_root="$(pwd -P)"
source_root="${FCLAW_SMOKE_SOURCE_ROOT:-}"
isolated_root="${FCLAW_SMOKE_ISOLATED_ROOT:-}"
case "$current_root/" in
  "$source_root/"*) inside_source=1 ;;
  *) inside_source=0 ;;
esac
if [[ -z "$source_root" || -z "$isolated_root" ||
      "$current_root" != "$isolated_root" || "$inside_source" == "1" ||
      ! -f .fclaw-smoke-isolated || -L .fclaw-smoke-isolated ]]; then
  printf '%s\n' \
    'smoke: refusing unsafe isolated override; fix: unset FCLAW_SMOKE_ISOLATED and rerun make test' \
    >&2
  exit 2
fi

export FCLAW_MODEL_PROFILES="$PWD/tests/fixtures/smoke/model_profiles_mock.json"

# The shipped config force-disables the setup-needing actions (manage_codex
# among them); this smoke fakes codex via FCLAW_CODEX_BIN, so enable it the
# way an operator would. Safe in place: this copy of the tree is isolated.
cat > config/floofclaw_config.json <<'CFG'
{ "bot_name": "FloofClaw", "default_floop": "hello", "force_disable": [] }
CFG

openclaw_prompt="How are you doing, what's your name, and please look up example.com on the internet and write a short poem about it in POEM.md."
modern_prompt="Make a small Frogger web app at frogger.html — single page, playable in the browser."

assert_openclaw_poem_result() {
  local out="$1"
  local run_id="$2"
  local log="workspace/runs/$run_id/event_log.jsonl"

  test -f "$log"
  test -f workspace/POEM.md
  grep -q '"type":"run_done"' "$log"
  grep -q "I'm doing well." <<<"$out"
  grep -q "I'm FloofClaw." <<<"$out"
  grep -q 'POEM.md' <<<"$out"
  grep -q 'Example Domain' workspace/POEM.md
  test "$(wc -l < workspace/POEM.md | tr -d ' ')" -ge 4
  grep -q '"source":"main_claw"' "$log"
  grep -Rq '"action":"bash"' workspace/runs/*/event_log.jsonl
  grep -Rq '"action":"write_file"' workspace/runs/*/event_log.jsonl
  grep -Rq 'example.com' workspace/runs/*/event_log.jsonl
  test "$(grep -Rh '"source":"main_claw"' workspace/runs/*/event_log.jsonl | wc -l | tr -d ' ')" -ge 3
  ! grep -Rq '"source":"openclaw_tool_calls"' workspace/runs/*/event_log.jsonl
  ! grep -Rq '"source":"openclaw_conversation"' workspace/runs/*/event_log.jsonl
  grep -Rq '"type":"operation_result"' workspace/runs/*/event_log.jsonl
  grep -q 'Tool bash returned:' workspace/memory/memory.jsonl
  grep -q 'Tool write_file returned:' workspace/memory/memory.jsonl
}

assert_modern_frogger_result() {
  local out="$1"
  local run_id="$2"
  local log="workspace/runs/$run_id/event_log.jsonl"

  test -f "$log"
  test -f workspace/frogger.html
  grep -q '"type":"run_done"' "$log"
  grep -q '"source":"chat_manager"' "$log"
  grep -q '"action":"manage_codex"' "$log"
  grep -q '"state":"succeeded"' "$log"
  grep -q 'I’ll build the Frogger page' <<<"$out"
  grep -Rq '"type":"operation_result"' workspace/runs/*/event_log.jsonl
  grep -q '<canvas id="game"' workspace/frogger.html
}

run_openclaw() {
  rm -rf workspace/runs workspace/memory workspace/media workspace/POEM.md
  local network_guard="$PWD/workspace/.smoke-no-network"
  local network_marker="$network_guard/attempted"
  mkdir -p "$network_guard"
  cp "$PWD/tests/fixtures/smoke/deny_network.sh" "$network_guard/curl"
  chmod +x "$network_guard/curl"
  export FCLAW_SMOKE_NETWORK_ATTEMPT_FILE="$network_marker"
  local guard_rc=0
  PATH="$network_guard:$PATH" curl https://example.com >/dev/null 2>&1 || guard_rc=$?
  test "$guard_rc" -eq 97
  test -f "$network_marker"
  rm "$network_marker"
  export LLM_MOCK_RESPONSE_PATHS="$PWD/tests/fixtures/smoke/openclaw_lookup_and_poem_tool.json:$PWD/tests/fixtures/smoke/openclaw_lookup_and_poem_write.json:$PWD/tests/fixtures/smoke/openclaw_lookup_and_poem_conversation.json"
  local out run_id
  out="$(PATH="$network_guard:$PATH" ./bin/fclaw run -a --floop openclaw --text "$openclaw_prompt")"
  printf '%s\n' "$out"
  run_id="$(printf '%s\n' "$out" | sed -n 's/^{"run_id":"\([^"]*\)".*/\1/p')"
  test -n "$run_id"
  assert_openclaw_poem_result "$out" "$run_id"
  test ! -e "$network_marker"
}

run_modern_floop() {
  rm -rf workspace/runs workspace/memory workspace/media workspace/frogger.html
  export FCLAW_CODEX_BIN="$PWD/tests/fixtures/smoke/fake_codex.sh"
  export FCLAW_CODEX_START_SYNC_MS=500
  # Default floofclaw runs the Action20 controller: chat_manager admits+acks the
  # work (turn 1), then work_manager selects manage_codex (turn 2). `fclaw run`
  # returns on chat_manager's synchronous ack, so a single invocation consumes
  # exactly these two LLM turns; the codex operation completes in-run and writes
  # frogger.html. (result_manager's terminal turn is driven by a bus follow-up
  # run, which this synchronous smoke does not pump.)
  export LLM_MOCK_RESPONSE_PATHS="$PWD/tests/fixtures/smoke/modern_chat_frogger_work.json:$PWD/tests/fixtures/smoke/modern_work_manager_codex.json"
  local out run_id
  out="$(./bin/fclaw run --floop floofclaw --text "$modern_prompt")"
  printf '%s\n' "$out"
  run_id="$(printf '%s\n' "$out" | sed -n 's/^{"run_id":"\([^"]*\)".*/\1/p')"
  test -n "$run_id"
  assert_modern_frogger_result "$out" "$run_id"
}

run_openclaw
run_modern_floop

printf 'lookup and poem smoke ok\n'
