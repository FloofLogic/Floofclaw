#!/usr/bin/env bash
set -euo pipefail

input="$(cat)"

if ! command -v jq >/dev/null 2>&1; then
  printf '%s\n' '{"events":[],"result":{},"error":{"code":"DEPENDENCY_MISSING","message":"git_github requires jq. Install jq with your package manager, then restart the FloofClaw gateway."}}'
  exit 1
fi

emit_error() {
  jq -cn --arg code "$1" --arg message "$2" \
    '{events:[],result:{},error:{code:$code,message:$message}}'
  exit 1
}

emit_unavailable() {
  jq -cn --arg handle "$handle" --arg tool "$1" --arg code "$2" --arg text "$3" \
    '{events:[],result:{status:"finished",handle:$handle,ok:false,tool:$tool,exit_code:127,code:$code,text:$text},error:null}'
  exit 0
}

jq -e 'type == "object" and (.args | type == "object")' >/dev/null 2>&1 <<<"$input" ||
  emit_error "BAD_ARGS" "expected a FloofClaw action input envelope"

op="$(jq -r '.args.op // ""' <<<"$input")"
request_id="$(jq -r '.request_id // ""' <<<"$input")"
safe_id="$(printf '%s' "$request_id" | tr -cd '[:alnum:]_-')"
[ -n "$safe_id" ] || safe_id="request"
handle="gitgithub_${safe_id:0:72}"

[ "$op" = "start" ] || emit_error "BAD_ARGS" "args.op must be start"

kind="$(jq -r '.args.kind // ""' <<<"$input")"
case "$kind" in
  check|git|gh) ;;
  *) emit_error "BAD_ARGS" "args.kind is required with op:start and must be check, git, or gh" ;;
esac

workspace_root="${FCLAW_WORKSPACE_ROOT:-}"
[ -n "$workspace_root" ] || emit_error "CONFIG_ERROR" "FCLAW_WORKSPACE_ROOT is required"
[ -d "$workspace_root" ] || emit_error "CONFIG_ERROR" "FCLAW_WORKSPACE_ROOT must name an existing directory"
workspace_root="$(cd "$workspace_root" && pwd -P)" ||
  emit_error "CONFIG_ERROR" "could not resolve FCLAW_WORKSPACE_ROOT"

repo="$(jq -r '.args.repo // "."' <<<"$input")"
case "$repo" in
  ""|.) work_candidate="$workspace_root" ;;
  /*|..|../*|*/../*|*/..) emit_error "BAD_ARGS" "args.repo must be a workspace-relative directory without .. traversal" ;;
  *) work_candidate="$workspace_root/$repo" ;;
esac
[ -d "$work_candidate" ] ||
  emit_error "REPO_NOT_FOUND" "args.repo must name an existing directory inside the FloofClaw workspace"
work_dir="$(cd "$work_candidate" && pwd -P)" ||
  emit_error "REPO_NOT_FOUND" "could not resolve args.repo"
