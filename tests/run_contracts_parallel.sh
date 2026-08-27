#!/usr/bin/env bash
# Run independent shell/public-contract probes in four fixed workers. Every
# probe still creates its own disposable repository copy; this only overlaps
# their wall time and keeps output deterministic by replaying worker logs in
# order after all workers finish.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"

worker_one_tests=(
  tests/test_cli_output_modes.sh
  tests/test_local_client_api.sh
  tests/test_config_cli.sh
  tests/test_managed_handle_restart.sh
)
worker_two_tests=(
  tests/test_version_contract.sh
  tests/test_action_cli.sh
  tests/test_bus_typed_ingress.sh
  tests/test_manage_hermes_completion.sh
)
worker_three_tests=(
  tests/test_web_read_action.sh
  tests/test_tokenwatch_app.sh
  tests/test_telegram_transport.sh
  tests/test_gcal_result_text.sh
)
worker_four_tests=(
  tests/test_mcp_generation.sh
  tests/test_action_sandbox.sh
)

# These are run directly by `make test` or `make test-build-contracts` rather
# than by the workers below. Keeping them in the same registry makes an added
# tests/test_*.sh impossible to omit silently from both gates.
separately_registered_tests=(
  tests/test_adapter_selection.sh
  tests/test_android_arm64_build_contract.sh
  tests/test_fixture_isolation.sh
  tests/test_mock_only_build_contract.sh
  tests/test_release_packaging.sh
  tests/test_release_small_build_contract.sh
)

assert_contract_registry() {
  local actual expected
  actual="$(cd "$ROOT" && printf '%s\n' tests/test_*.sh | LC_ALL=C sort)"
  expected="$(printf '%s\n' \
    "${worker_one_tests[@]}" \
    "${worker_two_tests[@]}" \
    "${worker_three_tests[@]}" \
    "${worker_four_tests[@]}" \
    "${separately_registered_tests[@]}" | LC_ALL=C sort)"
  if [[ "$actual" != "$expected" ]]; then
    printf 'shell contract registry does not match tests/test_*.sh\n' >&2
    diff -u <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") >&2 || true
    return 1
  fi
}

if [[ "${1:-}" == "--check-registry" ]]; then
  [[ "$#" -eq 1 ]] || {
    printf 'usage: %s [--check-registry]\n' "$0" >&2
    exit 2
  }
  assert_contract_registry
  exit 0
fi
[[ "$#" -eq 0 ]] || {
  printf 'usage: %s [--check-registry]\n' "$0" >&2
  exit 2
}
assert_contract_registry

log_dir="$(mktemp -d "${TMPDIR:-/tmp}/fclaw-contracts.XXXXXX")"
pids=()
cleanup() {
  local status=$?
  trap - EXIT HUP INT TERM
  for pid in "${pids[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -rf -- "$log_dir"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

run_contract_tests() {
  local test
  for test in "$@"; do
    ./tests/run_in_isolated_copy.sh bash "./$test"
  done
}

worker_one() {
  cd "$ROOT"
  run_contract_tests "${worker_one_tests[@]}"
}
worker_two() {
  cd "$ROOT"
  run_contract_tests "${worker_two_tests[@]}"
}
worker_three() {
  cd "$ROOT"
  run_contract_tests "${worker_three_tests[@]}"
}
worker_four() {
  cd "$ROOT"
  run_contract_tests "${worker_four_tests[@]}"
}

for worker in one two three four; do
  "worker_$worker" >"$log_dir/$worker.log" 2>&1 &
  pids+=("$!")
done

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then status=1; fi
done
pids=()

for worker in one two three four; do
  printf '===== contract worker %s =====\n' "$worker"
  cat "$log_dir/$worker.log"
done
[ "$status" -eq 0 ]
