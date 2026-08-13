#!/usr/bin/env bash
set -euo pipefail

input="$(cat)"

emit_error() {
  jq -cn --arg code "$1" --arg message "$2" \
    '{events:[],result:{},error:{code:$code,message:$message}}'
  exit 1
}

command -v jq >/dev/null 2>&1 || {
  printf '%s\n' '{"events":[],"result":{},"error":{"code":"DEPENDENCY_MISSING","message":"http_call requires jq"}}'
  exit 1
}
command -v curl >/dev/null 2>&1 || emit_error "DEPENDENCY_MISSING" "http_call requires curl"
jq -e 'type == "object" and (.args | type == "object")' >/dev/null 2>&1 <<<"$input" ||
  emit_error "BAD_ARGS" "expected a FloofClaw action input envelope"

op="$(jq -r '.args.op // ""' <<<"$input")"
request_id="$(jq -r '.request_id // ""' <<<"$input")"
safe_id="$(printf '%s' "$request_id" | tr -cd '[:alnum:]_-')"
[ -n "$safe_id" ] || safe_id="request"
handle="http_${safe_id:0:80}"

if [ "$op" = "poll" ]; then
  supplied="$(jq -r '.args.handle // ""' <<<"$input")"
  [ -z "$supplied" ] || handle="$supplied"
  jq -cn --arg handle "$handle" \
    '{events:[],result:{status:"finished",handle:$handle,text:"http_call completes synchronously; call op:start."},error:null}'
  exit 0
fi
[ "$op" = "start" ] || emit_error "BAD_ARGS" "args.op must be start or poll"

method="$(jq -r '.args.method // ""' <<<"$input")"
url="$(jq -r '.args.url // ""' <<<"$input")"
case "$method" in GET|POST|PUT|PATCH|DELETE|HEAD|OPTIONS) ;; *) emit_error "BAD_ARGS" "args.method is required and must be an allowed uppercase HTTP method" ;; esac
case "$url" in http://*|https://*) ;; *) emit_error "BAD_ARGS" "args.url is required and must be absolute HTTP or HTTPS" ;; esac
authority="${url#*://}"; authority="${authority%%/*}"
case "$authority" in *@*) emit_error "BAD_ARGS" "credentials must not be embedded in args.url; use args.auth_ref" ;; esac

workspace_root="${FCLAW_WORKSPACE_ROOT:-}"
body_artifact="$(jq -r '.artifacts.body // ""' <<<"$input")"
[ -n "$workspace_root" ] || emit_error "CONFIG_ERROR" "FCLAW_WORKSPACE_ROOT is required"
case "$body_artifact" in ""|/*|..|../*|*/../*|*/..) emit_error "BAD_ARTIFACT" "artifacts.body must be a safe workspace-relative path" ;; esac
body_path="$workspace_root/$body_artifact"
mkdir -p "$(dirname "$body_path")" || emit_error "ARTIFACT_ERROR" "could not create response artifact directory"

header_count="$(jq -r '(.args.headers // []) | length' <<<"$input")"
[ "$header_count" -le 32 ] || emit_error "BAD_ARGS" "args.headers supports at most 32 entries"
curl_args=(--silent --show-error --proto '=http,https' --connect-timeout 10 --max-time 30 --request "$method" --output "$body_path" --write-out '%{http_code}')
for ((i = 0; i < header_count; i++)); do
  header="$(jq -r --argjson i "$i" '.args.headers[$i]' <<<"$input")"
  case "$header" in *$'\r'*|*$'\n'*|:*|*:) emit_error "BAD_ARGS" "headers must be non-empty 'Name: value' strings without newlines" ;; esac
  [[ "$header" == *:* ]] || emit_error "BAD_ARGS" "headers must be 'Name: value' strings"
  header_name="${header%%:*}"
  [[ "$header_name" =~ ^[A-Za-z0-9_-]+$ ]] || emit_error "BAD_ARGS" "header names may contain only letters, digits, hyphen, and underscore"
  header_name="$(printf '%s' "$header_name" | tr '[:upper:]' '[:lower:]')"
  case "$header_name" in authorization|proxy-authorization|cookie|set-cookie|x-api-key|api-key) emit_error "BAD_ARGS" "credential headers must not be placed in args.headers; use args.auth_ref" ;; esac
  curl_args+=(--header "$header")
