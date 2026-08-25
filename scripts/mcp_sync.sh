#!/usr/bin/env bash
# Turn every configured MCP server's tool catalog into ordinary FloofClaw
# actions.
#
# This is generation, not integration: an MCP tools/list entry maps onto
# action.json nearly field for field, so the kernel never learns the word
# MCP. No new executor, no dlopen, no protocol code in the binary. What
# comes out is a directory of actions that floop allowlists, force_disable,
# timeouts, and action artifacts already govern.
#
# Explicit and idempotent. It rewrites actions/mcp/ from the servers'
# current catalogs, reports what changed, and never runs at gateway
# startup: between syncs the on-disk catalog is the truth, exactly like
# every other action.
set -uo pipefail

CONFIG="${FCLAW_MCP_CONFIG:-config/mcp.json}"
OUT_DIR="${FCLAW_MCP_ACTIONS_DIR:-actions/mcp}"
PROTOCOL="2024-11-05"
LIST_TIMEOUT_S="${FCLAW_MCP_LIST_TIMEOUT_S:-30}"
DESC_MAX=512
dry_run=0
as_json=0

die() {
  if [ "${as_json:-0}" = 1 ]; then
    jq -cn --arg m "$*" '{ok:false,error:$m}' 2>/dev/null \
      || printf '{"ok":false,"error":"mcp sync failed"}\n'
  fi
  printf 'mcp-sync: %s\n' "$*" >&2
  exit 1
}
# Progress chatter is for an operator watching; agent mode gets one object.
note() { [ "$as_json" = 1 ] || printf '  %s\n' "$*"; }

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dry-run) dry_run=1; shift ;;
    --json) as_json=1; shift ;;
    -h|--help)
      printf 'usage: %s [--dry-run] [--json]\n\n' "$0"
      printf 'Reads %s and regenerates %s.\n' "$CONFIG" "$OUT_DIR"
      printf '--json emits one result object instead of operator text.\n'
      exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
done

command -v jq >/dev/null 2>&1 || die "mcp sync requires jq"
[ -f "$CONFIG" ] || die "$CONFIG is missing; copy config/mcp.example.json and edit it"
jq -e '.mcpServers | type == "object"' "$CONFIG" >/dev/null 2>&1 \
  || die "$CONFIG has no mcpServers object"

# A tool id must survive as an action id and a directory name.
safe_token() { printf '%s' "$1" | grep -Eq '^[A-Za-z0-9_.-]{1,48}$'; }

# Run a child with a bound, feeding it infile and capturing outfile.
# macOS has no coreutils `timeout`, and a hung MCP server must fail this one
# call fast rather than sit on the action's whole deadline. The redirections
# belong to the child, not to this function: bash hands an async command
# /dev/null for stdin no matter how its caller was redirected.
# Returns 124 when it had to kill the child.
run_bounded() {
  local secs="$1" infile="$2" outfile="$3"; shift 3
  local pid waited monitor=0
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
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$secs" ]; then
      kill -TERM -"$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null
      sleep 1
      kill -KILL -"$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return 124
    fi
    sleep 1
    waited=$((waited + 1))
  done
  wait "$pid"
}

