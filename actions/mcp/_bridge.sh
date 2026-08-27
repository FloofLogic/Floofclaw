#!/usr/bin/env bash
# The only piece that speaks MCP, and it speaks it at call time only.
#
# Invoked by a generated action's run.sh as:  _bridge.sh <server> <tool>
# Reads the action args JSON on stdin (the ordinary subprocess-action
# contract) and writes the ordinary action result JSON on stdout, so the
# managed-operation driver turns it into an operation_result exactly like
# any other action. Nothing here is known to the kernel.
set -uo pipefail

# The result JSON must reach the real stdout even when die() runs inside a
# command substitution, so keep a private handle on it.
exec 3>&1
tmp_in=""; tmp_out=""
cleanup() { [ -n "$tmp_in" ] && rm -f "$tmp_in"; [ -n "$tmp_out" ] && rm -f "$tmp_out"; return 0; }
trap cleanup EXIT

server="${1-}"
tool="${2-}"
CONFIG="${FCLAW_MCP_CONFIG:-config/mcp.json}"
RESULT_MAX=8192          # matches the operation result-text bound
PROTOCOL="2024-11-05"
# Bounded below the generated action's 60s timeout, so the bridge reports a
# hung server itself instead of being killed mid-write.
CALL_TIMEOUT_S="${FCLAW_MCP_CALL_TIMEOUT_S:-55}"

# die <code> <message>. A failure here fails THIS action call, loudly, and
# nothing else: a bad, dead, or hung MCP server must never wedge the gateway.
die() {
  local code="$1"; shift
  printf '%s\n' "mcp: $*" >&2
  jq -cn --arg c "$code" --arg m "$*" \
    '{events:[],result:{},error:{code:$c,message:$m}}' >&3 2>/dev/null \
    || printf '{"events":[],"result":{},"error":{"code":"MCP_ERROR","message":"mcp call failed"}}\n' >&3
  # Inside a command substitution this exits only the subshell; the caller
  # checks the status and exits for real. The JSON is already out on fd 3.
  exit 1
}

command -v jq >/dev/null 2>&1 || {
  printf '%s\n' '{"events":[],"result":{},"error":{"code":"DEPENDENCY_MISSING","message":"the mcp bridge requires jq"}}'
  exit 1
}
[ -n "$server" ] && [ -n "$tool" ] || die "BAD_ARGS" "usage: _bridge.sh <server> <tool>"
[ -f "$CONFIG" ] || die "CONFIG_ERROR" "$CONFIG is missing; see config/mcp.example.json"

input="$(cat)"
jq -e 'type == "object" and (.args | type == "object")' >/dev/null 2>&1 <<<"$input" ||
  die "BAD_ARGS" "expected a FloofClaw action input envelope"
[ "$(jq -r '.args.op // ""' <<<"$input")" = "start" ] ||
  die "BAD_ARGS" "args.op must be start"

# op is FloofClaw's managed-operation verb, not the tool's argument. Strip it
# so a tool never sees a field it did not declare.
arguments="$(jq -c '.args | del(.op)' <<<"$input")"

# The complete JSON-RPC reply belongs in the call's artifact; only a bounded
# excerpt travels as the operation result.
body_artifact="$(jq -r '.artifacts.body // ""' <<<"$input")"
workspace_root="${FCLAW_WORKSPACE_ROOT:-}"
body_path=""
case "$body_artifact" in
  ""|/*|..|../*|*/../*|*/..) body_path="" ;;
  *) [ -n "$workspace_root" ] && body_path="$workspace_root/$body_artifact" ;;
esac

spec="$(jq -c --arg s "$server" '.mcpServers[$s] // empty' "$CONFIG" 2>/dev/null)" \
  || die "CONFIG_ERROR" "$CONFIG is not readable JSON"
[ -n "$spec" ] || die "CONFIG_ERROR" "no server named '$server' in $CONFIG"

# ${NAME} in an env value means "take NAME from this process's environment".
# An action secret stored as action:<id>:name arrives as $NAME; so does a
# gateway environment variable. The value is never in the config file.
resolve_env() {
  local raw="$1" name
  case "$raw" in
    '${'*'}')
      name="${raw#\$\{}"; name="${name%\}}"
      if [ -z "${!name-}" ]; then
        printf '%s' "$name" >&2
        return 1
      fi
      printf '%s' "${!name}"
      ;;
    *) printf '%s' "$raw" ;;
  esac
}

# Run a child with a bound, feeding it infile and capturing outfile.
# macOS has no coreutils `timeout`, and a hung MCP server must fail this one
# call fast rather than sit on the action's whole deadline. The redirections
# belong to the child, not to this function: bash hands an async command
# /dev/null for stdin no matter how its caller was redirected.
# Returns 124 when it had to kill the child.
run_bounded() {
  local secs="$1" infile="$2" outfile="$3"; shift 3
  local pid waited max_polls monitor=0
  # Job control gives the child its own process group, so the bound can kill
  # the server AND anything it spawned. Without that an orphaned grandchild
  # keeps inherited descriptors open and the caller blocks anyway.
  case "$-" in *m*) monitor=1 ;; esac
  set -m
  # 3>&- : fd 3 is this script's private handle on the real stdout. A server
  # must never inherit it, or a stray child holds our caller's pipe open.
  "$@" < "$infile" > "$outfile" 2>/dev/null 3>&- &
  pid=$!
  [ "$monitor" = 1 ] || set +m
  waited=0
  max_polls=$((secs * 50))
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$max_polls" ]; then
      kill -TERM -"$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null
      sleep 0.1
      kill -KILL -"$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return 124
    fi
    sleep 0.02
    waited=$((waited + 1))
  done
  wait "$pid"
}

