#!/usr/bin/env bash
# Fake `claude --print --output-format stream-json ...` driver for tests.
# Mirrors fake_codex.sh: writes a deterministic file when the prompt names
# a known target, prints stream-json events to stdout, and returns 0.
# Argv is parsed loosely — we only care about --add-dir and the trailing
# positional prompt after "--".
set -euo pipefail

cwd="$PWD"
prompt=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --add-dir)
      cwd="$2"
      shift 2
      ;;
    --)
      shift
      prompt="$*"
      break
      ;;
    *)
      shift
      ;;
  esac
done

if grep -qi 'claude_smoke\.md' <<<"$prompt"; then
  cat > "$cwd/claude_smoke.md" <<'MD'
hello from claude
MD
  printf '{"type":"system","subtype":"init"}\n'
  printf '{"type":"assistant","message":{"content":[{"type":"text","text":"Wrote claude_smoke.md."}]}}\n'
  printf '{"type":"result","result":"Wrote claude_smoke.md."}\n'
elif grep -qi 'silent_claude\.md' <<<"$prompt"; then
  # Silent-quiesce reproducer for claude: succeed without writing the file.
  printf '{"type":"system","subtype":"init"}\n'
  printf '{"type":"result","result":"Claude finished."}\n'
else
  printf '{"type":"result","result":"Claude finished."}\n'
fi
