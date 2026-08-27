#!/usr/bin/env bash
# Telegram live-transport proof, hermetic.
#
# A local python fixture plays the Bot API on loopback plain HTTP. The
# adapter reaches it through the channels.telegram.api_base test seam,
# which is refused entirely unless FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK=1
# and then accepts exactly http://127.0.0.1:<port>. Everything else is
# the production path: the reactor conn state machine, the shared http1
# parser, getUpdates -> getFile -> private manifest -> LLM-child download ->
# Gemini request -> delivery -> outbox sendMessage, and the
# offset-acks-after-publish rule.
#
# Drives ./bin/fclaw_test_all: the gateway binary that always contains
# every adapter, so this gate does not depend on what FCLAW_ADAPTERS
# selected for bin/fclaw.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

if [[ "${FCLAW_TEST_ISOLATED:-0}" != "1" || ! -f .fclaw-test-isolated ]]; then
  printf '%s\n' 'telegram transport: refusing to run outside an isolated copy' >&2
  exit 2
fi

FCLAW=./bin/fclaw_test_all
[[ -x "$FCLAW" ]] || { echo "telegram transport: build bin/fclaw_test_all first" >&2; exit 2; }

gateway_running=0
server_pid=""
stop_gateway() {
  if [[ "$gateway_running" == "1" ]]; then
    "$FCLAW" gateway stop -a >/dev/null 2>&1 || true
    gateway_running=0
  fi
}
cleanup() {
  local status=$?
  trap - EXIT
  stop_gateway
  if [[ -n "$server_pid" ]]; then kill "$server_pid" 2>/dev/null || true; fi
  exit "$status"
}
trap cleanup EXIT

fail() { printf 'telegram transport: %s\n' "$1" >&2; exit 1; }

file_mode() {
  if stat -c '%a' "$1" >/dev/null 2>&1; then
    stat -c '%a' "$1"
  else
    stat -f '%Lp' "$1"
  fi
}

port="$("$FCLAW" internal free-port)"
fixture_dir="$PWD/tg_fixture"
mkdir -p "$fixture_dir"

cat > "$fixture_dir/model_profiles.json" <<'JSON'
{
  "version": 1,
  "profiles": [
    {"id":"hello","provider":"telegram_fixture","model":"fixture"}
  ]
}
JSON
cat > "$fixture_dir/llm_registry.json" <<JSON
{
  "version": 1,
  "providers": [
    {"id":"telegram_fixture","kind":"gemini_key",\
"url":"http://127.0.0.1:$port"}
  ]
}
JSON

# ----- configure: telegram enabled, DMs allowed, api_base at the fixture -----
printf '%s' 'TESTTOKEN' | "$FCLAW" auth set-stdin -a telegram_token >/dev/null

python3 - "$port" <<'PY'
import json, sys
path = "config/floofclaw_config.json"
with open(path) as f:
    cfg = json.load(f)
# Only the channel under test may be enabled: on a deployment branch the
# checked-in config enables that bot's channel, and its missing token would
# be the refusal named instead of the Telegram seam. The transport fixture
# uses the small hello floop, but makes its one LLM listen to the event so
# the ordinary event-media handoff is exercised without the full manager.
cfg["default_floop"] = "hello"
for ch in cfg.get("channels", {}).values():
    if isinstance(ch, dict):
        ch["enabled"] = False
cfg.setdefault("channels", {})["telegram"] = {
    "enabled": True,
    "allow_dms": True,
    "api_base": "http://127.0.0.1:%s" % sys.argv[1],
}
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)

agent_path = "floops/hello/agents/hello/agent.json"
with open(agent_path) as f:
    agent = json.load(f)
agent["listen"] = ["event", "tasks"]
with open(agent_path, "w") as f:
    json.dump(agent, f, indent=2)
PY

# ----- the seam is refused without the hermetic switch -----
# Adapter creation happens in the daemonized child, so under load the
# parent `gateway start` can return 0 before the child reaches the refusal
# and exits. Accept both shapes: an immediate nonzero, or a daemon that
# takes itself down within a few seconds. What must never happen is a
# gateway that stays up with api_base set and no switch.
set +e
env -u FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK \
    "$FCLAW" gateway start -a >/dev/null 2>"$fixture_dir/refused.err"
