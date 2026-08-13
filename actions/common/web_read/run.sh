#!/usr/bin/env bash
set -euo pipefail
# web_read as a managed-operation. Contract:
#   op:start(url) -> {status:"finished", handle, text:<bounded>, url, title, ...}
#   op:poll(handle) -> {status:"finished", handle, text:""} (never reached in
#     practice — start finishes synchronously)
#
# The underlying extractor is built from the public `pluck` submodule owned by
# this action. Public FloofClaw releases flatten that pinned source. This script
# bridges the FloofClaw managed-op contract to pluck.
input="$(cat)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUCK="$SCRIPT_DIR/_pluck/zig-out/bin/pluck"
python3 - "$input" "$PLUCK" <<'PY'
import json, subprocess, sys, os, secrets

req = json.loads(sys.argv[1])
pluck = sys.argv[2]
args = req.get("args", {}) or {}
op = (args.get("op") or "").strip()

def emit(result=None, error=None, code=0):
    print(json.dumps({"events": [], "result": result or {}, "error": error}))
    sys.exit(code)

def make_handle():
    return "webrd_" + secrets.token_hex(6)

if op == "poll":
    # Kept for contract completeness. start always finishes.
    handle = (args.get("handle") or "").strip() or make_handle()
    emit(result={
        "status": "finished",
        "handle": handle,
        "text": "web_read has no long-running state; call op:start."
    })

if op != "start":
    emit(error={"code": "BAD_ARGS",
                "message": "op must be 'start' or 'poll'"}, code=1)

url = args.get("url", "")
if not url:
    emit(error={"code": "BAD_ARGS",
                "message": "args.url is required with op:start"}, code=1)
if not os.access(pluck, os.X_OK):
    emit(error={"code": "PLUCK_MISSING",
                "message": ("pluck binary not built; from the FloofClaw "
                            "checkout root run: "
                            "bash actions/common/web_read/build.sh")},
         code=1)

try:
    p = subprocess.run([pluck, "--json", "--timeout", "20000",
                        "--max-bytes", "5000000", url],
                       capture_output=True, text=True, timeout=30)
except Exception as exc:
    emit(error={"code": "WEB_READ_FAILED", "message": str(exc)}, code=1)

if p.returncode != 0:
    emit(error={"code": "WEB_READ_FAILED",
                "message": (p.stderr or "pluck failed").strip()[:2000]}, code=1)

try:
    d = json.loads(p.stdout)
except Exception:
    # Fall back to raw text; still speak the contract.
    emit(result={
        "status": "finished",
        "handle": make_handle(),
        "text": p.stdout[:1800],
        "url": url,
    })

full = d.get("text_plain") or d.get("text_markdown") or d.get("excerpt") or ""
true_len = d.get("length") or len(full)

# Result JSON must fit RT_LARGE (4096); the driver's operation_result
# envelope also caps the text field at OP_TEXT_EXCERPT_MAX (~1800). So
# the actual LLM-facing budget for text is ~1800 chars. Trim aggressively
# and let the model call read_file with the excerpt+url if it wants more.
CEILING_TEXT = 1600     # leave headroom for JSON escaping + other fields
CEILING_TOTAL = 3500    # < RT_LARGE 4096
excerpt = (d.get("excerpt") or "")[:400]

def build(text, truncated):
    r = {
        "status": "finished",
        "handle": make_handle(),
        "text": text,
        "url": d.get("url") or url,
        "title": d.get("title"),
        "excerpt": excerpt,
        "length": true_len,
        "truncated": truncated,
    }
    if truncated:
        r["note"] = ("text is the readable head of the page (length is "
                     "the full size); this is enough to summarize or "
                     "quote — proceed, do not re-read the same url")
    return r

text = full[:CEILING_TEXT]
truncated = len(full) > CEILING_TEXT
while text and len(json.dumps(build(text, truncated))) > CEILING_TOTAL:
    text = text[: max(0, len(text) - 128)]
emit(result=build(text, truncated))
PY