init_params() {
  jq -cn --arg p "$PROTOCOL" \
    '{protocolVersion:$p,capabilities:{},clientInfo:{name:"floofclaw",version:"1"}}'
}

# ---- stdio transport -------------------------------------------------
call_stdio() {
  local cmd line raw val
  local -a rest=() env_pairs=()
  cmd="$(printf '%s' "$spec" | jq -r '.command // empty')"
  [ -n "$cmd" ] || die "CONFIG_ERROR" "server '$server' declares neither command nor url"
  while IFS= read -r a; do rest+=("$a"); done < <(printf '%s' "$spec" | jq -r '(.args // [])[]')
  tmp_in="$(mktemp)"; tmp_out="$(mktemp)"
  {
    jq -cn --argjson p "$(init_params)" \
      '{jsonrpc:"2.0",id:1,method:"initialize",params:$p}'
    jq -cn '{jsonrpc:"2.0",method:"notifications/initialized"}'
    jq -cn --arg n "$tool" --argjson a "$arguments" \
      '{jsonrpc:"2.0",id:2,method:"tools/call",params:{name:$n,arguments:$a}}'
  } > "$tmp_in"

  while IFS= read -r key; do
    [ -n "$key" ] || continue
    raw="$(printf '%s' "$spec" | jq -r --arg k "$key" '.env[$k]')"
    if ! val="$(resolve_env "$raw" 2>/dev/null)"; then
      die "CONFIG_ERROR" "server '$server' needs env ${raw} but it is not set in this action's environment"
    fi
    env_pairs+=("$key=$val")
  done < <(printf '%s' "$spec" | jq -r '(.env // {}) | keys[]')

  # The server is spawned per call and exits when its stdin closes. Cold
  # start is the v1 tradeoff; a long-lived supervised server is a separate,
  # deliberate project.
  if [ "${#env_pairs[@]}" -gt 0 ]; then
    run_bounded "$CALL_TIMEOUT_S" "$tmp_in" "$tmp_out" \
      env "${env_pairs[@]}" "$cmd" ${rest+"${rest[@]}"}
  else
    run_bounded "$CALL_TIMEOUT_S" "$tmp_in" "$tmp_out" \
      "$cmd" ${rest+"${rest[@]}"}
  fi
  [ "$?" = 124 ] && die "TIMEOUT" "server '$server' did not answer within ${CALL_TIMEOUT_S}s"
  [ -s "$tmp_out" ] || die "SERVER_ERROR" "server '$server' returned nothing"
  # Match the response by id rather than trusting order.
  line="$(jq -c 'select(.id == 2)' < "$tmp_out" 2>/dev/null | head -n 1)"
  [ -n "$line" ] || die "SERVER_ERROR" "server '$server' sent no reply to tools/call"
  printf '%s' "$line"
}

# ---- http transport --------------------------------------------------
call_http() {
  local url body out
  url="$(printf '%s' "$spec" | jq -r '.url')"
  command -v curl >/dev/null 2>&1 || die "DEPENDENCY_MISSING" "http transport requires curl"
  body="$(jq -cn --argjson p "$(init_params)" \
    '{jsonrpc:"2.0",id:1,method:"initialize",params:$p}')"
  curl -sS -m 30 -X POST -H 'Content-Type: application/json' \
       --data-binary "$body" "$url" >/dev/null 2>&1 \
    || die "SERVER_ERROR" "initialize failed against $url"
  body="$(jq -cn --arg n "$tool" --argjson a "$arguments" \
    '{jsonrpc:"2.0",id:2,method:"tools/call",params:{name:$n,arguments:$a}}')"
  out="$(curl -sS -m 60 -X POST -H 'Content-Type: application/json' \
         --data-binary "$body" "$url" 2>/dev/null)" \
    || die "SERVER_ERROR" "tools/call failed against $url"
  [ -n "$out" ] || die "SERVER_ERROR" "server '$server' returned nothing"
  printf '%s' "$out"
}

if printf '%s' "$spec" | jq -e 'has("url")' >/dev/null 2>&1; then
  reply="$(call_http)" || exit 1
else
  reply="$(call_stdio)" || exit 1
fi

# A JSON-RPC error is this tool failing, reported as this action failing.
# Keep the whole reply where an operator can read it, whatever happens next.
if [ -n "$body_path" ]; then
  mkdir -p "$(dirname "$body_path")" 2>/dev/null &&
    printf '%s\n' "$reply" > "$body_path" 2>/dev/null || body_path=""
fi

if printf '%s' "$reply" | jq -e 'has("error")' >/dev/null 2>&1; then
  die "TOOL_ERROR" "$(printf '%s' "$reply" | jq -r '.error.message // "tool returned an error"')"
fi
if printf '%s' "$reply" | jq -e '.result.isError == true' >/dev/null 2>&1; then
  die "TOOL_ERROR" "$(printf '%s' "$reply" |
         jq -r '[.result.content[]? | select(.type=="text") | .text] | join("\n")
                | if . == "" then "tool reported an error" else . end')"
fi

# Text content, joined, bounded. The complete reply stays in the action
# artifact; only the excerpt travels as the operation result.
text="$(printf '%s' "$reply" |
  jq -r '[.result.content[]? | select(.type=="text") | .text] | join("\n")')"
[ -n "$text" ] || text="(the tool returned no text content)"
if [ "${#text}" -gt "$RESULT_MAX" ]; then
  text="${text:0:$RESULT_MAX}
(truncated at ${RESULT_MAX} bytes; the full reply is in this action's artifact)"
fi
jq -cn --arg t "$text" '{events:[],result:{text:$t},error:null}'
