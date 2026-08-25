#!/usr/bin/env bash
# Runs one shell command in the workspace root.
#
# Two things this script deliberately does not decide for itself: how long
# it may run (the manifest's timeout_ms is what the kernel kills the job
# at, and it arrives as FCLAW_ACTION_TIMEOUT_MS) and whether it may write
# outside the workspace (it may not, unless the operator says so). Both
# live in actions/common/_lib/sandbox.sh so every executing action answers
# them the same way.
set -euo pipefail

lib=""
probe="$(cd "$(dirname "$0")" && pwd -P)"
while [ "$probe" != "/" ]; do
  if [ -f "$probe/common/_lib/sandbox.sh" ]; then lib="$probe/common/_lib/sandbox.sh"; break; fi
  probe="$(dirname "$probe")"
done
if [ -z "$lib" ]; then
  printf '%s\n' '{"events":[],"result":{},"error":{"message":"bash action could not locate actions/common/_lib/sandbox.sh"}}'
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
      "$(fclaw_sandbox_unavailable_message bash)"
    exit 1
  fi
fi
export FCLAW_SANDBOX_ARGV="$sandbox_argv"
export FCLAW_CMD_TIMEOUT_S="$(fclaw_action_timeout_s 30 5)"

scratch="$(fclaw_sandbox_scratch "$workspace_root")" || {
  printf '%s\n' '{"events":[],"result":{},"error":{"message":"could not create the confined scratch directory"}}'
  exit 1
}
export TMPDIR="$scratch" TMP="$scratch" TEMP="$scratch"
cleanup() { [ -n "${scratch:-}" ] && rm -rf "$scratch"; return 0; }
trap cleanup EXIT


python3 - "$input" <<'PY'
import json, os, subprocess, sys
req = json.loads(sys.argv[1])
cmd = req.get("args", {}).get("cmd", "")
try:
    workspace_root = os.environ["FCLAW_WORKSPACE_ROOT"]
    prefix = [a for a in os.environ.get("FCLAW_SANDBOX_ARGV", "").split("\n") if a]
    timeout = int(os.environ.get("FCLAW_CMD_TIMEOUT_S", "30"))
    p = subprocess.run(prefix + ["/bin/sh", "-c", cmd], cwd=workspace_root,
                       text=True, capture_output=True, timeout=timeout)
    print(json.dumps({"events": [], "result": {"stdout": p.stdout, "stderr": p.stderr, "exit_code": p.returncode, "text": p.stdout[:8000]}, "error": None if p.returncode == 0 else {"message": p.stderr or "command failed"}}))
    sys.exit(p.returncode)
except subprocess.TimeoutExpired:
    print(json.dumps({"events": [], "result": {}, "error": {"message": "command exceeded the %ss action deadline" % os.environ.get("FCLAW_CMD_TIMEOUT_S", "30")}}))
    sys.exit(1)
except Exception as exc:
    print(json.dumps({"events": [], "result": {}, "error": {"message": str(exc)}}))
    sys.exit(1)
PY