case "$work_dir" in
  "$workspace_root"|"$workspace_root"/*) ;;
  *) emit_error "BAD_ARGS" "args.repo resolves outside the FloofClaw workspace" ;;
esac

case "$(uname -s 2>/dev/null || printf unknown)" in
  Darwin)
    git_install='Install Git by running: xcode-select --install (or: brew install git). Then restart the FloofClaw gateway.'
    gh_install='Install GitHub CLI by running: brew install gh. Then run: gh auth login. Run setup as the same OS account that starts FloofClaw, then restart the gateway.'
    ;;
  Linux)
    git_install='Install Git with your distribution package manager (Debian/Ubuntu: sudo apt-get update && sudo apt-get install -y git). Then restart the FloofClaw gateway.'
    gh_install='Install GitHub CLI using https://cli.github.com/ or your distribution package manager. Then run: gh auth login. For a headless gateway, store a token with: printf "%s" "$TOKEN" | ./bin/fclaw auth set-stdin -h action:git_github:gh_token. Restart the gateway afterward.'
    ;;
  *)
    git_install='Install Git from https://git-scm.com/downloads, ensure git is on PATH for the FloofClaw process, then restart the gateway.'
    gh_install='Install GitHub CLI from https://cli.github.com/, run: gh auth login, and ensure gh is on PATH for the FloofClaw process. Then restart the gateway.'
    ;;
esac
git_identity_setup='Configure commit identity in the repository with: git config user.name "Your Name" and git config user.email "you@example.com". Use --global only if that identity is correct for every repository owned by this OS account.'
github_auth_setup='Authenticate GitHub CLI by running: gh auth login. Run it as the same OS account that starts FloofClaw. For a headless gateway, store GH_TOKEN with: printf "%s" "$TOKEN" | ./bin/fclaw auth set-stdin -h action:git_github:gh_token. Then restart the gateway.'

git_available=false
git_version=""
git_name=""
git_email=""
git_identity_configured=false
inside_work_tree=false
if command -v git >/dev/null 2>&1; then
  git_available=true
  git_version="$(git --version 2>&1 | head -c 200 || true)"
  git_name="$(git -C "$work_dir" config --get user.name 2>/dev/null || true)"
  git_email="$(git -C "$work_dir" config --get user.email 2>/dev/null || true)"
  [ -z "$git_name" ] || [ -z "$git_email" ] || git_identity_configured=true
  if git -C "$work_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    inside_work_tree=true
  fi
fi

gh_available=false
gh_version=""
gh_authenticated=false
if command -v gh >/dev/null 2>&1; then
  gh_available=true
  gh_version="$(gh --version 2>&1 | head -n 1 | head -c 200 || true)"
  if GH_PROMPT_DISABLED=1 gh auth status --hostname "${GH_HOST:-github.com}" >/dev/null 2>&1; then
    gh_authenticated=true
  fi
fi

if [ "$kind" = "check" ]; then
  ready=false
  [ "$git_available" = true ] && [ "$gh_available" = true ] &&
    [ "$gh_authenticated" = true ] && ready=true
  text=""
  if [ "$git_available" = true ]; then
    text="Git is installed: $git_version."
    if [ "$inside_work_tree" = true ]; then
      text="$text The selected directory is inside a Git work tree."
    else
      text="$text The selected directory is not inside a Git work tree."
    fi
    if [ "$git_identity_configured" = true ]; then
      text="$text Commit identity is configured as $git_name <$git_email>."
    else
      text="$text Commit identity is not fully configured. $git_identity_setup"
    fi
  else
    text="Git is not installed or is not visible on FloofClaw's PATH. $git_install"
  fi
  if [ "$gh_available" = true ]; then
    text="$text GitHub CLI is installed: $gh_version."
    if [ "$gh_authenticated" = true ]; then
      text="$text GitHub authentication is ready."
    else
      text="$text GitHub authentication is not ready. $github_auth_setup"
    fi
  else
    text="$text GitHub CLI is not installed or is not visible on FloofClaw's PATH. $gh_install"
  fi
  jq -cn --arg handle "$handle" --arg text "$text" --arg repo "$repo" \
    --arg git_version "$git_version" --arg gh_version "$gh_version" \
    --arg git_name "$git_name" --arg git_email "$git_email" \
    --argjson ready "$ready" --argjson git_available "$git_available" \
    --argjson gh_available "$gh_available" \
    --argjson gh_authenticated "$gh_authenticated" \
    --argjson git_identity_configured "$git_identity_configured" \
    --argjson inside_work_tree "$inside_work_tree" \
    '{events:[],result:{status:"finished",handle:$handle,ok:true,kind:"check",ready:$ready,repo:$repo,git_available:$git_available,git_version:$git_version,git_identity_configured:$git_identity_configured,git_name:$git_name,git_email:$git_email,inside_work_tree:$inside_work_tree,gh_available:$gh_available,gh_version:$gh_version,gh_authenticated:$gh_authenticated,text:$text},error:null}'
  exit 0
fi

argv_count="$(jq -r '(.args.argv // []) | length' <<<"$input")"
[ "$argv_count" -gt 0 ] || emit_error "BAD_ARGS" "args.argv must be a non-empty array for kind:git or kind:gh"
[ "$argv_count" -le 64 ] || emit_error "BAD_ARGS" "args.argv supports at most 64 literal arguments"
jq -e 'all(.args.argv[]; type == "string" and (length <= 4096) and (test("[\\r\\n]") | not))' >/dev/null 2>&1 <<<"$input" ||
  emit_error "BAD_ARGS" "each args.argv item must be a string of at most 4096 characters without newlines"

argv=()
while IFS= read -r arg; do
  argv+=("$arg")
done < <(jq -r '.args.argv[]' <<<"$input")

if [ "$kind" = "git" ]; then
  command -v git >/dev/null 2>&1 ||
    emit_unavailable "git" "DEPENDENCY_MISSING" "Git is not installed or is not visible on FloofClaw's PATH. $git_install"
  for arg in "${argv[@]}"; do
    case "$arg" in
      -C|--git-dir|--git-dir=*|--work-tree|--work-tree=*|--exec-path|--exec-path=*)
        emit_error "BAD_ARGS" "repository-routing flags are not allowed in args.argv; select the repository with args.repo"
        ;;
    esac
  done
  command_name="git"
  command_path="$(command -v git)"
else
  command -v gh >/dev/null 2>&1 ||
    emit_unavailable "gh" "DEPENDENCY_MISSING" "GitHub CLI is not installed or is not visible on FloofClaw's PATH. $gh_install"
  if [ "${argv[0]}" = "auth" ] && [ "${argv[1]-}" = "token" ]; then
    emit_error "SECRET_ACCESS_DENIED" "gh auth token is not allowed because action output is recorded; configure authentication outside the action"
  fi
  for arg in "${argv[@]}"; do
    case "$arg" in
      --show-token|--with-token)
        emit_error "SECRET_ACCESS_DENIED" "$arg is not allowed because action arguments and output are recorded; configure authentication outside the action"
        ;;
    esac
  done
  command_name="gh"
  command_path="$(command -v gh)"
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/fclaw-git-github.XXXXXX")" ||
  emit_error "EXEC_ERROR" "could not create command output directory"
trap 'rm -rf "$tmp"' EXIT
stdout_file="$tmp/stdout"
stderr_file="$tmp/stderr"

set +e
(
  cd "$work_dir" || exit 125
  GH_PROMPT_DISABLED=1 GH_PAGER=cat PAGER=cat GIT_TERMINAL_PROMPT=0 \
    "$command_path" "${argv[@]}"
) >"$stdout_file" 2>"$stderr_file"
command_rc=$?
set -e

body_artifact="$(jq -r '.artifacts.body // ""' <<<"$input")"
if [ -n "$body_artifact" ]; then
  case "$body_artifact" in
    /*|..|../*|*/../*|*/..) emit_error "BAD_ARTIFACT" "artifacts.body must be a safe workspace-relative path" ;;
  esac
  body_path="$workspace_root/$body_artifact"
  mkdir -p "$(dirname "$body_path")" ||
    emit_error "ARTIFACT_ERROR" "could not create the command output artifact directory"
  {
    printf '%s\n' '--- stdout ---'
    cat "$stdout_file"
    printf '%s\n' '--- stderr ---'
    cat "$stderr_file"
  } >"$body_path" ||
    emit_error "ARTIFACT_ERROR" "could not write the command output artifact"
