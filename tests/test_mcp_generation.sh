#!/usr/bin/env bash
# MCP arrives as generated actions, and the kernel never learns the word.
#
# Proves the whole chain against hermetic fixture servers: the generator
# turns a tools/list catalog into correct action manifests, re-sync is
# idempotent and prunes stale tools, the bridge round-trips a real call,
# every server failure mode fails this one call loudly instead of wedging
# anything, and a generated action runs end-to-end through a floop as an
# ordinary managed operation — allowlisted, force-disable-able, schema
# validated, with its credential injected but absent from every artifact.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

if [[ "${FCLAW_TEST_ISOLATED:-0}" != "1" || ! -f .fclaw-test-isolated ]]; then
  printf '%s\n' 'mcp generation: refusing to run outside an isolated copy' >&2
  exit 2
fi

fail() { printf 'mcp generation: %s\n' "$1" >&2; exit 1; }

gateway_running=0
http_pid=""
cleanup() {
  local status=$?
  trap - EXIT
  [[ "$gateway_running" == "1" ]] && ./bin/fclaw gateway stop -a >/dev/null 2>&1
  [[ -n "$http_pid" ]] && kill "$http_pid" 2>/dev/null
  exit "$status"
}
trap cleanup EXIT

FIX=tests/fixtures/mcp/stdio_server.sh
export FCLAW_MCP_CALL_TIMEOUT_S=2
export FCLAW_MCP_LIST_TIMEOUT_S=3

write_config() { cat > config/mcp.json; }

# Subprocess actions are handed an input envelope, not bare args. Calling a
# generated action by hand means building the same envelope the jobrunner
# would, so these checks exercise the real contract.
envelope() { # envelope <args-json> [request-id]
  jq -cn --argjson a "$1" --arg r "${2:-req_test}" \
    '{request_id:$r,action:"test",args:$a,run_id:"run_test",task_id:"task_test",
      call_id:$r,artifacts:{body:("runs/run_test/action_calls/" + $r + ".body.json")}}'
}
export FCLAW_WORKSPACE_ROOT="$PWD/workspace"
mkdir -p workspace

# ---- 1. generation ---------------------------------------------------
write_config <<J
{"mcpServers":{"fix":{"command":"$FIX"}}}
J
./scripts/mcp_sync.sh > /tmp/mcp_sync1.$$ 2>&1 || fail "first sync failed: $(cat /tmp/mcp_sync1.$$)"
grep -q '+ fix__echo' /tmp/mcp_sync1.$$ || fail "sync did not report fix__echo as added"
grep -q '+ fix__add'  /tmp/mcp_sync1.$$ || fail "sync did not report fix__add as added"

m=actions/mcp/fix__echo/action.json
[ -f "$m" ] || fail "no manifest generated for fix__echo"
[ -x actions/mcp/fix__echo/run.sh ] || fail "run.sh is not executable"

check() { # check <jq filter> <expected> <label>
  local got; got="$(jq -r "$1" "$m")"
  [ "$got" = "$2" ] || fail "$3: expected '$2', got '$got'"
}
check '.id' 'fix__echo' 'id is <server>__<tool>'
check '.outside_world' 'true' 'outside_world'
check '.contract' 'managed_operation' 'contract'
check '.default_timeout_ms' '60000' 'default_timeout_ms'
check '.exec | join(" ")' 'bash run.sh' 'exec'
check '.description' 'MCP tool echo from server fix. Echo the text back.' 'description'
# The tool's own schema is carried through, not reinterpreted.
check '.args_schema.properties.text.type' 'string' 'inputSchema property copied'
check '.args_schema.properties.text.description' 'Text to echo.' 'property description copied'
check '.args_schema.additionalProperties' 'false' 'schema keyword preserved verbatim'
# op is FloofClaw's own managed-operation verb, added to the tool's schema.
check '.args_schema.properties.op.enum | join(",")' 'start' 'op enum'
[ "$(jq -r '.args_schema.required | sort | join(",")' "$m")" = "op,text" ] \
  || fail "required did not merge the tool's fields with op"
# run.sh names the server and tool and nothing else.
grep -q "_bridge.sh' fix' 'echo'" <<<"$(tr -s ' ' < actions/mcp/fix__echo/run.sh)" ||
  grep -q "fix' 'echo'" actions/mcp/fix__echo/run.sh || fail "run.sh does not pass server and tool"

