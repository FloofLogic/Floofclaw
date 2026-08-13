#!/usr/bin/env bash
set -euo pipefail
input="$(cat)"
python3 - "$input" <<'PY'
import json, os, subprocess, sys
req = json.loads(sys.argv[1])
cmd = req.get("args", {}).get("cmd", "")
try:
    workspace_root = os.environ.get("FCLAW_WORKSPACE_ROOT", "")
    if not workspace_root:
        print(json.dumps({"events": [], "result": {}, "error": {"message": "FCLAW_WORKSPACE_ROOT is required"}}))
        sys.exit(1)
    p = subprocess.run(cmd, shell=True, cwd=workspace_root, text=True, capture_output=True, timeout=30)
    print(json.dumps({"events": [], "result": {"stdout": p.stdout, "stderr": p.stderr, "exit_code": p.returncode, "text": p.stdout[:8000]}, "error": None if p.returncode == 0 else {"message": p.stderr or "command failed"}}))
    sys.exit(p.returncode)
except Exception as exc:
    print(json.dumps({"events": [], "result": {}, "error": {"message": str(exc)}}))
    sys.exit(1)
PY
