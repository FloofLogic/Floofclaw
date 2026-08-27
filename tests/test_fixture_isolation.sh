#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$ROOT"

# The proof is "a SIGKILLed test run cannot dirty the checkout" —
# meaningless without a checkout. Release-export staging trees are
# plain tar extracts; users who clone the release repo have git.
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "fixture isolation: skipped — not a git checkout (nothing to prove against)"
  exit 0
fi

tmp_parent="$(cd "${TMPDIR:-/tmp}" && pwd -P)"
case "$tmp_parent/" in
  "$ROOT/"*)
    printf 'fixture isolation: TMPDIR must be outside the repository: %s\n' \
      "$tmp_parent" >&2
    exit 2
    ;;
esac
control_dir="$(mktemp -d "$tmp_parent/fclaw-fixture-control.XXXXXX")"
before="$control_dir/status.before"
after="$control_dir/status.after"
direct_log="$control_dir/direct.log"
budget_log="$control_dir/budget.log"
shard_log="$control_dir/shard.log"
sanitizer_input="$control_dir/sanitizer.input"
sanitizer_log="$control_dir/sanitizer.log"
registry_log="$control_dir/registry.log"
probe_log="$control_dir/probe.log"
ready="$control_dir/ready"
child_pid_file="$control_dir/child.pid"
: > "$ready"
: > "$child_pid_file"
wrapper_pid=""
probe_pid=""

cleanup() {
  local status=$?
  trap - EXIT
  if [[ -n "$probe_pid" ]] && kill -0 "$probe_pid" 2>/dev/null; then
    kill -KILL "$probe_pid" 2>/dev/null || true
  fi
  if [[ -n "$wrapper_pid" ]] && kill -0 "$wrapper_pid" 2>/dev/null; then
    if [[ -z "$probe_pid" ]]; then
      kill "$wrapper_pid" 2>/dev/null || true
    fi
    wait "$wrapper_pid" 2>/dev/null || true
  fi
  cd "$ROOT" || status=1
  rm -rf -- "$control_dir" || status=1
  exit "$status"
}
trap cleanup EXIT

git status --porcelain=v1 -z --untracked-files=all > "$before"

set +e
FCLAW_TEST_ISOLATED= FCLAW_TEST_SOURCE_ROOT= FCLAW_TEST_ISOLATED_ROOT= \
  ./bin/fclaw_integration_tests opgeneric_fake_one_full_lifecycle \
  >"$direct_log" 2>&1
direct_rc=$?
set -e
if [[ "$direct_rc" -ne 2 ]] ||
   ! grep -q 'refusing to run outside an isolated copy' "$direct_log"; then
  printf 'fixture isolation: direct test binary did not refuse safely\n' >&2
  cat "$direct_log" >&2
  exit 1
fi

set +e
FCLAW_TEST_BUDGET_SCALE=not-a-number \
  ./tests/run_in_isolated_copy.sh \
  ./bin/fclaw_integration_tests configured_default_floop_is_loadable \
  >"$budget_log" 2>&1
budget_rc=$?
set -e
if [[ "$budget_rc" -ne 2 ]] ||
   ! grep -q 'FCLAW_TEST_BUDGET_SCALE must be an integer from 1 to 100' \
     "$budget_log"; then
  printf 'fixture isolation: malformed budget scale did not fail loudly\n' >&2
  cat "$budget_log" >&2
  exit 1
fi

set +e
FCLAW_TEST_SHARD=not-a-shard \
  ./tests/run_in_isolated_copy.sh \
  ./bin/fclaw_integration_tests configured_default_floop_is_loadable \
  >"$shard_log" 2>&1
shard_rc=$?
set -e
if [[ "$shard_rc" -ne 2 ]] ||
   ! grep -q 'FCLAW_TEST_SHARD must be I/N with 1 <= I <= N' "$shard_log"; then
  printf 'fixture isolation: malformed shard did not fail loudly\n' >&2
  cat "$shard_log" >&2
  exit 1
fi

printf '%s\n' \
  'child: runtime error: signed integer overflow' \
  '==1==ERROR: AddressSanitizer: heap-use-after-free' >"$sanitizer_input"
set +e
./tests/run_integration_parallel.sh --check-sanitizer-log "$sanitizer_input" \
  >"$sanitizer_log" 2>&1