# A description longer than the manifest convention is bounded, because the
# registry refuses a manifest whose description overruns.
[ "$(jq -r '.description | length' "$m")" -le 512 ] || fail "description exceeds 512"

# ---- 2. idempotence --------------------------------------------------
cp "$m" /tmp/mcp_before.$$
./scripts/mcp_sync.sh > /tmp/mcp_sync2.$$ 2>&1 || fail "second sync failed"
grep -q '(no change)' /tmp/mcp_sync2.$$ || fail "re-sync was not a no-op: $(cat /tmp/mcp_sync2.$$)"
cmp -s /tmp/mcp_before.$$ "$m" || fail "re-sync rewrote an unchanged manifest"

# --dry-run reports without touching the tree.
MCP_FIXTURE_MODE=notools ./scripts/mcp_sync.sh --dry-run > /tmp/mcp_dry.$$ 2>&1 ||
  fail "dry run failed"
grep -q '\- fix__echo' /tmp/mcp_dry.$$ || fail "dry run did not report the removal"
[ -f "$m" ] || fail "dry run deleted a manifest"

# ---- 3. stale tools disappear ---------------------------------------
MCP_FIXTURE_MODE=notools ./scripts/mcp_sync.sh > /tmp/mcp_sync3.$$ 2>&1 ||
  fail "empty-catalog sync failed"
grep -q '\- fix__echo' /tmp/mcp_sync3.$$ || fail "stale tool was not reported removed"
[ -d actions/mcp/fix__echo ] && fail "stale action directory survived the sync"
[ -f actions/mcp/_bridge.sh ] || fail "sync deleted the shared bridge"

# ---- 4. an unreachable server keeps its catalog ----------------------
./scripts/mcp_sync.sh >/dev/null 2>&1 || true   # restore the two tools
[ -d actions/mcp/fix__echo ] || fail "restore sync did not regenerate"
write_config <<'J'
{"mcpServers":{"fix":{"command":"tests/fixtures/mcp/no_such_server_binary"}}}
J
if ./scripts/mcp_sync.sh > /tmp/mcp_sync4.$$ 2>&1; then
  fail "sync reported success for a server that never answered"
fi
grep -q 'did not answer' /tmp/mcp_sync4.$$ || fail "sync did not name the unreachable server"
[ -d actions/mcp/fix__echo ] ||
  fail "a transient server outage deleted the deployment's catalog"

# A hung server is bounded rather than waited on forever.
write_config <<J
{"mcpServers":{"fix":{"command":"env","args":["MCP_FIXTURE_MODE=hang","$FIX"]}}}
J
start=$(date +%s)
./scripts/mcp_sync.sh >/dev/null 2>&1 || true
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -le 12 ] || fail "sync waited ${elapsed}s on a hung server"

# ---- 5. bridge round-trip and failure modes -------------------------
write_config <<J
{"mcpServers":{"fix":{"command":"$FIX"}}}
J
./scripts/mcp_sync.sh >/dev/null 2>&1 || fail "sync failed before bridge checks"

out="$(envelope '{"op":"start","text":"MCP_ROUNDTRIP"}' | ./actions/mcp/fix__echo/run.sh)" ||
  fail "a good call exited nonzero"
[ "$(jq -r '.result.text' <<<"$out")" = "MCP_ROUNDTRIP" ] ||
  fail "round-trip did not return the tool's content: $out"
[ "$(jq -r '.error' <<<"$out")" = "null" ] || fail "a good call reported an error"

# Arguments reach the tool as the tool declared them, and op does not.
out="$(envelope '{"op":"start","a":2,"b":3}' | ./actions/mcp/fix__add/run.sh)"
[ "$(jq -r '.result.text' <<<"$out")" = "5" ] || fail "arguments did not reach the tool: $out"

# A bare args object is not the contract, and is refused as such.
out="$(printf '{"op":"start","text":"x"}' | ./actions/mcp/fix__echo/run.sh)" && rc=0 || rc=$?
[ "$rc" = 1 ] || fail "a non-envelope stdin was accepted"
[ "$(jq -r '.error.code' <<<"$out")" = "BAD_ARGS" ] || fail "bad envelope wrong code: $out"

