# Shared action sandbox and deadline helpers. Sourced, never executed.
#
# Two problems both belong to the action scripts rather than the kernel:
#
#   1. The deadline. The manifest's timeout_ms is what the kernel kills the
#      job at. A script that also hardcodes its own number can only agree by
#      coincidence, and disagreed in every shipped action. The kernel now
#      exports FCLAW_ACTION_TIMEOUT_MS, and fclaw_action_timeout_s derives
#      the command's budget from it, reserving a margin so the script reports
#      a clean timeout instead of being SIGKILLed mid-write.
#
#   2. The sandbox. An action that runs a shell command, applies a patch, or
#      fetches a URL runs as the gateway user with the whole filesystem
#      writable. fclaw_sandbox_argv returns the argv prefix that confines
#      writes to the workspace: sandbox-exec on macOS, bwrap on Linux.
#
# The sandbox fails CLOSED. If no supported mechanism is present the action
# refuses rather than running unconfined, and the operator opts out
# deliberately by setting actions.<id>.sandbox = "off" in
# config/floofclaw_config.json, which the manifest maps to $SANDBOX.

# fclaw_action_timeout_s <fallback_seconds> [reserve_seconds]
#
# Seconds the script may spend on its own work. Defaults to the manifest
# deadline minus a reserve for process startup and result serialization;
# falls back to the caller's constant when run outside the kernel.
fclaw_action_timeout_s() {
  local fallback="${1:-30}" reserve="${2:-5}" ms budget
  ms="${FCLAW_ACTION_TIMEOUT_MS:-}"
  case "$ms" in
    ''|*[!0-9]*) printf '%s' "$fallback"; return 0 ;;
  esac
  budget=$(( ms / 1000 - reserve ))
  [ "$budget" -lt 1 ] && budget=1
  printf '%s' "$budget"
}

# fclaw_sandbox_mode
#
# "off" when the operator opted out, otherwise "on".
fclaw_sandbox_mode() {
  case "${SANDBOX:-}" in
    off|OFF|false|0) printf 'off' ;;
    *)               printf 'on' ;;
  esac
}

# fclaw_sandbox_unavailable_message
#
# Why the action is refusing, and exactly how to proceed.
fclaw_sandbox_unavailable_message() {
  printf '%s' "no action sandbox is available on this host (need sandbox-exec on macOS or bwrap on Linux); \
install one, or accept the risk explicitly by setting actions.${1:-<action>}.sandbox to \"off\" in config/floofclaw_config.json"
}

# fclaw_sandbox_scratch <writable_root>
#
# Creates and prints a per-invocation scratch directory INSIDE the writable
# root. Confinement is not only about malice: `patch`, python's tempfile,
# and most tools that need working space write it to TMPDIR, which is
# outside everything the sandbox permits. The caller exports the result as
# TMPDIR (this only prints -- an export here would die with the command
# substitution) and removes it on exit.
#
# macOS mktemp(1) with no template ignores TMPDIR and goes straight to the
# per-user Darwin temp directory, so a bare `mktemp` fails under
# confinement. That is not an exception worth carving out: the per-user
# temp directory is a plausible install location, and allowing it would
# void the confinement for any deployment that lives there. Use
# "$TMPDIR", `mktemp -t`, or a workspace path instead.
fclaw_sandbox_scratch() {
  local root="${1:-}" dir
  [ -n "$root" ] || return 1
  dir="$root/.fclaw/tmp/$$"
  mkdir -p "$dir" || return 1
  chmod 700 "$root/.fclaw" "$root/.fclaw/tmp" "$dir" 2>/dev/null || true
  printf '%s' "$dir"
}

# fclaw_sandbox_argv <writable_root>
#
# Prints the argv prefix that confines writes to <writable_root>, one
# argument per line, and returns 0. Returns 1 and prints nothing when no
# mechanism is available; the caller must then refuse. Prints nothing and
# returns 0 when the operator has opted out.
fclaw_sandbox_argv() {
  local root="${1:-}" profile real
  [ -n "$root" ] || return 1
  [ "$(fclaw_sandbox_mode)" = "on" ] || return 0
  # Both mechanisms match on the resolved path, and on macOS /tmp is a
  # symlink to /private/tmp -- an unresolved root silently confines the
  # command out of its own workspace.
  real="$(cd "$root" 2>/dev/null && pwd -P)" || return 1
  [ -n "$real" ] || return 1
  root="$real"
  if command -v sandbox-exec >/dev/null 2>&1; then
    # Everything is readable and runnable; writes are denied except under
    # the workspace and the character devices a normal program needs.
    # An fd the parent opened before exec stays writable, so the action's
    # own stdout/stderr artifacts are unaffected.
    # One line on purpose: the argv prefix is newline-delimited, so a
    # multi-line profile would be read back as several arguments.
    profile="(version 1) (allow default) (deny file-write*)"
    profile="$profile (allow file-write* (subpath \"$root\"))"
    profile="$profile (allow file-write-data (literal \"/dev/null\") (literal \"/dev/zero\") (literal \"/dev/stdout\") (literal \"/dev/stderr\") (literal \"/dev/tty\") (literal \"/dev/dtracehelper\"))"
    profile="$profile (allow file-ioctl (literal \"/dev/tty\") (literal \"/dev/dtracehelper\"))"
    printf '%s\n' "sandbox-exec" "-p" "$profile"
    return 0
  fi
  if command -v bwrap >/dev/null 2>&1; then
    # Read-only root with the workspace bound writable; /tmp stays the
    # read-only view of the host's, exactly like macOS. No tmpfs over it:
    # scratch already lives inside the workspace via TMPDIR, and a tmpfs
    # mounted over /tmp hides everything the host keeps there -- including
    # a deployment or test copy that lives under /tmp, whose own workspace
    # bind it would shadow. --die-with-parent keeps a killed job from
    # leaving the child behind.
    printf '%s\n' "bwrap" "--ro-bind" "/" "/" "--bind" "$root" "$root" \
                  "--dev" "/dev" "--proc" "/proc" \
                  "--die-with-parent" "--"
    return 0
  fi
  return 1
}
