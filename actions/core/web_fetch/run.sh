#!/usr/bin/env bash
# Fetches one URL. curl needs the network, which the sandbox allows; what
# it does not need is the ability to write anywhere but the workspace.
# The deadline comes from the manifest, not a constant in this file.
set -euo pipefail
lib=""
probe="$(cd "$(dirname "$0")" && pwd -P)"
while [ "$probe" != "/" ]; do
  if [ -f "$probe/common/_lib/sandbox.sh" ]; then lib="$probe/common/_lib/sandbox.sh"; break; fi
  probe="$(dirname "$probe")"
done
if [ -z "$lib" ]; then
  printf '%s\n' '{"events":[],"result":{},"error":{"message":"web_fetch could not locate actions/common/_lib/sandbox.sh"}}'
  exit 1
fi
# shellcheck source=/dev/null
. "$lib"

input="$(cat)"
workspace_root="${FCLAW_WORKSPACE_ROOT:-}"
if [ -z "$workspace_root" ]; then
  printf '%s\n' '{"events":[],"result":{},"error":{"message":"FCLAW_WORKSPACE_ROOT is required"}}'
  exit 1
fi
sandbox_argv=""
if [ "$(fclaw_sandbox_mode)" = "on" ]; then
  if ! sandbox_argv="$(fclaw_sandbox_argv "$workspace_root")"; then
    python3 -c 'import json,sys; print(json.dumps({"events":[],"result":{},"error":{"message":sys.argv[1]}}))' \
      "$(fclaw_sandbox_unavailable_message web_fetch)"
    exit 1
  fi
fi
export FCLAW_SANDBOX_ARGV="$sandbox_argv"
export FCLAW_CMD_TIMEOUT_S="$(fclaw_action_timeout_s 40 5)"

scratch="$(fclaw_sandbox_scratch "$workspace_root")" || {
  printf '%s\n' '{"events":[],"result":{},"error":{"message":"could not create the confined scratch directory"}}'
  exit 1
}
export TMPDIR="$scratch" TMP="$scratch" TEMP="$scratch"
cleanup() { [ -n "${scratch:-}" ] && rm -rf "$scratch"; return 0; }
trap cleanup EXIT

python3 - "$input" <<'PYEOF'
import json, os, sys, subprocess
req = json.loads(sys.argv[1])
args = req.get("args", {})
url = args.get("url", "")
if not url.startswith(("http://", "https://")):
    print(json.dumps({"events": [], "result": {}, "error": {"message": "url must be http(s)://"}}))
    sys.exit(1)
try:
    artifacts = req.get("artifacts", {}) if isinstance(req.get("artifacts", {}), dict) else {}
    workspace_root = os.environ.get("FCLAW_WORKSPACE_ROOT", "")
    body_artifact = artifacts.get("body", "")
    if not workspace_root:
        print(json.dumps({"events": [], "result": {}, "error": {"message": "FCLAW_WORKSPACE_ROOT is required"}}))
        sys.exit(1)
    if not body_artifact or os.path.isabs(body_artifact) or body_artifact.startswith("../") or "/../" in body_artifact:
        print(json.dumps({"events": [], "result": {}, "error": {"message": "artifacts.body must be a safe workspace-relative path"}}))
        sys.exit(1)
    body_path = os.path.join(workspace_root, body_artifact)
    prefix = [a for a in os.environ.get("FCLAW_SANDBOX_ARGV", "").split("\n") if a]
    budget = int(os.environ.get("FCLAW_CMD_TIMEOUT_S", "40"))
    # curl gets the smaller share so its own timeout reports a clean HTTP
    # failure before the wrapper's kills the process.
    r = subprocess.run(prefix + ["curl", "-sSL", "--max-time", str(max(budget - 5, 1)),
                                 "-w", "\n%{http_code}", url],
                       capture_output=True, text=True, timeout=budget)
    out = r.stdout
    last_nl = out.rfind("\n")
    if last_nl >= 0:
        body = out[:last_nl]
        http_status = out[last_nl+1:].strip()
    else:
        body = out; http_status = "0"
    os.makedirs(os.path.dirname(body_path), exist_ok=True)
    with open(body_path, "w", encoding="utf-8") as f:
        f.write(body)
    body_bytes = len(body.encode("utf-8", errors="replace"))
    body_excerpt = body[:1200]
    print(json.dumps({
        "events": [],
        "result": {
            "url": url,
            "http_status": http_status,
            "body_artifact": body_artifact,
            "body_excerpt": body_excerpt,
            "body_bytes": body_bytes,
            "truncated": False,
            "stderr_excerpt": r.stderr[:1000],
            "text": "HTTP " + http_status + " from " + url + "\n" + body_excerpt,
        },
        "error": None if r.returncode == 0 else {"message": "curl exit " + str(r.returncode), "stderr_excerpt": r.stderr[:1000]},
    }))
    sys.exit(r.returncode)
except Exception as e:
    print(json.dumps({"events": [], "result": {}, "error": {"message": str(e)}}))
    sys.exit(1)
PYEOF
