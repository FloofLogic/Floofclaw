#!/usr/bin/env bash
set -euo pipefail
input="$(cat)"
python3 - "$input" <<'PY'
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
try:
    r = subprocess.run(["patch", "-p" + str(strip), "-i", patch_path],
                       cwd=cwd_path, capture_output=True, text=True, timeout=30)
    print(json.dumps({
        "events": [],
        "result": {"stdout": r.stdout[:8192], "exit_code": r.returncode, "text": r.stdout[:8000]},
        "error": None if r.returncode == 0 else {"message": "patch exit " + str(r.returncode), "stderr": r.stderr[:8192]},
    }))
    sys.exit(r.returncode)
finally:
    os.unlink(patch_path)
PY
