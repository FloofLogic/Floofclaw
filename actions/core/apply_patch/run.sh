#!/usr/bin/env bash
# Applies one patch inside the workspace. The deadline and the write
# confinement both come from actions/common/_lib/sandbox.sh so this script
# cannot disagree with the manifest the kernel enforces.
set -euo pipefail
lib=""
probe="$(cd "$(dirname "$0")" && pwd -P)"
while [ "$probe" != "/" ]; do
  if [ -f "$probe/common/_lib/sandbox.sh" ]; then lib="$probe/common/_lib/sandbox.sh"; break; fi
  probe="$(dirname "$probe")"
done
if [ -z "$lib" ]; then
  printf '%s\n' '{"events":[],"result":{},"error":{"message":"apply_patch could not locate actions/common/_lib/sandbox.sh"}}'
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
      "$(fclaw_sandbox_unavailable_message apply_patch)"
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

python3 - "$input" <<'PYEOF'
import json, os, subprocess, sys, tempfile
req = json.loads(sys.argv[1])
args = req.get("args", {})
patch_text = args.get("patch", "")
strip = int(args.get("strip", 1))
cwd = args.get("cwd", ".")
if not patch_text:
    print(json.dumps({"events": [], "result": {}, "error": {"message": "patch is required"}}))
    sys.exit(1)
workspace_root = os.environ.get("FCLAW_WORKSPACE_ROOT", "")
if not workspace_root:
    print(json.dumps({"events": [], "result": {}, "error": {"message": "FCLAW_WORKSPACE_ROOT is required"}}))
    sys.exit(1)
if os.path.isabs(cwd) or cwd.startswith("../") or "/../" in cwd:
    print(json.dumps({"events": [], "result": {}, "error": {"message": "cwd must be workspace-relative"}}))
    sys.exit(1)
cwd_path = os.path.join(workspace_root, cwd)
with tempfile.NamedTemporaryFile("w", suffix=".patch", delete=False) as f:
    f.write(patch_text)
    patch_path = f.name
prefix = [a for a in os.environ.get("FCLAW_SANDBOX_ARGV", "").split("\n") if a]
timeout = int(os.environ.get("FCLAW_CMD_TIMEOUT_S", "30"))
try:
    r = subprocess.run(prefix + ["patch", "-p" + str(strip), "-i", patch_path],
                       cwd=cwd_path, capture_output=True, text=True, timeout=timeout)
    print(json.dumps({
        "events": [],
        "result": {"stdout": r.stdout[:8192], "exit_code": r.returncode, "text": r.stdout[:8000]},
        "error": None if r.returncode == 0 else {"message": "patch exit " + str(r.returncode), "stderr": r.stderr[:8192]},
    }))
    sys.exit(r.returncode)
finally:
    os.unlink(patch_path)
PYEOF
