#!/usr/bin/env bash
set -euo pipefail

action_dir="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/git-github-test.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/missing-bin" "$tmp/workspace/repos/project"

cat >"$tmp/bin/git" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" >"${CAPTURE_GIT_ARGS:?}"
case "${1-}" in
  --version) printf '%s\n' 'git version 2.50.1'; exit 0 ;;
esac
if [ "${1-}" = "-C" ]; then
  shift 2
fi
if [ "${1-}" = "config" ] && [ "${2-}" = "--get" ]; then
  case "${3-}" in
    user.name) [ "${MOCK_GIT_IDENTITY:-1}" = 1 ] && printf '%s\n' 'Floof Tester' ;;
    user.email) [ "${MOCK_GIT_IDENTITY:-1}" = 1 ] && printf '%s\n' 'floof@example.test' ;;
  esac
  [ "${MOCK_GIT_IDENTITY:-1}" = 1 ]
  exit
fi
if [ "${1-}" = "rev-parse" ]; then
  [ "${MOCK_GIT_REPO:-1}" = 1 ] && printf '%s\n' true
  [ "${MOCK_GIT_REPO:-1}" = 1 ]
  exit
fi
printf '%s\n' "${MOCK_GIT_STDOUT:-git command ok}"
printf '%s' "${MOCK_GIT_STDERR:-}" >&2
exit "${MOCK_GIT_RC:-0}"
SH

cat >"$tmp/bin/gh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" >"${CAPTURE_GH_ARGS:?}"
if [ "${1-}" = "--version" ]; then
  printf '%s\n' 'gh version 2.76.1 (fixture)'
  exit 0
fi
if [ "${1-}" = "auth" ] && [ "${2-}" = "status" ]; then
  if [ "${MOCK_GH_AUTH:-1}" = 1 ]; then
    printf '%s\n' 'Logged in to github.com'
    exit 0
  fi
  printf '%s\n' 'not logged in' >&2
  exit 1
fi
printf '%s\n' "${MOCK_GH_STDOUT:-pull request list fixture}"
printf '%s' "${MOCK_GH_STDERR:-}" >&2
exit "${MOCK_GH_RC:-0}"
SH

chmod +x "$tmp/bin/git" "$tmp/bin/gh"

export PATH="$tmp/bin:$PATH"
export FCLAW_WORKSPACE_ROOT="$tmp/workspace"
export CAPTURE_GIT_ARGS="$tmp/git.args"
export CAPTURE_GH_ARGS="$tmp/gh.args"
export MOCK_GIT_IDENTITY=1 MOCK_GIT_REPO=1 MOCK_GH_AUTH=1

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

run() {
  printf '%s' "$1" | "$action_dir/run.sh"
}

check_request='{"request_id":"check_1","action":"git_github","args":{"op":"start","kind":"check","repo":"repos/project"}}'
check_output="$(run "$check_request")"
[ "$(jq -r '.result.ready' <<<"$check_output")" = true ] || fail "check should report ready"
[ "$(jq -r '.result.git_identity_configured' <<<"$check_output")" = true ] || fail "check should report Git identity"
[ "$(jq -r '.result.gh_authenticated' <<<"$check_output")" = true ] || fail "check should report GitHub authentication"

git_request='{"request_id":"git_1","action":"git_github","args":{"op":"start","kind":"git","repo":"repos/project","argv":["status","--short"]},"artifacts":{"body":"runs/test/action_calls/git.txt"}}'
git_output="$(run "$git_request")"
[ "$(jq -r '.result.ok' <<<"$git_output")" = true ] || fail "git status should succeed"
[ "$(jq -r '.result.tool' <<<"$git_output")" = git ] || fail "git result should identify tool"
grep -qx 'status' "$CAPTURE_GIT_ARGS" || fail "git subcommand should be one literal argument"
grep -qx -- '--short' "$CAPTURE_GIT_ARGS" || fail "git flag should be one literal argument"
grep -q 'git command ok' "$tmp/workspace/runs/test/action_calls/git.txt" || fail "full Git output artifact should be written"