start_rc=$?
set -e
if [[ "$start_rc" -eq 0 ]]; then
  aborted=0
  for _ in $(seq 1 20); do
    if ! "$FCLAW" gateway status -a 2>/dev/null | grep -q '"running":true'; then
      aborted=1; break
    fi
    sleep 0.5
  done
  if [[ "$aborted" != "1" ]]; then
    "$FCLAW" gateway stop -a >/dev/null 2>&1 || true
    fail "gateway stayed up with api_base set but no hermetic switch"
  fi
fi
# The named fix goes to gateway.log, not the parent CLI's stderr.
grep -q "FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK" \
    "$fixture_dir/refused.err" workspace/logs/gateway.log 2>/dev/null ||
  fail "refusal did not name the hermetic switch"

# ----- fixture: plays the Bot API, records what it sees -----
python3 - "$port" "$fixture_dir" <<'PY' &
import json, sys, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port = int(sys.argv[1])
out = sys.argv[2]
state = {"served_update": False}

UPDATE = {"ok": True, "result": [{
    "update_id": 7,
    "message": {
        "message_id": 1,
        "caption": "describe this fixture photo",
        "photo": [
            {"file_id": "photo-small", "file_unique_id": "small", "file_size": 2,
             "width": 16, "height": 16},
            {"file_id": "photo-large", "file_unique_id": "large", "file_size": 4,
             "width": 1024, "height": 768},
        ],
        "chat": {"id": 424242, "type": "private"},
        "from": {"id": 424242, "is_bot": False},
    },
}]}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, obj, content_type="application/json"):
        if isinstance(obj, bytes):
            body = obj
        else:
            body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/file/botTESTTOKEN/photos/file_0.jpg":
            with open(out + "/media_fetch.log", "a") as f:
                f.write(parsed.path + "\n")
            self._send(b"JPEG", "image/jpeg")
            return
        if "/getUpdates" not in parsed.path:
            self._send({"ok": False}); return
        if "botTESTTOKEN" not in parsed.path:
            self._send({"ok": False, "description": "wrong token"}); return
        offset = urllib.parse.parse_qs(parsed.query).get("offset", ["0"])[0]
        with open(out + "/offsets.log", "a") as f:
            f.write(offset + "\n")
        if not state["served_update"]:
            state["served_update"] = True
            self._send(UPDATE)
        else:
            time.sleep(1.0)  # hold the long poll briefly
            self._send({"ok": True, "result": []})
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode()
        if "/getFile" in self.path:
            request = json.loads(body)
            with open(out + "/get_file.log", "a") as f:
                f.write(request.get("file_id", "") + "\n")
            self._send({"ok": True, "result": {
                "file_id": request.get("file_id"),
                "file_unique_id": "large",
                "file_size": 4,
                "file_path": "photos/file_0.jpg",
            }})
            return
        if "/models/fixture:generateContent" in self.path:
            with open(out + "/llm_request.json", "w") as f:
                f.write(body)
            self._send({"candidates": [{"content": {"parts": [{
                "text": json.dumps({"message": "I saw the Telegram photo."})
            }]}}]})
            return
        if "/sendMessage" in self.path:
            with open(out + "/sent.log", "a") as f:
                f.write(body + "\n")
        self._send({"ok": True, "result": {"message_id": 2}})

ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
PY
server_pid=$!

# Readiness probe on a neutral path: it must not touch getUpdates, which
# serves the single fixture update to its first caller.
for _ in $(seq 1 50); do
  if curl -s "http://127.0.0.1:$port/health" >/dev/null 2>&1; then break; fi
  sleep 0.1
done

# ----- the real run -----
export FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK=1
export FCLAW_MEDIA_TEST_ALLOW_LOOPBACK=1
export FCLAW_DISABLE_LLM_STREAM=1
export FCLAW_MODEL_PROFILES="$fixture_dir/model_profiles.json"
export FCLAW_LLM_REGISTRY="$fixture_dir/llm_registry.json"
"$FCLAW" gateway start -a >/dev/null
gateway_running=1