# Ask one server for its catalog. Prints the tools/list result JSON.
list_tools() {
  local server="$1" spec="$2" tmp_in tmp_out url body
  local -a rest=() env_pairs=()
  local init
  init="$(jq -cn --arg p "$PROTOCOL" \
    '{protocolVersion:$p,capabilities:{},clientInfo:{name:"floofclaw",version:"1"}}')"
  if printf '%s' "$spec" | jq -e 'has("url")' >/dev/null 2>&1; then
    command -v curl >/dev/null 2>&1 || die "http transport requires curl"
    url="$(printf '%s' "$spec" | jq -r '.url')"
    body="$(jq -cn --argjson p "$init" '{jsonrpc:"2.0",id:1,method:"initialize",params:$p}')"
    curl -sS -m 30 -X POST -H 'Content-Type: application/json' \
         --data-binary "$body" "$url" >/dev/null 2>&1 || return 1
    body='{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
    curl -sS -m 30 -X POST -H 'Content-Type: application/json' \
         --data-binary "$body" "$url" 2>/dev/null | jq -c 'select(.id==2)' | head -n 1
    return 0
  fi
  local cmd; cmd="$(printf '%s' "$spec" | jq -r '.command // empty')"
  [ -n "$cmd" ] || return 1
  while IFS= read -r a; do rest+=("$a"); done < <(printf '%s' "$spec" | jq -r '(.args // [])[]')
  while IFS= read -r key; do
    [ -n "$key" ] || continue
    local raw name value
    raw="$(printf '%s' "$spec" | jq -r --arg k "$key" '.env[$k]')"
    case "$raw" in
      '${'*'}') name="${raw#\$\{}"; name="${name%\}}"; value="${!name-}" ;;
      *) value="$raw" ;;
    esac
    env_pairs+=("$key=$value")
  done < <(printf '%s' "$spec" | jq -r '(.env // {}) | keys[]')
  tmp_in="$(mktemp)"; tmp_out="$(mktemp)"
  {
    jq -cn --argjson p "$init" '{jsonrpc:"2.0",id:1,method:"initialize",params:$p}'
    jq -cn '{jsonrpc:"2.0",method:"notifications/initialized"}'
    printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
  } > "$tmp_in"
  if [ "${#env_pairs[@]}" -gt 0 ]; then
    run_bounded "$LIST_TIMEOUT_S" "$tmp_in" "$tmp_out" \
      env "${env_pairs[@]}" "$cmd" ${rest+"${rest[@]}"}
  else
    run_bounded "$LIST_TIMEOUT_S" "$tmp_in" "$tmp_out" \
      "$cmd" ${rest+"${rest[@]}"}
  fi
  if [ "$?" = 124 ]; then rm -f "$tmp_in" "$tmp_out"; return 1; fi
  jq -c 'select(.id==2)' < "$tmp_out" 2>/dev/null | head -n 1
  rm -f "$tmp_in" "$tmp_out"
}

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
generated=0
failed_servers=()