sanitizer_rc=$?
set -e
if [[ "$sanitizer_rc" -ne 1 ]] ||
   ! grep -q 'emitted a sanitizer diagnostic' "$sanitizer_log"; then
  printf 'fixture isolation: child sanitizer diagnostic was not fatal\n' >&2
  cat "$sanitizer_log" >&2
  exit 1
fi

set +e
./tests/run_in_isolated_copy.sh bash -c '
  printf "#!/usr/bin/env bash\n" > tests/test_unregistered_contract.sh
  ./tests/run_contracts_parallel.sh --check-registry
' >"$registry_log" 2>&1
registry_rc=$?
set -e
if [[ "$registry_rc" -ne 1 ]] ||
   ! grep -q 'shell contract registry does not match tests/test_\*.sh' \
     "$registry_log" ||
   ! grep -q 'test_unregistered_contract.sh' "$registry_log"; then
  printf 'fixture isolation: unregistered shell contract was not rejected\n' >&2
  cat "$registry_log" >&2
  exit 1
fi

FCLAW_TEST_FIXTURE_ISOLATION_READY_FILE="$ready" \
  FCLAW_TEST_ISOLATION_CHILD_PID_FILE="$child_pid_file" \
  ./tests/run_in_isolated_copy.sh \
  ./bin/fclaw_integration_tests opgeneric_fake_one_full_lifecycle \
  >"$probe_log" 2>&1 &
wrapper_pid=$!

for _ in $(seq 1 500); do
  [[ -s "$child_pid_file" ]] && break
  if ! kill -0 "$wrapper_pid" 2>/dev/null; then
    printf 'fixture isolation: wrapper exited before publishing child PID\n' >&2
    cat "$probe_log" >&2
    exit 1
  fi
  sleep 0.01
done
if [[ ! -s "$child_pid_file" ]]; then
  printf 'fixture isolation: timed out waiting for fixture child PID\n' >&2
  cat "$probe_log" >&2
  exit 1
fi
probe_pid="$(sed -n '1p' "$child_pid_file")"
if [[ ! "$probe_pid" =~ ^[0-9]+$ ]]; then
  printf 'fixture isolation: malformed child PID record\n' >&2
  exit 1
fi

for _ in $(seq 1 500); do
  [[ -s "$ready" ]] && break
  if ! kill -0 "$probe_pid" 2>/dev/null; then
    printf 'fixture isolation: probe exited before publishing readiness\n' >&2
    cat "$probe_log" >&2
    exit 1
  fi
  sleep 0.01
done
if [[ ! -s "$ready" ]]; then
  printf 'fixture isolation: timed out waiting for fixture writer\n' >&2
  cat "$probe_log" >&2
  exit 1
fi

ready_pid="$(sed -n '1p' "$ready")"
probe_root="$(sed -n '2p' "$ready")"
if [[ "$ready_pid" != "$probe_pid" ]] || [[ -z "$probe_root" ]]; then
  printf 'fixture isolation: malformed readiness record\n' >&2
  exit 1
fi
case "$probe_root/" in
  "$ROOT/"*)
    printf 'fixture isolation: writer ran inside repository: %s\n' \
      "$probe_root" >&2
    exit 1
    ;;
esac
test -f "$probe_root/actions/testfx/fake_one/action.json"
test -f "$probe_root/actions/testfx/fake_two/action.json"
test -f "$probe_root/floops/opgeneric/loop.json"

kill -KILL "$probe_pid"
set +e
wait "$wrapper_pid"
probe_rc=$?
set -e
wrapper_pid=""
probe_pid=""
if [[ "$probe_rc" -ne 137 ]]; then
  printf 'fixture isolation: killed fixture writer exited %d, expected 137\n' \
    "$probe_rc" >&2
  cat "$probe_log" >&2
  exit 1
fi
if [[ -e "$probe_root" ]]; then
  printf 'fixture isolation: scratch root survived killed writer: %s\n' \
    "$probe_root" >&2
  exit 1
fi

git status --porcelain=v1 -z --untracked-files=all > "$after"
if ! cmp -s "$before" "$after"; then
  printf 'fixture isolation: killed test changed checkout status\n' >&2
  git status --short >&2
  exit 1
fi

printf 'fixture isolation: direct runner, malformed controls, sanitizer diagnostics, and unregistered shell contracts refused; SIGKILL left checkout unchanged\n'
