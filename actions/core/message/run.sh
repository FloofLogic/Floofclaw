#!/usr/bin/env bash
set -euo pipefail
input="$(cat)"
python3 - "$input" <<'PY'
import json, sys
req = json.loads(sys.argv[1])
message = req.get("args", {}).get("message", "")
print(json.dumps({"events": [], "result": {"message": message}, "error": None}))
PY
