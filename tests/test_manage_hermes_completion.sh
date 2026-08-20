#!/usr/bin/env bash
# manage_hermes hermetic completion + timeout proof.
#
# A local python fixture server plays the Hermes API. The first run's
# job completes: the detached waiter (spawned via `fclaw internal
# detach`) observes it leaf-side and submits one claim through
# `fclaw operation complete`, which returns as a correlated
# operation_result run. The second run's job never completes: the
# action-declared deadline (shortened in the copied manifest) produces
# exactly one timed_out consequence. No provider credentials, no
# network, no operation_poll envelope anywhere.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

if [[ "${FCLAW_TEST_ISOLATED:-0}" != "1" || ! -f .fclaw-test-isolated ]]; then
  printf '%s\n' 'manage_hermes completion: refusing to run outside an isolated copy' >&2
  exit 2
fi

gateway_running=0
server_pid=""
stop_gateway() {
  if [[ "$gateway_running" == "1" ]]; then
    ./bin/fclaw gateway stop -a >/dev/null 2>&1 || true
    gateway_running=0
  fi
}
cleanup() {
  local status=$?
  trap - EXIT
  stop_gateway
  pkill -f "manage_hermes/run.sh __waiter" 2>/dev/null || true
  if [[ -n "$server_pid" ]]; then kill "$server_pid" 2>/dev/null || true; fi
  exit "$status"
}
trap cleanup EXIT

port="$(./bin/fclaw internal free-port)"

python3 - "$port" <<'PY' &
import json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer

port = int(sys.argv[1])
started = {}
count = 0

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        global count
        if self.path != "/v1/runs":
            return self._send(404, {"error": {"code": "not_found"}})
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        count += 1
        run_id = "fixt_done" if count == 1 else "fixt_stuck"
        started[run_id] = time.time()
        self._send(202, {"run_id": run_id})
    def do_GET(self):
        if not self.path.startswith("/v1/runs/"):
            return self._send(404, {"error": {"code": "not_found"}})
        run_id = self.path.rsplit("/", 1)[1]
        if run_id not in started:
            return self._send(404, {"error": {"code": "run_not_found"}})
        if run_id == "fixt_done" and time.time() - started[run_id] > 0.5:
            return self._send(200, {"status": "completed",
                                    "output": "HERMES_FIXTURE_OUTPUT_OK"})
        self._send(200, {"status": "running"})

HTTPServer(("127.0.0.1", port), H).serve_forever()
PY
server_pid=$!

export FCLAW_MODEL_PROFILES="$PWD/tests/fixtures/smoke/model_profiles_mock.json"
export FCLAW_GATEWAY_POLL_MS=5
export FCLAW_GATEWAY_DISABLE_CHANNELS=1
export HERMES_API_URL="http://127.0.0.1:$port"

# The shipped config force-disables manage_hermes until an operator sets it
# up; this proof fakes the Hermes server, so enable it the way an operator
# would. Safe in place: this script refuses to run outside an isolated copy.
cat > config/floofclaw_config.json <<'CFG'
{ "bot_name": "FloofClaw", "default_floop": "hello", "force_disable": [] }
CFG

printf %s fixture-hermes-key |
  ./bin/fclaw auth set-stdin -a action:manage_hermes:api_server_key >/dev/null

./bin/fclaw clear -a --yes >/dev/null
mkdir -p workspace/fixtures
cat > workspace/fixtures/hermes_start.json <<'EOF'
{"calls":[{"name":"manage_hermes","args":{"op":"start","task":"do the hermes fixture work"}}]}
EOF
cat > workspace/fixtures/hermes_done_reply.json <<'EOF'
{"calls":[{"name":"message","args":{"message":"HERMES_DONE_REPLY"}},{"name":"task.update","args":{"task_id":"task_run_001_000002","state":"completed"}}]}
EOF
cat > workspace/fixtures/hermes_timeout_reply.json <<'EOF'
{"calls":[{"name":"message","args":{"message":"HERMES_TIMEOUT_REPLY"}},{"name":"task.update","args":{"task_id":"task_run_003_000002","state":"completed"}}]}
EOF

# ---- completion half -------------------------------------------------
export LLM_MOCK_RESPONSE_PATHS="$PWD/workspace/fixtures/hermes_start.json:$PWD/workspace/fixtures/hermes_done_reply.json"
./bin/fclaw gateway start -a --floop openclaw >/dev/null
gateway_running=1
out="$(./bin/fclaw run --floop openclaw --text 'run the hermes fixture')"
printf '%s\n' "$out"
grep -q 'HERMES_DONE_REPLY' <<<"$out"
stop_gateway

grep -q 'HERMES_FIXTURE_OUTPUT_OK' workspace/logs/bus.jsonl
grep -q '"type":"operation_completed"' workspace/logs/bus.jsonl
if grep -q 'operation_poll' workspace/logs/bus.jsonl; then
  printf '%s\n' 'manage_hermes completion: found a poll envelope' >&2
  exit 1
fi
ls workspace/logs/hermes_ops/*.waiter.log >/dev/null

# ---- timeout half ----------------------------------------------------
# Shorten the copied manifest's action-owned deadline; the fixture job
# for run 2 never completes.
python3 - <<'PY'
import json
p = "actions/common/manage_hermes/action.json"
d = json.load(open(p))
d["default_timeout_ms"] = 1500
d["max_timeout_ms"] = 1500
json.dump(d, open(p, "w"), indent=2)
open(p, "a").write("\n")
PY

export LLM_MOCK_RESPONSE_PATHS="$PWD/workspace/fixtures/hermes_start.json:$PWD/workspace/fixtures/hermes_timeout_reply.json"
./bin/fclaw gateway start -a --floop openclaw >/dev/null
gateway_running=1
out="$(./bin/fclaw run --floop openclaw --text 'run the stuck hermes fixture')"
printf '%s\n' "$out"
grep -q 'HERMES_TIMEOUT_REPLY' <<<"$out"
stop_gateway

if [[ "$(grep -c '"status":"timed_out"' workspace/runs/*/event_log.jsonl | awk -F: '{s+=$2} END {print s}')" -lt 1 ]]; then
  printf '%s\n' 'manage_hermes completion: no timed_out consequence recorded' >&2
  exit 1
fi
grep -q 'reached its completion deadline' workspace/logs/bus.jsonl

printf '%s\n' 'manage_hermes hermetic completion + timeout ok'
