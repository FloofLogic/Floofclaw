#!/usr/bin/env bash
# Action sandbox and deadline contract.
#
# Two defects, one file. The executing actions ran unconfined as the
# gateway user — `bash` could write anywhere the process could reach — and
# each script enforced its own hardcoded timeout beside a manifest that
# declared a different one, with the managed-operation window a further ten
# times larger than either.
#
# This gate pins both: writes are confined to the workspace and nowhere
# else — scratch space is a TMPDIR inside it, and the per-user Darwin temp
# directory that a bare macOS mktemp(1) reaches for is denied, since
# allowing it would void the confinement for any deployment installed
# there — the confinement fails closed when no mechanism exists, the
# operator opt-out is the only way past it, and the command's own budget
# is derived from the manifest deadline the kernel kills the job at rather
# than a constant in the script.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

if [[ "${FCLAW_TEST_ISOLATED:-0}" != "1" || ! -f .fclaw-test-isolated ]]; then
  printf '%s\n' 'action sandbox: refusing to run outside an isolated copy' >&2
  exit 2
fi

fail() { printf 'action sandbox: %s\n' "$*" >&2; exit 1; }

WS="$ROOT/workspace/sandbox_probe"
rm -rf "$WS"
mkdir -p "$WS"
export FCLAW_WORKSPACE_ROOT="$WS"

BASH_ACTION="$ROOT/actions/core/bash/run.sh"
[[ -x "$BASH_ACTION" ]] || fail "actions/core/bash/run.sh is missing or not executable"

run_bash_action() { # run_bash_action <cmd>  -> the action's JSON on stdout
  python3 -c 'import json,sys; print(json.dumps({"request_id":"actionreq_probe","args":{"op":"start","cmd":sys.argv[1]}}))' "$1" \
    | "$BASH_ACTION" 2>/dev/null || true
}

