#!/usr/bin/env bash
set -euo pipefail
input="$(cat)"
python3 - "$input" <<'PY'
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
    r = subprocess.run(["curl", "-sSL", "--max-time", "30", "-w", "\n%{http_code}", url],
                       capture_output=True, text=True, timeout=35)
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
PY