done

auth_ref="$(jq -r '.args.auth_ref // ""' <<<"$input")"
config_path=""
cleanup() { [ -z "$config_path" ] || rm -f "$config_path"; }
trap cleanup EXIT
if [ -n "$auth_ref" ]; then
  [[ "$auth_ref" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || emit_error "BAD_ARGS" "args.auth_ref must be an environment-variable name"
  secret="${!auth_ref-}"
  [ -n "$secret" ] || emit_error "AUTH_MISSING" "the environment variable named by args.auth_ref is unset or empty"
  case "$secret" in *$'\r'*|*$'\n'*) emit_error "AUTH_INVALID" "the referenced bearer token contains a newline" ;; esac
  config_path="$(mktemp "${TMPDIR:-/tmp}/fclaw-http-call.XXXXXX")" || emit_error "CONFIG_ERROR" "could not create temporary curl config"
  chmod 600 "$config_path"
  escaped="${secret//\\/\\\\}"; escaped="${escaped//\"/\\\"}"
  printf 'header = "Authorization: Bearer %s"\n' "$escaped" >"$config_path"
  curl_args+=(--config "$config_path")
fi

has_body=false
body=""
if jq -e '.args | has("body")' >/dev/null <<<"$input"; then
  has_body=true
  body="$(jq -r '.args.body' <<<"$input")"
  body_bytes="$(LC_ALL=C printf '%s' "$body" | wc -c | tr -d ' ')"
  [ "$body_bytes" -le 65536 ] || emit_error "BAD_ARGS" "args.body exceeds 65536 bytes"
  curl_args+=(--data-binary @-)
fi

set +e
if [ "$has_body" = true ]; then
  http_status="$(printf '%s' "$body" | curl "${curl_args[@]}" "$url")"
  curl_rc=$?
else
  http_status="$(curl "${curl_args[@]}" "$url")"
  curl_rc=$?
fi
set -e
[ "$curl_rc" -eq 0 ] || emit_error "HTTP_CALL_FAILED" "curl exited $curl_rc"
[[ "$http_status" =~ ^[0-9]{3}$ ]] || emit_error "HTTP_PROTOCOL_ERROR" "curl did not return a three-digit HTTP status"

body_bytes="$(wc -c <"$body_path" | tr -d ' ')"
body_excerpt="$(LC_ALL=C head -c 1200 "$body_path")"
truncated=false
[ "$body_bytes" -le 1200 ] || truncated=true
url_display="${url:0:500}"
while :; do
  text="HTTP $method $url_display returned $http_status."
  [ -z "$body_excerpt" ] || text="$text
$body_excerpt"
  result="$(jq -cn --arg handle "$handle" --arg method "$method" --arg http_status "$http_status" \
    --arg url "$url_display" --arg body "$body_excerpt" --arg body_artifact "$body_artifact" --arg text "$text" \
    --argjson body_bytes "$body_bytes" --argjson truncated "$truncated" \
    '{status:"finished",handle:$handle,method:$method,url:$url,http_status:$http_status,body:$body,body_artifact:$body_artifact,body_excerpt:$body,body_bytes:$body_bytes,truncated:$truncated,text:$text}')"
  [ "${#result}" -le 3500 ] && break
  [ "${#body_excerpt}" -gt 0 ] || emit_error "RESULT_TOO_LARGE" "response metadata exceeds the managed-operation result budget"
  body_excerpt="${body_excerpt:0:${#body_excerpt}-128}"
  truncated=true
done
jq -cn --argjson result "$result" '{events:[],result:$result,error:null}'