# The full JSON-RPC reply is kept as the call's artifact.
rm -f workspace/runs/run_test/action_calls/req_test.body.json
envelope '{"op":"start","text":"ARTIFACT_PROOF"}' | ./actions/mcp/fix__echo/run.sh >/dev/null
jq -e '.result.content[0].text == "ARTIFACT_PROOF"' \
  workspace/runs/run_test/action_calls/req_test.body.json >/dev/null ||
  fail "the complete reply was not written to the call artifact"

# isError is this tool failing, reported as this action failing.
out="$(envelope '{"op":"start","text":"x"}' | MCP_FIXTURE_MODE=error ./actions/mcp/fix__echo/run.sh)" && rc=0 || rc=$?
[ "$rc" = 1 ] || fail "isError did not fail the call"
grep -q 'the tool refused' <<<"$out" || fail "isError message was lost: $out"
[ -n "$(jq -r '.error.message' <<<"$out")" ] || fail "isError produced no result JSON on stdout"

# A JSON-RPC error, a server that speaks nonsense, and a dead command each
# fail loudly with usable JSON rather than silence.
for mode in garbage dead unknown_tool; do
  case "$mode" in
    garbage) out="$(envelope '{"op":"start","text":"x"}' | MCP_FIXTURE_MODE=garbage ./actions/mcp/fix__echo/run.sh)" && rc=0 || rc=$? ;;
    dead)    write_config <<'J'
{"mcpServers":{"fix":{"command":"tests/fixtures/mcp/no_such_server_binary"}}}
J
             out="$(envelope '{"op":"start","text":"x"}' | ./actions/mcp/fix__echo/run.sh)" && rc=0 || rc=$?
             write_config <<J
{"mcpServers":{"fix":{"command":"$FIX"}}}
J
             ;;
    unknown_tool) out="$(envelope '{"op":"start"}' | ./actions/mcp/_bridge.sh fix nosuch)" && rc=0 || rc=$? ;;
  esac
  [ "$rc" = 1 ] || fail "$mode did not fail the call (rc=$rc)"
  jq -e '.error.message | length > 0' <<<"$out" >/dev/null ||
    fail "$mode produced no error JSON on stdout: $out"
done

# A hung server fails inside the bridge's own bound, well under the
# action deadline, so the jobrunner never has to kill it mid-write.
start=$(date +%s)
out="$(envelope '{"op":"start","text":"x"}' | MCP_FIXTURE_MODE=hang ./actions/mcp/fix__echo/run.sh)" && rc=0 || rc=$?
elapsed=$(( $(date +%s) - start ))
[ "$rc" = 1 ] || fail "a hung server did not fail the call"
[ "$elapsed" -le 10 ] || fail "the bridge waited ${elapsed}s on a hung server"
grep -q 'did not answer within' <<<"$out" || fail "hang was not reported as a timeout: $out"

