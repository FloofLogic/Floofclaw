#!/usr/bin/env bash
# Hermetic MCP server over stdio. Speaks just enough of the protocol to
# prove the bridge and the generator: initialize, tools/list, tools/call.
#
# Behaviour is steered by the environment so one fixture covers every case:
#   MCP_FIXTURE_MODE=ok|error|hang|garbage|notools
#   MCP_FIXTURE_SECRET   echoed back in the tool result when set
set -uo pipefail
mode="${MCP_FIXTURE_MODE:-ok}"

[ "$mode" = "hang" ] && { sleep 300; exit 0; }

tools_json='[
  {"name":"echo","description":"Echo the text back.",
   "inputSchema":{"type":"object","required":["text"],"additionalProperties":false,
     "properties":{"text":{"type":"string","description":"Text to echo."}}}},
  {"name":"add","description":"Add two numbers.",
   "inputSchema":{"type":"object","required":["a","b"],
     "properties":{"a":{"type":"number"},"b":{"type":"number"}}}}
]'
[ "$mode" = "notools" ] && tools_json='[]'

while IFS= read -r line; do
  [ -n "$line" ] || continue
  if [ "$mode" = "garbage" ]; then printf 'this is not json\n'; continue; fi
  id="$(printf '%s' "$line" | jq -r '.id // empty' 2>/dev/null)"
  method="$(printf '%s' "$line" | jq -r '.method // empty' 2>/dev/null)"
  [ -n "$id" ] || continue          # notification: no reply
  case "$method" in
    initialize)
      jq -cn --argjson id "$id" \
        '{jsonrpc:"2.0",id:$id,result:{protocolVersion:"2024-11-05",capabilities:{},
          serverInfo:{name:"fixture",version:"1"}}}' ;;
    tools/list)
      jq -cn --argjson id "$id" --argjson t "$tools_json" \
        '{jsonrpc:"2.0",id:$id,result:{tools:$t}}' ;;
    tools/call)
      name="$(printf '%s' "$line" | jq -r '.params.name')"
      args="$(printf '%s' "$line" | jq -c '.params.arguments // {}')"
      if [ "$mode" = "error" ]; then
        jq -cn --argjson id "$id" \
          '{jsonrpc:"2.0",id:$id,result:{isError:true,
            content:[{type:"text",text:"the tool refused: bad input"}]}}'
      elif [ "$name" = "echo" ]; then
        text="$(printf '%s' "$args" | jq -r '.text // ""')"
        [ -n "${MCP_FIXTURE_SECRET-}" ] && text="$text|secret=${MCP_FIXTURE_SECRET}"
        jq -cn --argjson id "$id" --arg t "$text" \
          '{jsonrpc:"2.0",id:$id,result:{content:[{type:"text",text:$t}]}}'
      elif [ "$name" = "add" ]; then
        sum="$(printf '%s' "$args" | jq -r '(.a // 0) + (.b // 0)')"
        jq -cn --argjson id "$id" --arg t "$sum" \
          '{jsonrpc:"2.0",id:$id,result:{content:[{type:"text",text:$t}]}}'
      else
        jq -cn --argjson id "$id" --arg n "$name" \
          '{jsonrpc:"2.0",id:$id,error:{code:-32601,message:("no such tool: "+$n)}}'
      fi ;;
    *) jq -cn --argjson id "$id" \
         '{jsonrpc:"2.0",id:$id,error:{code:-32601,message:"method not found"}}' ;;
  esac
done