while IFS= read -r server; do
  [ -n "$server" ] || continue
  if ! safe_token "$server"; then
    die "server name '$server' is not [A-Za-z0-9_.-]{1,48}"
  fi
  spec="$(jq -c --arg s "$server" '.mcpServers[$s]' "$CONFIG")"
  reply="$(list_tools "$server" "$spec")"
  if [ -z "$reply" ] || ! printf '%s' "$reply" | jq -e '.result.tools | type=="array"' >/dev/null 2>&1; then
    failed_servers+=("$server")
    note "server $server: no tool catalog (skipped; its existing actions are left alone)"
    continue
  fi
  count="$(printf '%s' "$reply" | jq '.result.tools | length')"
  note "server $server: $count tool(s)"
  for i in $(seq 0 $((count - 1))); do
    tool="$(printf '%s' "$reply" | jq -r --argjson i "$i" '.result.tools[$i].name')"
    if ! safe_token "$tool"; then
      note "  skipped tool '$tool' (name is not [A-Za-z0-9_.-]{1,48})"
      continue
    fi
    id="${server}__${tool}"
    dir="$staging/$id"
    mkdir -p "$dir"
    # description: the tool's own, truncated to the manifest convention and
    # prefixed so a model can see which server it came from. Untrusted text
    # like any other action description.
    printf '%s' "$reply" | jq \
      --argjson i "$i" --arg id "$id" --arg server "$server" --argjson max "$DESC_MAX" '
      .result.tools[$i] as $t
      | ($t.description // "No description provided by the server.") as $d
      | ("MCP tool " + $t.name + " from server " + $server + ". " + $d) as $full
      | {
          id: $id,
          description: (if ($full | length) > $max then ($full[0:$max-1] + "…") else $full end),
          outside_world: true,
          args_schema: (
            ($t.inputSchema // {type:"object"})
            | if type=="object" then . else {type:"object"} end
            | .properties = ((.properties // {}) + {
                op: {type:"string", enum:["start"],
                     description:"Operation to perform. '\''start'\'' calls the tool and returns its result on a later turn."}
              })
            | .required = (((.required // []) + ["op"]) | unique)
          ),
          exec: ["bash","run.sh"],
          timeout_ms: 60000,
          contract: "managed_operation",
          default_timeout_ms: 60000,
          max_timeout_ms: 300000
        }' > "$dir/action.json"
    cat > "$dir/run.sh" <<RUNSH
#!/usr/bin/env bash
# Generated by scripts/mcp_sync.sh — do not edit; re-run the sync instead.
exec "\$(dirname "\$0")/../_bridge.sh" '$server' '$tool'
RUNSH
    chmod +x "$dir/run.sh"
    generated=$((generated + 1))
  done
done < <(jq -r '.mcpServers | keys[]' "$CONFIG")

# Report the difference before touching anything.
added=(); changed=(); removed=()
mkdir -p "$OUT_DIR"
for dir in "$staging"/*/; do
  [ -d "$dir" ] || continue
  id="$(basename "$dir")"
  if [ ! -d "$OUT_DIR/$id" ]; then added+=("$id")
  elif ! cmp -s "$dir/action.json" "$OUT_DIR/$id/action.json"; then changed+=("$id")
  fi
done
for dir in "$OUT_DIR"/*/; do
  [ -d "$dir" ] || continue
  id="$(basename "$dir")"
  # A server that failed to answer keeps its actions: a transient outage
  # must not silently delete a deployment's tool catalog.
  server="${id%%__*}"
  skip=0
  for f in ${failed_servers+"${failed_servers[@]}"}; do [ "$f" = "$server" ] && skip=1; done
  [ "$skip" = 1 ] && continue
  [ -d "$staging/$id" ] || removed+=("$id")
done

json_array() { printf '%s\n' ${1+"$@"} | jq -R . | jq -sc 'map(select(length > 0))'; }

report() {
  local applied="$1"
  if [ "$as_json" = 1 ]; then
    jq -cn --argjson tools "$generated" --arg config "$CONFIG" \
      --argjson applied "$applied" --argjson dry_run "$dry_run" \
      --argjson added   "$(json_array ${added+"${added[@]}"})" \
      --argjson changed "$(json_array ${changed+"${changed[@]}"})" \
      --argjson removed "$(json_array ${removed+"${removed[@]}"})" \
      --argjson unreachable "$(json_array ${failed_servers+"${failed_servers[@]}"})" \
      '{ok:(($unreachable|length)==0),config:$config,tools:$tools,applied:$applied,
        dry_run:($dry_run==1),added:$added,changed:$changed,removed:$removed,
        unreachable:$unreachable}'
    return
  fi
  printf 'mcp-sync: %d tool(s) from %s\n' "$generated" "$CONFIG"
  for id in ${added+"${added[@]}"};   do printf '  + %s\n' "$id"; done
  for id in ${changed+"${changed[@]}"}; do printf '  ~ %s\n' "$id"; done
  for id in ${removed+"${removed[@]}"}; do printf '  - %s\n' "$id"; done
  [ "${#added[@]}" -eq 0 ] && [ "${#changed[@]}" -eq 0 ] && [ "${#removed[@]}" -eq 0 ] &&
    printf '  (no change)\n'
}

if [ "$dry_run" = 1 ]; then
  report false
  [ "$as_json" = 1 ] ||
    printf 'mcp-sync: dry run; %s was not modified\n' "$OUT_DIR"
  exit 0
fi

for id in ${removed+"${removed[@]}"}; do rm -rf "${OUT_DIR:?}/$id"; done
for dir in "$staging"/*/; do
  [ -d "$dir" ] || continue
  id="$(basename "$dir")"
  mkdir -p "$OUT_DIR/$id"
  cp "$dir/action.json" "$OUT_DIR/$id/action.json"
  cp "$dir/run.sh" "$OUT_DIR/$id/run.sh"
  chmod +x "$OUT_DIR/$id/run.sh"
done

report true

if [ "${#failed_servers[@]}" -gt 0 ]; then
  printf 'mcp-sync: %d server(s) did not answer: %s\n' \
    "${#failed_servers[@]}" "${failed_servers[*]}" >&2
  printf 'mcp-sync: their existing actions were left in place; fix the server and re-run\n' >&2
fi
[ "$as_json" = 1 ] ||
  printf 'mcp-sync: restart the gateway to pick up the catalog\n'
[ "${#failed_servers[@]}" -eq 0 ] || exit 1
exit 0