fi

stdout_bytes="$(wc -c <"$stdout_file" | tr -d ' ')"
stderr_bytes="$(wc -c <"$stderr_file" | tr -d ' ')"
output_excerpt="$(LC_ALL=C head -c 1600 "$stdout_file")"
stderr_excerpt="$(LC_ALL=C head -c 1200 "$stderr_file")"
if [ -n "$stderr_excerpt" ]; then
  if [ -n "$output_excerpt" ]; then
    output_excerpt="$output_excerpt
--- stderr ---
$stderr_excerpt"
  else
    output_excerpt="$stderr_excerpt"
  fi
fi
truncated=false
[ $((stdout_bytes + stderr_bytes)) -le 2800 ] || truncated=true

if [ "$command_rc" -eq 0 ]; then
  ok=true
  text="$command_name command completed successfully."
else
  ok=false
  text="$command_name command exited with status $command_rc."
  if [ "$kind" = "gh" ] &&
    ! GH_PROMPT_DISABLED=1 gh auth status --hostname "${GH_HOST:-github.com}" >/dev/null 2>&1; then
    text="$text GitHub authentication is not ready. $github_auth_setup"
  fi
fi
[ -z "$output_excerpt" ] || text="$text
$output_excerpt"

while :; do
  result="$(jq -cn --arg handle "$handle" --arg tool "$command_name" \
    --arg repo "$repo" --arg text "$text" --arg body_artifact "$body_artifact" \
    --argjson ok "$ok" --argjson exit_code "$command_rc" \
    --argjson stdout_bytes "$stdout_bytes" --argjson stderr_bytes "$stderr_bytes" \
    --argjson truncated "$truncated" \
    '{status:"finished",handle:$handle,ok:$ok,tool:$tool,repo:$repo,exit_code:$exit_code,stdout_bytes:$stdout_bytes,stderr_bytes:$stderr_bytes,truncated:$truncated,output_artifact:$body_artifact,text:$text}')"
  [ "${#result}" -le 3500 ] && break
  [ "${#output_excerpt}" -gt 0 ] ||
    emit_error "RESULT_TOO_LARGE" "command result metadata exceeds the managed-operation result budget"
  output_excerpt="${output_excerpt:0:${#output_excerpt}-128}"
  truncated=true
  if [ "$command_rc" -eq 0 ]; then
    text="$command_name command completed successfully."
  else
    text="$command_name command exited with status $command_rc."
  fi
  [ -z "$output_excerpt" ] || text="$text
$output_excerpt"
done

jq -cn --argjson result "$result" '{events:[],result:$result,error:null}'
