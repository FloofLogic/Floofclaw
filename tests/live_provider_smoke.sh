#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

if [ "${FCLAW_LIVE_SMOKE:-0}" != "1" ]; then
  cat >&2 <<'MSG'
live provider smoke is opt-in.
Run: FCLAW_LIVE_SMOKE=1 make live-smoke
This sends one cheap request through the configured provider profile.
MSG
  exit 2
fi

has_auth=0
for env_name in GEMINI_API_KEY OPENROUTER_API_KEY OPENAI_API_KEY ANTHROPIC_API_KEY; do
  if [ -n "${!env_name:-}" ]; then
    has_auth=1
  fi
done
for endpoint in gemini_key openrouter_key openai_key anthropic_key; do
  if ./bin/fclaw auth get -a "$endpoint" >/dev/null 2>&1; then
    has_auth=1
  fi
done
if [ "$has_auth" -ne 1 ]; then
  cat >&2 <<'MSG'
live provider smoke needs provider auth first.
Set a provider env var such as GEMINI_API_KEY or OPENROUTER_API_KEY, or store
one with ./bin/fclaw auth set -h gemini_key|openrouter_key|openai_key|anthropic_key.
MSG
  exit 2
fi

if [ "${FCLAW_LIVE_SMOKE_ISOLATED:-0}" != "1" ]; then
  tmp_parent="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
  case "$tmp_parent/" in
    "$ROOT/"*)
      printf 'live smoke: TMPDIR must be outside the repository: %s\n' \
        "$tmp_parent" >&2
      exit 2
      ;;
  esac
  tmpdir="$(mktemp -d "$tmp_parent/fclaw-live-smoke.XXXXXX")"
  cleanup_live_smoke_copy() {
    local status=$?
    trap - EXIT
    cd "$ROOT" || status=1
    rm -rf -- "$tmpdir" || status=1
    exit "$status"
  }
  trap cleanup_live_smoke_copy EXIT
  rsync -a --exclude '.git' --exclude '.fclaw' --exclude 'workspace' \
    --exclude '.fclaw-live-smoke-isolated' ./ "$tmpdir"/
  cd "$tmpdir"
  printf '%s\n' "$ROOT" > .fclaw-live-smoke-isolated
  FCLAW_LIVE_SMOKE_ISOLATED=1 FCLAW_LIVE_SMOKE=1 \
    FCLAW_LIVE_SMOKE_SOURCE_ROOT="$ROOT" \
    FCLAW_LIVE_SMOKE_ISOLATED_ROOT="$(pwd -P)" \
    ./tests/live_provider_smoke.sh
  exit $?
fi

current_root="$(pwd -P)"
source_root="${FCLAW_LIVE_SMOKE_SOURCE_ROOT:-}"
isolated_root="${FCLAW_LIVE_SMOKE_ISOLATED_ROOT:-}"
case "$current_root/" in
  "$source_root/"*) inside_source=1 ;;
  *) inside_source=0 ;;
esac
if [[ -z "$source_root" || -z "$isolated_root" ||
      "$current_root" != "$isolated_root" || "$inside_source" == "1" ||
      ! -f .fclaw-live-smoke-isolated || -L .fclaw-live-smoke-isolated ]]; then
  printf '%s\n' \
    'live smoke: refusing unsafe isolated override; fix: unset FCLAW_LIVE_SMOKE_ISOLATED and rerun make live-smoke' \
    >&2
  exit 2
fi

profile_ref="${FCLAW_LIVE_PROFILE_REF:-responses}"
unset LLM_MOCK_RESPONSE_PATHS

rm -rf workspace/runs workspace/memory workspace/media workspace/logs workspace/bus
mkdir -p loops agents/live_provider_smoke_agent

cat > loops/live_provider_smoke_loop.json <<'JSON'
{
  "version": 1,
  "name": "live_provider_smoke_loop",
  "steps": [
    { "id": "speak_status", "type": "agent", "agent": "live_provider_smoke_agent" }
  ]
}
JSON

cat > agents/live_provider_smoke_agent/agent.json <<JSON
{
  "id": "live_provider_smoke_agent",
  "executor": "llm",
  "model": { "ref": "$profile_ref" },
  "actions": ["message"],
  "conversational_payload_only": true,
  "autocomplete_message_task_on_send": true,
  "listen": ["tasks"]
}
JSON

cat > agents/live_provider_smoke_agent/prompt.md <<'PROMPT'
Return exactly this JSON object and nothing else:
{"message":"pong"}
PROMPT

out="$(./bin/fclaw run --floop live_provider_smoke_loop --text "live provider smoke: reply pong" 2>&1)" || {
  rc=$?
  printf '%s\n' "$out" >&2
  if grep -q 'agent_output_invalid' <<<"$out"; then
    echo "live provider smoke failed as agent_output_invalid; provider normalization path regressed" >&2
  fi
  exit "$rc"
}
printf '%s\n' "$out"

run_id="$(printf '%s\n' "$out" | sed -n 's/^{"run_id":"\([^"]*\)".*/\1/p')"
if [ -z "$run_id" ]; then
  echo "live provider smoke could not find run_id in fclaw output" >&2
  exit 1
fi

provider_dir="workspace/runs/$run_id/provider_calls"
test -d "$provider_dir"
find "$provider_dir" -name '*_request.json' -print -quit | grep -q .
find "$provider_dir" -name '*_raw_request.json' -print -quit | grep -q .
find "$provider_dir" -name '*_raw_response.json' -print -quit | grep -q .
find "$provider_dir" -name '*_response.json' -print -quit | grep -q .
grep -R '"ok":true' "$provider_dir"/*_meta.json >/dev/null
grep -q 'pong' <<<"$out"
! grep -R 'agent_output_invalid' "workspace/runs/$run_id" >/dev/null

printf 'live provider smoke ok (%s via %s)\n' "$run_id" "$profile_ref"