# ---- 6. HTTP transport ----------------------------------------------
port="$(./bin/fclaw internal free-port)"
python3 - "$port" <<'PY' &
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
port = int(sys.argv[1])
TOOLS = [{"name": "ping", "description": "Reply pong.",
          "inputSchema": {"type": "object", "properties": {}}}]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        m = req.get("method")
        if m == "initialize":
            r = {"protocolVersion": "2024-11-05", "capabilities": {}}
        elif m == "tools/list":
            r = {"tools": TOOLS}
        else:
            r = {"content": [{"type": "text", "text": "pong"}]}
        body = json.dumps({"jsonrpc": "2.0", "id": req.get("id"), "result": r}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
HTTPServer(("127.0.0.1", port), H).serve_forever()
PY
http_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
  curl -sS -m 1 -X POST --data '{"jsonrpc":"2.0","id":1,"method":"initialize"}' \
    "http://127.0.0.1:$port" >/dev/null 2>&1 && break
  sleep 0.3
done
write_config <<J
{"mcpServers":{"fix":{"command":"$FIX"},"web":{"url":"http://127.0.0.1:$port"}}}
J
./scripts/mcp_sync.sh >/dev/null 2>&1 || fail "sync over http failed"
[ -f actions/mcp/web__ping/action.json ] || fail "http server produced no action"
out="$(envelope '{"op":"start"}' | ./actions/mcp/web__ping/run.sh)" || fail "http call failed"
[ "$(jq -r '.result.text' <<<"$out")" = "pong" ] || fail "http round-trip wrong: $out"
kill "$http_pid" 2>/dev/null; http_pid=""

# ---- 7. hostile names are refused, not executed ---------------------
write_config <<J
{"mcpServers":{"fix":{"command":"$FIX"},"bad name":{"command":"$FIX"}}}
J
if ./scripts/mcp_sync.sh >/tmp/mcp_bad.$$ 2>&1; then
  fail "a server name with a space was accepted"
fi
grep -q 'is not \[A-Za-z0-9_.-\]' /tmp/mcp_bad.$$ || fail "the refusal does not name the grammar"
[ -d "actions/mcp/bad name__echo" ] && fail "an unsafe server name reached the filesystem"

# ---- 8. secrets ------------------------------------------------------
write_config <<J
{"mcpServers":{"fix":{"command":"$FIX","env":{"MCP_FIXTURE_SECRET":"\${FIX_TOKEN}"}}}}
J
./scripts/mcp_sync.sh >/dev/null 2>&1 || fail "sync with an env reference failed"

# Unset: the call fails and names what is missing, rather than calling the
# server with an empty credential.
out="$(envelope '{"op":"start","text":"x"}' | ./actions/mcp/fix__echo/run.sh)" && rc=0 || rc=$?
[ "$rc" = 1 ] || fail "a missing credential did not fail the call"
grep -q 'FIX_TOKEN' <<<"$out" || fail "the refusal does not name the missing variable: $out"

printf %s 'SUPER_SECRET_MCP_VALUE' | ./scripts/mcp_secret.sh fix FIX_TOKEN >/dev/null ||
  fail "storing the server credential failed"
# The store now holds one copy per generated action, and only per action.
./bin/fclaw auth get -a "action:fix__echo:fix_token" >/dev/null 2>&1 ||
  fail "the credential was not stored for fix__echo"
./bin/fclaw auth get -a "action:fix__add:fix_token" >/dev/null 2>&1 ||
  fail "the credential was not stored for fix__add"
grep -q 'SUPER_SECRET_MCP_VALUE' config/mcp.json &&
  fail "the credential leaked into config/mcp.json"
grep -rq 'SUPER_SECRET_MCP_VALUE' actions/mcp/ &&
  fail "the credential leaked into a generated action"

# ---- 9. end to end, as an ordinary managed operation ----------------
export FCLAW_MODEL_PROFILES="$PWD/tests/fixtures/smoke/model_profiles_mock.json"
export FCLAW_GATEWAY_POLL_MS=5
export FCLAW_GATEWAY_DISABLE_CHANNELS=1
cat > config/floofclaw_config.json <<'CFG'
{ "bot_name": "FloofClaw", "default_floop": "hello", "force_disable": [] }
CFG
./bin/fclaw clear -a --yes >/dev/null
mkdir -p workspace/fixtures
cat > workspace/fixtures/mcp_start.json <<'J'
{"calls":[{"name":"fix__echo","args":{"op":"start","text":"MCP_ROUNDTRIP_E2E"}}]}
J
cat > workspace/fixtures/mcp_reply.json <<'J'
{"calls":[{"name":"message","args":{"message":"MCP_E2E_REPLY"}},{"name":"task.update","args":{"task_id":"task_run_001_000002","state":"completed"}}]}
J
export LLM_MOCK_RESPONSE_PATHS="$PWD/workspace/fixtures/mcp_start.json:$PWD/workspace/fixtures/mcp_reply.json:$PWD/workspace/fixtures/mcp_reply.json"
./bin/fclaw gateway start -a --floop openclaw >/dev/null
gateway_running=1
out="$(./bin/fclaw run --floop openclaw --text 'call the mcp echo tool')" || true
printf '%s\n' "$out"
grep -q 'MCP_E2E_REPLY' <<<"$out" || fail "the run did not reach the reply after the tool call"
./bin/fclaw gateway stop -a >/dev/null 2>&1; gateway_running=0

grep -Rq '"type":"operation_result"' workspace/runs/*/event_log.jsonl ||
  fail "the generated action did not complete as a managed operation"
grep -Rq 'MCP_ROUNDTRIP_E2E' workspace/runs/*/event_log.jsonl ||
  fail "the tool's content never reached the run"
# The credential was injected into the child — the fixture echoes it back —
# and it is nowhere in what the run wrote down.
grep -Rq 'secret=SUPER_SECRET_MCP_VALUE' workspace/runs/*/event_log.jsonl ||
  fail "the action secret was not injected into the MCP server"
if grep -Rl 'SUPER_SECRET_MCP_VALUE' workspace/runs/*/action_calls/ >/dev/null 2>&1; then
  # The fixture deliberately echoes it into the RESULT; that is the proof of
  # injection. What must never appear is the credential as stored input.
  grep -Rh 'SUPER_SECRET_MCP_VALUE' workspace/runs/*/action_calls/* |
    grep -qv 'secret=SUPER_SECRET_MCP_VALUE' &&
    fail "the credential appeared in an action artifact as input"
fi
ls workspace/runs/*/action_calls/*fix__echo* >/dev/null 2>&1 ||
  ls workspace/runs/*/action_calls/ >/dev/null 2>&1 ||
  fail "the generated action left no artifact"

# ---- 10. force_disable masks a generated action ---------------------
cat > config/floofclaw_config.json <<'CFG'
{ "bot_name": "FloofClaw", "default_floop": "hello", "force_disable": ["fix__echo"] }
CFG
./bin/fclaw clear -a --yes >/dev/null
export LLM_MOCK_RESPONSE_PATHS="$PWD/workspace/fixtures/mcp_start.json:$PWD/workspace/fixtures/mcp_reply.json:$PWD/workspace/fixtures/mcp_reply.json"
./bin/fclaw gateway start -a --floop openclaw >/dev/null
gateway_running=1
./bin/fclaw run --floop openclaw --text 'call the masked mcp tool' >/dev/null 2>&1 || true
./bin/fclaw gateway stop -a >/dev/null 2>&1; gateway_running=0
grep -Rq 'force_disabled' workspace/runs/*/event_log.jsonl ||
  fail "a masked generated action was not rejected as force_disabled"
# The request is still recorded — the mask is a refusal, not amnesia — so the
# proof that nothing ran is that the action never started.
# Scoped to this action: the reply's own builtin calls succeed normally.
if grep -Rq '"type":"action_succeeded".*"action":"fix__echo"' workspace/runs/*/event_log.jsonl; then
  fail "a masked generated action still reached the MCP server"
fi
[ -z "$(find workspace/runs -name '*.body.json' 2>/dev/null)" ] ||
  fail "a masked generated action still produced a server reply artifact"

# ---- 11. args are validated against the copied schema ---------------
cat > config/floofclaw_config.json <<'CFG'
{ "bot_name": "FloofClaw", "default_floop": "hello", "force_disable": [] }
CFG
./bin/fclaw clear -a --yes >/dev/null
cat > workspace/fixtures/mcp_badargs.json <<'J'
{"calls":[{"name":"fix__echo","args":{"op":"start"}}]}
J
export LLM_MOCK_RESPONSE_PATHS="$PWD/workspace/fixtures/mcp_badargs.json:$PWD/workspace/fixtures/mcp_reply.json:$PWD/workspace/fixtures/mcp_reply.json"
./bin/fclaw gateway start -a --floop openclaw >/dev/null
gateway_running=1
./bin/fclaw run --floop openclaw --text 'call echo with no text' >/dev/null 2>&1 || true
./bin/fclaw gateway stop -a >/dev/null 2>&1; gateway_running=0
grep -Rq 'Missing required property' workspace/runs/*/event_log.jsonl ||
  fail "the tool's own required field was not enforced from the copied schema"

# ---- 12. the CLI verb --------------------------------------------------
# `fclaw mcp` wraps the scripts so the operator entry point obeys the same
# output contract as every other command. It must not become a second
# implementation.
write_config <<J
{"mcpServers":{"fix":{"command":"$FIX"}}}
J
./bin/fclaw mcp sync -h > /tmp/mcp_cli_h.$$ 2>&1 || fail "fclaw mcp sync -h failed"
grep -q 'mcp-sync:' /tmp/mcp_cli_h.$$ || fail "human mode is not the script's operator text"

# Agent mode is the default and is one JSON object, per the output contract.
./bin/fclaw mcp sync > /tmp/mcp_cli_a.$$ 2>/dev/null || fail "fclaw mcp sync failed"
[ "$(wc -l < /tmp/mcp_cli_a.$$)" -eq 1 ] || fail "agent mode emitted more than one object"
jq -e '.ok == true and .tools >= 2 and (.added | type) == "array" and .applied == true' \
  /tmp/mcp_cli_a.$$ >/dev/null || fail "agent mode object is wrong: $(cat /tmp/mcp_cli_a.$$)"
jq -e '.dry_run == false' /tmp/mcp_cli_a.$$ >/dev/null || fail "applied run claimed dry_run"

./bin/fclaw mcp sync --dry-run > /tmp/mcp_cli_d.$$ 2>/dev/null || fail "dry run through the verb failed"
jq -e '.dry_run == true and .applied == false' /tmp/mcp_cli_d.$$ >/dev/null ||
  fail "dry run did not report itself: $(cat /tmp/mcp_cli_d.$$)"

# Both output modes at once is an error everywhere else, and here too.
./bin/fclaw mcp sync -a -h >/dev/null 2>&1 && fail "-a -h together was accepted"
./bin/fclaw mcp sync --bogus >/dev/null 2>&1 && fail "an unknown flag was accepted"
./bin/fclaw mcp nosuch >/dev/null 2>&1 && fail "an unknown subcommand was accepted"

# An unreachable server is a nonzero exit and a machine-readable reason.
write_config <<'J'
{"mcpServers":{"fix":{"command":"tests/fixtures/mcp/no_such_server_binary"}}}
J
./bin/fclaw mcp sync > /tmp/mcp_cli_f.$$ 2>/dev/null && fail "the verb hid an unreachable server"
jq -e '.ok == false and (.unreachable | index("fix"))' /tmp/mcp_cli_f.$$ >/dev/null ||
  fail "the verb did not name the unreachable server: $(cat /tmp/mcp_cli_f.$$)"
write_config <<J
{"mcpServers":{"fix":{"command":"$FIX"}}}
J
./bin/fclaw mcp sync >/dev/null 2>&1

# The credential path is reachable the same way, and stays off argv.
printf %s 'VERB_SECRET_VALUE' | ./bin/fclaw mcp secret fix FIX_TOKEN > /tmp/mcp_cli_s.$$ 2>&1 ||
  fail "fclaw mcp secret failed"
jq -e '.ok == true and .actions >= 2 and .env == "FIX_TOKEN"' /tmp/mcp_cli_s.$$ >/dev/null ||
  fail "mcp secret agent output is wrong: $(cat /tmp/mcp_cli_s.$$)"

# ---- 13. the kernel never learns the protocol -------------------------
# The verb is a wrapper, so the binary contains the word. What it must never
# contain is the protocol: no method names, no transport, no tool handling.
if grep -rniEq 'jsonrpc|tools/(call|list)|notifications/initialized|mcpServers' \
     runtime/ --include=*.c --include=*.h; then
  grep -rniE 'jsonrpc|tools/(call|list)|notifications/initialized|mcpServers' \
    runtime/ --include=*.c --include=*.h | head -5 >&2
  fail "the MCP protocol reached the runtime; generation must keep it out"
fi
# The one file allowed to name MCP is the wrapper, and only to find the
# scripts and print usage.
stray="$(grep -rlniE '\bmcp' runtime/ --include=*.c --include=*.h |
         grep -v 'runtime/mcp_cli.c' | grep -v 'runtime/main.c' || true)"
[ -z "$stray" ] || fail "MCP leaked past the CLI wrapper into: $stray"
# And the wrapper only ever hands off to the scripts.
grep -q 'scripts/mcp_sync.sh' runtime/mcp_cli.c ||
  fail "the wrapper does not defer to the sync script"

# Nothing but an operator runs a sync: not the build, not the gateway.
if grep -rq 'mcp_sync' Makefile runtime/gateway/; then
  fail "something other than the operator can trigger a sync"
fi

rm -f /tmp/mcp_*.$$
printf 'mcp generation: PASS\n'
