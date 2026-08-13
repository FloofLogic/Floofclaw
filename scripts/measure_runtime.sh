#!/usr/bin/env bash
# Run only through tests/run_in_isolated_copy.sh; emits key=value metrics.
set -euo pipefail

[ "${FCLAW_TEST_ISOLATED:-}" = "1" ] || {
  echo "measure-runtime: refusing to touch a non-isolated workspace" >&2
  exit 1
}
command -v jq >/dev/null 2>&1 || {
  echo "measure-runtime: jq is required for structural status parsing" >&2
  exit 1
}

timing_file="workspace/metrics-hello-time.txt"
mkdir -p workspace
TIMEFORMAT='%R'
{ time ./bin/fclaw run -a --text "hello" >/dev/null; } 2>"$timing_file"
hello_seconds="$(tail -n 1 "$timing_file" | tr -d '[:space:]')"

gateway_pid=""
cleanup() {
  ./bin/fclaw gateway stop -a >/dev/null 2>&1 || true
  if [ -n "$gateway_pid" ]; then
    wait "$gateway_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

./bin/fclaw gateway run -a >workspace/metrics-gateway.log 2>&1 &
gateway_pid=$!
running=0
for _ in {1..50}; do
  status_json="$(./bin/fclaw gateway status -a 2>/dev/null || true)"
  if jq -e '.gateway.running == true' >/dev/null 2>&1 <<<"$status_json"; then
    running=1
    break
  fi
  sleep 0.1
done
[ "$running" -eq 1 ] || {
  echo "measure-runtime: gateway did not become ready" >&2
  exit 1
}

idle_rss_kb="$(ps -o rss= -p "$gateway_pid" 2>/dev/null | tr -d '[:space:]' || true)"
[ -n "$idle_rss_kb" ] || idle_rss_kb="unavailable"

printf 'isolated_hello_wall_seconds=%s\n' "$hello_seconds"
printf 'idle_gateway_rss_kb=%s\n' "$idle_rss_kb"