deadline=$((SECONDS + 45))
sent_ok=0
while (( SECONDS < deadline )); do
  if [[ -s "$fixture_dir/sent.log" ]]; then sent_ok=1; break; fi
  sleep 0.5
done
[[ "$sent_ok" == "1" ]] || fail "no sendMessage reached the fixture"

grep -q '"chat_id":"424242"' "$fixture_dir/sent.log" ||
  fail "sendMessage did not target the originating chat"
python3 - "$fixture_dir/sent.log" <<'PY' || exit 1
import json, sys
line = open(sys.argv[1]).readline()
body = json.loads(line)
assert body.get("text"), "sendMessage carried no text"
PY

# The inbound message crossed the bus as ordinary channel traffic.
grep -q '"type":"user_message"' workspace/logs/bus.jsonl &&
  grep -q '"channel":"telegram"' workspace/logs/bus.jsonl ||
  fail "no telegram user_message on the bus"
grep -q '"text":"describe this fixture photo"' workspace/logs/bus.jsonl &&
  grep -q '"media":{"version":1' workspace/logs/bus.jsonl ||
  fail "Telegram caption and compact media reference did not reach the bus"

# getFile selected only the largest photo, then the LLM child fetched the
# private source and embedded those exact bytes in its Gemini request.
grep -qx 'photo-large' "$fixture_dir/get_file.log" ||
  fail "getFile did not select the largest Telegram photo"
[[ "$(wc -l < "$fixture_dir/get_file.log" | tr -d '[:space:]')" == "1" ]] ||
  fail "getFile resolved more than the selected Telegram photo"
grep -qx '/file/botTESTTOKEN/photos/file_0.jpg' "$fixture_dir/media_fetch.log" ||
  fail "the LLM child did not download the Telegram file"
grep -q '"inline_data"' "$fixture_dir/llm_request.json" &&
  grep -q '"mime_type":"image/jpeg"' "$fixture_dir/llm_request.json" &&
  grep -q '"data":"SlBFRw=="' "$fixture_dir/llm_request.json" ||
  fail "Gemini did not receive the downloaded Telegram photo"

manifest_count="$(find workspace/media/inbound -type f -name '*.json' | wc -l | tr -d '[:space:]')"
[[ "$manifest_count" == "1" ]] || fail "expected one private inbound media manifest"
manifest="$(find workspace/media/inbound -type f -name '*.json' -print -quit)"
[[ "$(file_mode "$manifest")" == "600" ]] ||
  fail "Telegram media manifest is not mode 0600"
grep -q '/file/botTESTTOKEN/photos/file_0.jpg' "$manifest" ||
  fail "private manifest does not carry the resolved Telegram source"
run_media="$(find workspace/runs -type f -path '*/media/*.bin' -print -quit)"
[[ -n "$run_media" && "$(cat "$run_media")" == "JPEG" ]] ||
  fail "downloaded Telegram bytes are absent from the run-local media cache"

# The token-bearing URL is confined to the private manifest; it must never
# enter the bus, gateway log, or an agent/run artifact.
if grep -R -F 'TESTTOKEN' workspace/logs/bus.jsonl workspace/logs/gateway.log \
    workspace/runs >/dev/null 2>&1; then
  fail "Telegram token escaped the private manifest"
fi

# Offset acked only after publish: the file holds update_id + 1 ...
offset_deadline=$((SECONDS + 10))
while (( SECONDS < offset_deadline )); do
  [[ -f .fclaw/run/telegram_offset ]] && break
  sleep 0.5
done
[[ "$(tr -d '[:space:]' < .fclaw/run/telegram_offset)" == "8" ]] ||
  fail "offset file does not hold 8 (update_id + 1)"

# ... and a later poll carried it back to the server.
ack_deadline=$((SECONDS + 15))
acked=0
while (( SECONDS < ack_deadline )); do
  if grep -qx "8" "$fixture_dir/offsets.log"; then acked=1; break; fi
  sleep 0.5
done
[[ "$acked" == "1" ]] || fail "no later getUpdates carried offset=8"

stop_gateway
echo "telegram transport test passed"
