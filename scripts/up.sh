#!/usr/bin/env bash
# up.sh — bring the whole floofclaw demo stack up, idempotently.
#
#   ./scripts/up.sh
#
# Starts whatever isn't already running, leaves alone whatever is:
#   1. the fclaw gateway (config default floop, or FLOOP=name override)
#   2. Pulse        http://127.0.0.1:8003  (live board)
#   3. Runtime Flow http://127.0.0.1:8002  (run analyzer; Pulse run
#      cards deep-link here via ?run=)
#   4. Tokenwatch   http://127.0.0.1:8004  (usage, cache, and spend)
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$repo_root"

PULSE_PORT=8003
FLOW_PORT=8002
TOKENWATCH_PORT=8004
# Floop: config's default_floop unless overridden (FLOOP=name ./scripts/up.sh).
# A fresh clone gets the mock-backed hello floop; a configured install gets
# whatever its config selects.
FLOOP="${FLOOP:-}"

port_busy() { lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; }

if [ ! -x bin/fclaw ]; then
  echo "bin/fclaw missing — building..."
  make
fi

gateway_json="$(./bin/fclaw gateway status -a 2>/dev/null || true)"
if python3 -c 'import json,sys; sys.exit(not json.load(sys.stdin)["gateway"]["running"])' \
    <<<"$gateway_json" 2>/dev/null; then
  gateway_pid="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["gateway"]["pid"])' \
    <<<"$gateway_json")"
  echo "gateway: already running (pid $gateway_pid)"
else
  floop_args=()
  [ -n "$FLOOP" ] && floop_args=(--floop "$FLOOP")
  mkdir -p workspace
  nohup ./bin/fclaw gateway start -h "${floop_args[@]}" \
    > workspace/gateway_console.log 2>&1 &
  sleep 2
  gateway_json="$(./bin/fclaw gateway status -a 2>/dev/null || true)"
  if python3 -c 'import json,sys; sys.exit(not json.load(sys.stdin)["gateway"]["running"])' \
      <<<"$gateway_json" 2>/dev/null; then
    echo "gateway: started (floop ${FLOOP:-config default})"
  else
    echo "gateway: FAILED to start — see workspace/gateway_console.log" >&2
    exit 1
  fi
fi

start_app() {
  local name="$1" dir="$2" port="$3"
  if port_busy "$port"; then
    echo "$name: already serving on :$port"
  else
    (cd "$dir" && nohup python3 -m http.server "$port" --bind 127.0.0.1 \
      > "/tmp/fclaw-$name-server.log" 2>&1 &)
    sleep 1
    if port_busy "$port"; then
      echo "$name: started on :$port"
    else
      echo "$name: FAILED — see /tmp/fclaw-$name-server.log" >&2
      exit 1
    fi
  fi
}

python3 app/pulse/generate.py --repo-root . >/dev/null
python3 app/runtime_flow/generate.py >/dev/null 2>&1 || true
python3 app/tokenwatch/generate.py --repo-root . >/dev/null

# Generator loop: the pages are static and only ever re-read their JSON
# artifacts; this keeps the artifacts fresh. Pulse every cycle, Runtime
# Flow every 4th (it scans every run and is heavier).
if pgrep -f "fclaw-dashboard-regen" >/dev/null 2>&1; then
  echo "regen loop: already running"
else
  nohup bash -c 'exec -a fclaw-dashboard-regen bash -c "
    n=0
    while :; do
      python3 app/pulse/generate.py --repo-root . >/dev/null 2>&1
      python3 app/tokenwatch/generate.py --repo-root . >/dev/null 2>&1
      if [ \$((n % 4)) -eq 0 ]; then
        python3 app/runtime_flow/generate.py >/dev/null 2>&1
      fi
      n=\$((n + 1))
      sleep 3
    done"' > /tmp/fclaw-regen.log 2>&1 &
  echo "regen loop: started (pulse/tokenwatch 3s, runtime flow 12s)"
fi
start_app pulse app/pulse "$PULSE_PORT"
start_app flow  app/runtime_flow "$FLOW_PORT"
start_app tokenwatch app/tokenwatch "$TOKENWATCH_PORT"

echo
echo "  pulse:  http://127.0.0.1:$PULSE_PORT"
echo "  flow:   http://127.0.0.1:$FLOW_PORT"
echo "  tokens: http://127.0.0.1:$TOKENWATCH_PORT"
