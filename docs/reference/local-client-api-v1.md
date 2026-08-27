# Local client API v1

FloofClaw exposes one authenticated, loopback-only listener for local app
clients. It is deliberately a fixed route table, not a general HTTP server:

| Method | Path | Response |
|---|---|---|
| `GET` | `/v1/health` | Process readiness, build identity, active work, and capabilities |
| `GET` | `/v1/pulse` | The versioned Pulse read model |
| `GET` | `/v1/usage` | Current usage-log-segment totals grouped by agent, profile, and model |
| `GET` | `/v1/usage/records` | Chunked NDJSON over current usage ledger records |
| `GET` | `/v1/cacheview/records?agent=<id>&n=<1..10>` | Chunked NDJSON over selected numbered provider-call artifacts |
| `GET` + WebSocket upgrade | `/v1/fchat` | FloofClawWS `1.0` streamed chat |

The listener uses `gateway.host` and `gateway.port`; `host` must be
`127.0.0.1` or `localhost`. Enable it with `channels.ws.enabled: true`.
Changing a field's meaning or adding a required field requires a new API or
protocol version. Optional response fields may be added within v1.

## Authentication

Create a high-entropy token in the mode-restricted secret store:

```bash
printf '%s' "$LOCAL_CLIENT_TOKEN" | ./bin/fclaw auth set-stdin -h local_client_api
```

The token is not configuration. Do not put it in a URL, source file, event,
trace, or log.

HTTP requests use `Authorization: Bearer <token>`. A WebSocket upgrade does
not carry the token in its URL; its first application frame must be an
authenticated `hello`. An unauthenticated connection receives no state and
cannot publish a message. Rotating the stored token requires reconnecting
clients.

## HTTP response models

All successful models contain `"schema_version": 1`.

`/v1/health` contains `ready`, `running`, `pid`, `floop`, API and chat
versions, active run/job counts, build version/revision, and capabilities.

`/v1/pulse` is produced by the same C projection used by
`app/pulse/generate.py`. It contains `generated_at`, `now_ms`, `bot_name`,
gateway state, affairs, tasks and archive count, operations, interleaved
messages, recent runs, warnings, and explicit collection/truncation limits.
The default limit is 40 messages and 25 runs.

`/v1/usage` covers only the current `workspace/logs/llm_usage.jsonl` segment;
rotated history is explicitly excluded. Its `rows` key is grouped by the
complete `(agent, profile, provider, model)` tuple. Totals and rows include
dated token-price estimates: `estimated_usd` is numeric only with complete
pricing coverage, while the priced partial, priced/unpriced call counts, and
`pricing_complete` remain explicit.

The record routes use `Content-Type: application/x-ndjson` with chunked
transfer. Their first line is a versioned `scope` record naming the active
floop and every configured LLM agent, including zero-call agents. Remaining
lines come directly from existing files:

- `/v1/usage/records` emits `usage_record` lines from the current
  `llm_usage.jsonl` segment. `?agent=<id>` filters them.
- `/v1/cacheview/records` emits `cacheview_call` lines from numbered
  `*_meta.json` and `*_raw_request.json` artifacts. `agent` is required;
  `n` selects 1–10 recent calls, and `run=<run_id>&call=<seq>` selects an exact
  call. `call` requires `run`.

These routes do not calculate prices, cache ratios, heatmaps, or chart series,
and they do not maintain an index or second cache. Each response advances one
record at a time while the connection drains.

```bash
curl -H "Authorization: Bearer $LOCAL_CLIENT_TOKEN" \
  http://127.0.0.1:41004/v1/usage/records
curl -H "Authorization: Bearer $LOCAL_CLIENT_TOKEN" \
  'http://127.0.0.1:41004/v1/cacheview/records?agent=chat_manager&n=10'
```

Errors are JSON:

```json
{"error":{"code":"UNAUTHORIZED","message":"A valid bearer token is required."}}
```

The fixed parser accepts no request body, caps the request head at 16 KiB,
caps queued output per connection at approximately 512 KiB, and closes a
request that makes no parse progress for 30 seconds. It serves one response
per HTTP connection with `Cache-Control: no-store`; record routes avoid the
queue cap by pumping bounded records as the connection drains.

## FloofClawWS 1.0

Client frames are `hello`, `message`, `cancel`, and `ping`. Server frames are
`hello_ack`, `status`, `reply_chunk`, `reply_done`, `error`, and `pong`.

```json
{"type":"hello","protocol":"FloofClawWS","version":"1.0","token":"<secret>","client_id":"floofforge","session_id":"sess_1","after_delivery_id":"deliv_12"}
{"type":"hello_ack","protocol":"FloofClawWS","version":"1.0","session_id":"sess_1","capabilities":["chat_stream","resume","cancel"]}
{"type":"message","id":"msg_1","ctx_id":"main","body":"Hello"}
{"type":"status","corr_id":"msg_1","state":"accepted","run_id":"run_13"}
{"type":"reply_chunk","corr_id":"msg_1","seq":0,"delta":"Hi"}
{"type":"reply_chunk","corr_id":"msg_1","seq":1,"delta":" there"}
{"type":"reply_done","corr_id":"msg_1","run_id":"run_13","delivery_id":"deliv_13","body":"Hi there"}
```

`session_id` and `message.id` form the stable correlation. `seq` starts at
zero and increases by one. Chunks are ephemeral presentation; the ordinary
validated `message` action and committed delivery remain truth.
`reply_done.body` is authoritative and clients must replace their optimistic
stream with it. On a successful streamed reply:

```text
concat(reply_chunk.delta) == reply_done.body
```

A non-streaming provider sends one complete `reply_chunk`, then the same
`reply_done`. If streamed text disagrees with the committed delivery, the
server sends `STREAM_MISMATCH` and never sends `reply_done`.

Reconnect with the same `session_id` and the last committed
`after_delivery_id`. FloofClaw replays later retained terminal deliveries;
it does not replay provider work or ephemeral chunks. An unknown/expired
cursor returns `CURSOR_EXPIRED`.

Cancel active work with:

```json
{"type":"cancel","corr_id":"msg_1"}
```

Cancellation is best effort, but its terminal client outcome is
`error.code = "CANCELED"` and its durable runtime outcome is
`run_canceled`; it never also returns `reply_done`.

Other stable WebSocket error codes are `UNAUTHORIZED`, `BAD_REQUEST`,
`UNSUPPORTED_TYPE`, `BUSY`, `NOT_FOUND`, `DUPLICATE_ID`,
`CURSOR_EXPIRED`, `STREAM_DROPPED`, `STREAM_MISMATCH`, `PROVIDER_ERROR`,
`NO_REPLY`, and `INTERNAL`. HTTP errors use the applicable
`BAD_REQUEST`, `REQUEST_BODY_NOT_ALLOWED`, `UNAUTHORIZED`, `NOT_FOUND`,
`METHOD_NOT_ALLOWED`, `UPGRADE_REQUIRED`, `BAD_UPGRADE`,
`REQUEST_TIMEOUT`, `REQUEST_TOO_LARGE`, or projection/allocation failure
code. Error messages never contain raw provider or model output.

The implementation caps active correlations at 64, clients at 16, a
WebSocket receive buffer at 16 KiB, progress records at 4 KiB, and the
jobrunner progress queue at 64 records. Presentation overflow may drop or
close a slow stream; it does not invalidate the durable run or delivery.
