#!/usr/bin/env bash
set -euo pipefail

action_dir="$(cd "$(dirname "$0")" && pwd)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/http-call-test.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/workspace"

cat >"$tmp/bin/curl" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
: >"$CAPTURE_ARGS"
output=""
config=""
while [ "$#" -gt 0 ]; do
  printf '%s\n' "$1" >>"$CAPTURE_ARGS"
  case "$1" in
    --output) output="$2"; printf '%s\n' "$2" >>"$CAPTURE_ARGS"; shift 2 ;;
    --config) config="$2"; printf '%s\n' "$2" >>"$CAPTURE_ARGS"; shift 2 ;;
    --request|--header|--connect-timeout|--max-time|--proto|--write-out|--data-binary)
      printf '%s\n' "$2" >>"$CAPTURE_ARGS"; shift 2 ;;
    *) shift ;;
  esac
done
cat >"$CAPTURE_STDIN"
[ -z "$config" ] || cp "$config" "$CAPTURE_CONFIG"
printf '%s' "$MOCK_BODY" >"$output"
printf '%s' "$MOCK_STATUS"
exit "${MOCK_CURL_RC:-0}"
SH
chmod +x "$tmp/bin/curl"

export PATH="$tmp/bin:$PATH"
export FCLAW_WORKSPACE_ROOT="$tmp/workspace"
export CAPTURE_ARGS="$tmp/curl.args"
export CAPTURE_STDIN="$tmp/curl.stdin"
export CAPTURE_CONFIG="$tmp/curl.config"
export CMS_API_TOKEN='fixture-secret-token'
export MOCK_BODY='{"accepted":true,"record_id":"rec_10"}'
export MOCK_STATUS=202

request='{"request_id":"actionreq_wo1_001","action":"http_call","args":{"op":"start","method":"POST","url":"http://cms.example.test/v1/records","headers":["Content-Type: application/json","X-Trace: wo1"],"body":"{\"name\":\"Mox\"}","auth_ref":"CMS_API_TOKEN"},"artifacts":{"body":"runs/run_contract/action_calls/001_http_call.body.txt"}}'
output="$(printf '%s' "$request" | "$action_dir/run.sh")"

[ "$(jq -r '.error' <<<"$output")" = "null" ] || { printf 'FAIL start: %s\n' "$output" >&2; exit 1; }
[ "$(jq -r '.result.status' <<<"$output")" = "finished" ] || { printf 'FAIL managed status\n' >&2; exit 1; }
[ "$(jq -r '.result.http_status' <<<"$output")" = "202" ] || { printf 'FAIL HTTP status\n' >&2; exit 1; }
[ "$(jq -r '.result.body' <<<"$output")" = "$MOCK_BODY" ] || { printf 'FAIL response body\n' >&2; exit 1; }
[ "$(cat "$CAPTURE_STDIN")" = '{"name":"Mox"}' ] || { printf 'FAIL request body\n' >&2; exit 1; }
grep -q 'Authorization: Bearer fixture-secret-token' "$CAPTURE_CONFIG" || { printf 'FAIL auth config\n' >&2; exit 1; }
! grep -q 'fixture-secret-token' "$CAPTURE_ARGS" || { printf 'FAIL secret leaked to curl argv\n' >&2; exit 1; }
grep -q 'Content-Type: application/json' "$CAPTURE_ARGS" || { printf 'FAIL content header\n' >&2; exit 1; }
grep -q 'X-Trace: wo1' "$CAPTURE_ARGS" || { printf 'FAIL trace header\n' >&2; exit 1; }
[ "$(cat "$tmp/workspace/runs/run_contract/action_calls/001_http_call.body.txt")" = "$MOCK_BODY" ] || { printf 'FAIL body artifact\n' >&2; exit 1; }

export MOCK_BODY='{"error":"missing"}' MOCK_STATUS=404
request_404='{"request_id":"actionreq_wo1_404","action":"http_call","args":{"op":"start","method":"GET","url":"https://cms.example.test/missing"},"artifacts":{"body":"runs/run_contract/action_calls/002_http_call.body.txt"}}'
output_404="$(printf '%s' "$request_404" | "$action_dir/run.sh")"
[ "$(jq -r '.result.http_status' <<<"$output_404")" = "404" ] || { printf 'FAIL 404 should be a returned HTTP outcome\n' >&2; exit 1; }

bad_auth='{"request_id":"actionreq_wo1_bad","action":"http_call","args":{"op":"start","method":"GET","url":"https://cms.example.test/","auth_ref":"NOT-A-NAME"},"artifacts":{"body":"runs/run_contract/action_calls/003_http_call.body.txt"}}'
set +e
bad_output="$(printf '%s' "$bad_auth" | "$action_dir/run.sh")"; bad_rc=$?
set -e
[ "$bad_rc" -ne 0 ] && [ "$(jq -r '.error.code' <<<"$bad_output")" = "BAD_ARGS" ] || { printf 'FAIL invalid auth_ref\n' >&2; exit 1; }

inline_auth='{"request_id":"actionreq_wo1_inline","action":"http_call","args":{"op":"start","method":"GET","url":"https://cms.example.test/","headers":["Authorization: Bearer must-not-log"]},"artifacts":{"body":"runs/run_contract/action_calls/004_http_call.body.txt"}}'
set +e
inline_output="$(printf '%s' "$inline_auth" | "$action_dir/run.sh")"; inline_rc=$?
set -e
[ "$inline_rc" -ne 0 ] && [ "$(jq -r '.error.code' <<<"$inline_output")" = "BAD_ARGS" ] || { printf 'FAIL inline credential header\n' >&2; exit 1; }

MOCK_BODY=""
for ((i = 0; i < 500; i++)); do MOCK_BODY="${MOCK_BODY}0123456789"; done
export MOCK_BODY MOCK_STATUS=200
large_request='{"request_id":"actionreq_wo1_large","action":"http_call","args":{"op":"start","method":"GET","url":"https://cms.example.test/large"},"artifacts":{"body":"runs/run_contract/action_calls/005_http_call.body.txt"}}'
large_output="$(printf '%s' "$large_request" | "$action_dir/run.sh")"
[ "$(jq -r '.result.truncated' <<<"$large_output")" = "true" ] || { printf 'FAIL large body truncation marker\n' >&2; exit 1; }
[ "$(printf '%s' "$(jq -c '.result' <<<"$large_output")" | wc -c | tr -d ' ')" -le 3500 ] || { printf 'FAIL result budget\n' >&2; exit 1; }
[ "$(wc -c <"$tmp/workspace/runs/run_contract/action_calls/005_http_call.body.txt" | tr -d ' ')" -eq 5000 ] || { printf 'FAIL full large body artifact\n' >&2; exit 1; }

printf 'PASS http_call contract: method, headers, body, env-only auth, bounded result, artifact, HTTP 404\n'