field() { python3 -c '
import json,sys
try: d = json.loads(sys.stdin.read() or "{}")
except Exception: d = {}
cur = d
for k in sys.argv[1].split("."):
    cur = cur.get(k) if isinstance(cur, dict) else None
print("" if cur is None else cur)' "$1"; }

# ---------------------------------------------------------------- 1
# Every manifest agrees with itself: the job deadline is the smallest
# number, and the managed-operation window is a backstop above it rather
# than an unrelated order of magnitude.
for manifest in actions/core/bash/action.json actions/core/apply_patch/action.json \
                actions/core/web_fetch/action.json actions/common/http_call/action.json; do
  read -r job def max < <(jq -r '[.timeout_ms, .default_timeout_ms, .max_timeout_ms] | @tsv' "$manifest")
  [[ "$job" =~ ^[0-9]+$ && "$def" =~ ^[0-9]+$ && "$max" =~ ^[0-9]+$ ]] ||
    fail "$manifest does not declare all three timeouts"
  (( def > job )) || fail "$manifest: default_timeout_ms ($def) must exceed timeout_ms ($job) so the job's own terminal wins"
  (( max >= def )) || fail "$manifest: max_timeout_ms ($max) is below default_timeout_ms ($def)"
  (( def <= job * 4 )) || fail "$manifest: the managed-operation window ($def) is disconnected from the job deadline ($job)"
  grep -q 'FCLAW_ACTION_TIMEOUT_MS' "$(dirname "$manifest")/run.sh" ||
    grep -q 'fclaw_action_timeout_s' "$(dirname "$manifest")/run.sh" ||
    fail "$(dirname "$manifest")/run.sh still hardcodes its own deadline"
done

# ---------------------------------------------------------------- 2
# The kernel is the one source of the deadline: the script's budget tracks
# FCLAW_ACTION_TIMEOUT_MS, not a constant of its own.
out="$(FCLAW_ACTION_TIMEOUT_MS=8000 run_bash_action 'sleep 20')"
[[ "$(field error.message)" == *"exceeded"* ]] <<<"$out" ||
  fail "a command past the manifest deadline did not report a clean timeout: $out"
grep -q '3s action deadline' <<<"$out" ||
  fail "the reported deadline is not derived from FCLAW_ACTION_TIMEOUT_MS: $out"

out="$(FCLAW_ACTION_TIMEOUT_MS=30000 run_bash_action 'sleep 1 && echo lived')"
[[ "$(field result.exit_code <<<"$out")" == "0" ]] ||
  fail "a command well inside the deadline was cut short: $out"

# ---------------------------------------------------------------- 3
# Confinement. Writes land in the workspace and nowhere that matters.
export FCLAW_ACTION_TIMEOUT_MS=30000
out="$(run_bash_action 'echo inside > confined.txt && echo ok')"
[[ "$(field result.exit_code <<<"$out")" == "0" ]] || fail "a workspace write was refused: $out"
[[ -f "$WS/confined.txt" ]] || fail "the workspace write did not land"

probe_denied() { # probe_denied <label> <path>
  local out
  out="$(run_bash_action "echo escaped > $2")"
  [[ "$(field result.exit_code <<<"$out")" != "0" ]] ||
    fail "a sandboxed command wrote outside the workspace ($1: $2)"
  [[ ! -e "$2" ]] || { rm -f "$2"; fail "$1 was created despite the sandbox ($2)"; }
}
probe_denied "the deployment tree" "$ROOT/SANDBOX_ESCAPE"
probe_denied "the operator's home" "$HOME/.fclaw_sandbox_escape"
probe_denied "shared temp" "/tmp/fclaw_sandbox_escape.$$"

# Scratch space exists, and it is inside the confinement rather than an
# exception to it: TMPDIR points into the workspace for the life of the
# call. Anything that honours TMPDIR — python's tempfile, `patch`, most
# build tools — therefore works unchanged.
out="$(run_bash_action 'printf %s "$TMPDIR"')"
tmpdir="$(field result.stdout <<<"$out")"
[[ -n "$tmpdir" ]] || fail "no confined TMPDIR was provided: $out"
[[ "$tmpdir" == "$WS"/* ]] || fail "TMPDIR points outside the workspace: $tmpdir"
out="$(run_bash_action 'echo scratch > "$TMPDIR/probe" && wc -c < "$TMPDIR/probe"')"
[[ "$(field result.exit_code <<<"$out")" == "0" ]] ||
  fail "a sandboxed command could not use its confined TMPDIR: $out"

# And the cost, stated rather than papered over: macOS mktemp(1) with no
# template ignores TMPDIR and goes to the per-user Darwin temp directory,
# which is denied. Allowing that directory would void the confinement for
# any deployment installed under it — this isolated test copy is.
if [[ "$(uname -s)" == "Darwin" ]]; then
  out="$(run_bash_action 't=$(mktemp) && echo scratch > "$t"')"
  [[ "$(field result.exit_code <<<"$out")" != "0" ]] ||
    fail "bare mktemp escaped the confinement on macOS: $out"
fi

# The scratch directory does not outlive the call.
[[ ! -d "$WS/.fclaw/tmp" ]] || [[ -z "$(ls -A "$WS/.fclaw/tmp")" ]] ||
  fail "the confined scratch directory was left behind: $(ls -A "$WS/.fclaw/tmp")"

# Reading outside the workspace stays allowed: this confines writes, not
# knowledge, and every action needs its interpreter and libraries.
out="$(run_bash_action 'head -c 3 /etc/hosts > /dev/null && echo readok')"
[[ "$(field result.exit_code <<<"$out")" == "0" ]] || fail "reads outside the workspace were refused: $out"

# ---------------------------------------------------------------- 4
# Fails closed. With no mechanism on PATH the action refuses rather than
# running unconfined, and says exactly how to proceed.
fakebin="$ROOT/workspace/sandbox_probe_bin"
rm -rf "$fakebin"; mkdir -p "$fakebin"
for tool in bash sh cat echo python3 head printf rm mktemp wc uname dirname basename env getconf; do
  src="$(command -v "$tool" || true)"
  [[ -n "$src" ]] && ln -sf "$src" "$fakebin/$tool"
done
out="$(python3 -c 'import json;print(json.dumps({"request_id":"r","args":{"op":"start","cmd":"echo hi"}}))' \
       | PATH="$fakebin" bash "$BASH_ACTION" 2>/dev/null || true)"
[[ "$(field error.message <<<"$out")" == *"no action sandbox is available"* ]] ||
  fail "a host with no sandbox mechanism did not fail closed: $out"
[[ "$(field error.message <<<"$out")" == *"actions.bash.sandbox"* ]] ||
  fail "the refusal does not name the operator opt-out: $out"

# ---------------------------------------------------------------- 5
# The opt-out is deliberate, operator-owned, and the only way past it.
out="$(SANDBOX=off run_bash_action "echo escaped > $ROOT/SANDBOX_OPTOUT")"
[[ "$(field result.exit_code <<<"$out")" == "0" ]] || fail "the operator opt-out did not disable confinement: $out"
[[ -f "$ROOT/SANDBOX_OPTOUT" ]] || fail "the opt-out run did not actually write outside the workspace"
rm -f "$ROOT/SANDBOX_OPTOUT"

# The opt-out rides declared action config, so it is set in the deployment
# config file rather than smuggled through the model's arguments.
jq -e '.config | map(select(.name == "SANDBOX" and .key == "sandbox")) | length == 1' \
   actions/core/bash/action.json >/dev/null ||
  fail "actions/core/bash/action.json does not declare the sandbox config key"

rm -rf "$WS" "$fakebin"
printf 'action sandbox: PASS\n'