literal='$(
touch should-never-exist
)'
injection_request="$(jq -cn --arg literal "$literal" '{request_id:"gh_literal",action:"git_github",args:{op:"start",kind:"gh",repo:"repos/project",argv:["pr","list","--search",$literal]}}')"
injection_output="$(run "$injection_request" 2>/dev/null || true)"
[ "$(jq -r '.error.code' <<<"$injection_output")" = BAD_ARGS ] || fail "newline-bearing argv should be rejected"
[ ! -e "$tmp/workspace/repos/project/should-never-exist" ] || fail "argv must never be evaluated as shell"

literal='$(touch should-never-exist)'
injection_request="$(jq -cn --arg literal "$literal" '{request_id:"gh_literal",action:"git_github",args:{op:"start",kind:"gh",repo:"repos/project",argv:["pr","list","--search",$literal]}}')"
injection_output="$(run "$injection_request")"
[ "$(jq -r '.result.ok' <<<"$injection_output")" = true ] || fail "literal shell syntax should be accepted as data"
grep -qxF '$(touch should-never-exist)' "$CAPTURE_GH_ARGS" || fail "shell syntax should reach gh literally"
[ ! -e "$tmp/workspace/repos/project/should-never-exist" ] || fail "literal argv must not execute"

export MOCK_GH_RC=1 MOCK_GH_AUTH=0 MOCK_GH_STDERR='authentication required'
auth_output="$(run '{"request_id":"gh_auth","action":"git_github","args":{"op":"start","kind":"gh","argv":["pr","list"]}}')"
[ "$(jq -r '.result.ok' <<<"$auth_output")" = false ] || fail "failed gh command should return ok:false"
grep -qF 'gh auth login' <<<"$(jq -r '.result.text' <<<"$auth_output")" || fail "failed auth should give direct login instructions"
unset MOCK_GH_RC MOCK_GH_STDERR

set +e
secret_output="$(run '{"request_id":"gh_secret","action":"git_github","args":{"op":"start","kind":"gh","argv":["auth","token"]}}')"
secret_rc=$?
set -e
[ "$secret_rc" -ne 0 ] || fail "credential-revealing gh command should fail"
[ "$(jq -r '.error.code' <<<"$secret_output")" = SECRET_ACCESS_DENIED ] || fail "credential command should have a clear error"

set +e
traversal_output="$(run '{"request_id":"bad_repo","action":"git_github","args":{"op":"start","kind":"git","repo":"../outside","argv":["status"]}}')"
traversal_rc=$?
set -e
[ "$traversal_rc" -ne 0 ] || fail "repo traversal should fail"
[ "$(jq -r '.error.code' <<<"$traversal_output")" = BAD_ARGS ] || fail "repo traversal should be a bad argument"

for name in jq cat tr uname head wc mkdir dirname rm; do
  source_path="$(command -v "$name")"
  ln -s "$source_path" "$tmp/missing-bin/$name"
done
missing_output="$(PATH="$tmp/missing-bin" /bin/bash "$action_dir/run.sh" <<'JSON'
{"request_id":"missing_1","action":"git_github","args":{"op":"start","kind":"check"}}
JSON
)"
[ "$(jq -r '.result.git_available' <<<"$missing_output")" = false ] || fail "missing check should report Git absent"
[ "$(jq -r '.result.gh_available' <<<"$missing_output")" = false ] || fail "missing check should report gh absent"
missing_text="$(jq -r '.result.text' <<<"$missing_output")"
grep -qF 'Git is not installed' <<<"$missing_text" || fail "missing Git should be explained"
grep -qF 'GitHub CLI is not installed' <<<"$missing_text" || fail "missing gh should be explained"

poll_output="$(run '{"request_id":"poll_1","action":"git_github","args":{"op":"poll","handle":"gitgithub_existing"}}')"
[ "$(jq -r '.result.status' <<<"$poll_output")" = finished ] || fail "poll compatibility should finish"
[ "$(jq -r '.result.handle' <<<"$poll_output")" = gitgithub_existing ] || fail "poll should retain handle"

printf '%s\n' 'PASS git_github: setup diagnostics, git/gh argv execution, auth guidance, artifact output, secret denial, path safety, and shell-literal handling'
