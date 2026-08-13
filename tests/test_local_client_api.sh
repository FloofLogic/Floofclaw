#!/usr/bin/env bash
set -euo pipefail

port="$(./bin/fclaw internal free-port)"
token="local-test-token-0123456789"
gateway_pid=""

cleanup() {
  if [[ -n "$gateway_pid" ]]; then
    kill "$gateway_pid" 2>/dev/null || true
    wait "$gateway_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

stop_gateway() {
  if [[ -n "$gateway_pid" ]]; then
    kill "$gateway_pid" 2>/dev/null || true
    wait "$gateway_pid" 2>/dev/null || true
    gateway_pid=""
  fi
}

start_gateway() {
  ./bin/fclaw gateway run -a --floop hello \
    > workspace/logs/local_client_test_gateway.log 2>&1 &
  gateway_pid="$!"
}

printf '%s\n' \
  "{\"bot_name\":\"FloofClaw\",\"default_floop\":\"hello\",\"gateway\":{\"host\":\"127.0.0.1\",\"port\":$port},\"affairs\":{\"cross_context_read\":true},\"channels\":{\"ws\":{\"enabled\":true,\"path\":\"/v1/fchat\"},\"irc\":{\"enabled\":false},\"discord\":{\"enabled\":false}}}" \
  > config/floofclaw_config.json
mkdir -p workspace/logs
if ./bin/fclaw gateway run -a --floop hello \
     > workspace/logs/missing_token_gateway.log 2>&1; then
  echo "local client listener started without its required token" >&2
  exit 1
fi
if ! grep -q "missing local client token" \
      workspace/logs/missing_token_gateway.log; then
  echo "missing-token startup failure did not name its provisioning fix" >&2
  exit 1
fi
printf '%s' "$token" | ./bin/fclaw auth set-stdin -a local_client_api >/dev/null

export LLM_MOCK_RESPONSE='{"message":"streamed hello from FloofClaw"}'
export FCLAW_MOCK_STREAM_CHUNK_BYTES=1
export FCLAW_MOCK_STREAM_DELAY_MS=3
export FCLAW_GATEWAY_POLL_MS=5

start_gateway

if ! ./bin/fclaw_local_client_probe "$port" "$token"; then
  sed -n '1,240p' workspace/logs/local_client_test_gateway.log >&2 || true
  find workspace/runs -type f -maxdepth 5 \
    -exec sh -c 'echo "$1"; sed -n "1,200p" "$1"' _ {} \; >&2
  exit 1
fi

provider_calls="$(find workspace/runs -path '*/provider_calls/*_meta.json' -type f | wc -l | tr -d ' ')"
if [[ "$provider_calls" != "2" ]]; then
  echo "duplicate message id started another model call: $provider_calls calls" >&2
  exit 1
fi
if ! grep -R -q '"type":"run_canceled"' workspace/runs/*/event_log.jsonl; then
  echo "cancel did not append durable run_canceled" >&2
  exit 1
fi
if [[ -f workspace/logs/deliveries.jsonl ]] &&
   grep '"corr_id":"msg_cancel"' workspace/logs/deliveries.jsonl |
     grep -qv '"request_id":"run_failed"'; then
  echo "canceled reply was committed as a successful delivery" >&2
  exit 1
fi
if grep -R -F -q "$token" workspace .fclaw 2>/dev/null; then
  echo "local client token leaked into runtime diagnostics or artifacts" >&2
  exit 1
fi

stop_gateway
./bin/fclaw clear -a --yes >/dev/null
mkdir -p workspace/logs
export FCLAW_DISABLE_LLM_STREAM=1
export LLM_MOCK_RESPONSE='{"message":"complete fallback reply"}'
start_gateway
./bin/fclaw_local_client_probe "$port" "$token" fallback
provider_calls="$(find workspace/runs -path '*/provider_calls/*_meta.json' -type f | wc -l | tr -d ' ')"
if [[ "$provider_calls" != "1" ]]; then
  echo "fallback path did not make exactly one provider call: $provider_calls" >&2
  exit 1
fi

stop_gateway
./bin/fclaw clear -a --yes >/dev/null
mkdir -p workspace/logs
unset FCLAW_DISABLE_LLM_STREAM
export LLM_MOCK_RESPONSE='not-json'
start_gateway
./bin/fclaw_local_client_probe "$port" "$token" error
if ! grep -R -q '"type":"run_failed"' workspace/runs/*/event_log.jsonl; then
  echo "provider/normalization failure did not fail the run" >&2
  exit 1
fi
if [[ -s workspace/logs/deliveries.jsonl ]] &&
   grep -qv '"request_id":"run_failed"' workspace/logs/deliveries.jsonl; then
  echo "failed reply created a successful committed delivery" >&2
  exit 1
fi

stop_gateway
./bin/fclaw clear -a --yes >/dev/null
mkdir -p workspace/logs
long_body=""
for ((i = 0; i < 3000; ++i)); do
  long_body+="x"
done
export LLM_MOCK_RESPONSE="{\"message\":\"$long_body\"}"
export FCLAW_MOCK_STREAM_DELAY_MS=0
start_gateway
./bin/fclaw_local_client_probe "$port" "$token" overflow
if [[ ! -s workspace/logs/deliveries.jsonl ]] ||
   ! grep -qv '"request_id":"run_failed"' workspace/logs/deliveries.jsonl; then
  echo "ephemeral overflow lost the authoritative successful delivery" >&2
  exit 1
fi

echo "local client API v1 lifecycle probes passed"
