#!/usr/bin/env bash
# Give one MCP server's credential to every action generated from it.
#
# Action secrets are scoped to a single action id by design: the gateway
# injects only endpoint:action:<id>:* into that action's environment, so
# one tool can never read another's credential. A server with eight tools
# therefore needs the value stored eight times. That is the correct
# security shape and a poor thing to do by hand, so this does it.
#
#   printf %s "$TOKEN" | scripts/mcp_secret.sh github GITHUB_TOKEN
#
# The value is read from stdin and passed to `fclaw auth set-stdin` on a
# pipe. It never appears in argv, in config/mcp.json, or in this script's
# output. In config/mcp.json the server's env entry names it:
#   "env": { "GITHUB_TOKEN": "${GITHUB_TOKEN}" }
set -uo pipefail

as_json=0
if [ "${1-}" = "--json" ]; then as_json=1; shift; fi

server="${1-}"
name="${2-}"
OUT_DIR="${FCLAW_MCP_ACTIONS_DIR:-actions/mcp}"
FCLAW="${FCLAW_BIN:-./bin/fclaw}"

die() {
  if [ "$as_json" = 1 ]; then
    jq -cn --arg m "$*" '{ok:false,error:$m}' 2>/dev/null \
      || printf '{"ok":false,"error":"mcp secret failed"}\n'
  fi
  printf 'mcp-secret: %s\n' "$*" >&2
  exit 1
}

[ -n "$server" ] && [ -n "$name" ] || die "usage: printf %s \"\$VALUE\" | $0 <server> <ENV_NAME>"
printf '%s' "$name" | grep -Eq '^[A-Za-z_][A-Za-z0-9_]*$' \
  || die "'$name' is not a usable environment variable name"
[ -x "$FCLAW" ] || die "$FCLAW is not executable (build first, or set FCLAW_BIN)"
[ -d "$OUT_DIR" ] || die "$OUT_DIR does not exist; run scripts/mcp_sync.sh first"

# The secret name is lowercased here because injection uppercases it: a
# secret stored as ...:github_token arrives in the action as $GITHUB_TOKEN.
lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')"

ids=()
for dir in "$OUT_DIR"/"${server}"__*/; do
  [ -d "$dir" ] || continue
  ids+=("$(basename "$dir")")
done
[ "${#ids[@]}" -gt 0 ] || die "no actions generated from server '$server'; run scripts/mcp_sync.sh"

value="$(cat)"
[ -n "$value" ] || die "no value on stdin"

count=0
for id in "${ids[@]}"; do
  printf '%s' "$value" | "$FCLAW" auth set-stdin -a "action:${id}:${lower}" >/dev/null \
    || die "storing action:${id}:${lower} failed"
  count=$((count + 1))
done
value=""

if [ "$as_json" = 1 ]; then
  printf '%s\n' "${ids[@]}" | jq -R . |
    jq -sc --arg n "$name" --arg s "$server" --argjson c "$count" \
      '{ok:true,server:$s,env:$n,actions:$c,ids:.}'
  exit 0
fi
printf 'mcp-secret: stored %s for %d action(s) of server %s\n' "$name" "$count" "$server"
printf 'mcp-secret: re-run this after any sync that adds tools to %s\n' "$server"
