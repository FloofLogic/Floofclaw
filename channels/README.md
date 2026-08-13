# `channels/` — external delivery bridges

Native channel adapters live under `adapters/` (Discord, IRC, and WebSocket)
and are statically linked into `bin/fclaw`. Shared synchronous channel helpers
live in `runtime/channel/`. This directory contains an optional script-based
last-mile bridge for transports the runtime does not speak natively: iMessage
and email.

## How a reply reaches a channel

When any agent emits a `message`, the runtime appends a delivery record to
`workspace/logs/deliveries.jsonl`, tagged with the origin `channel` and its
`ref` (the reply address — the same field Discord uses for its `channel_id`).
Each native adapter or external bridge consumes deliveries for its own channel
and performs the last-mile send. The `message` action is channel-agnostic: an
agent does not choose a transport or recipient. The delivery follows the
conversation origin, including across a `work` or operation hop because the
reply-routing `ref` is preserved.

```
agent `message` -> deliveries.jsonl {channel, ref, text} -> adapter/bridge -> send
```

## message_deliver.sh

Tails `deliveries.jsonl` and delivers `imessage` / `email` records:

- **iMessage** → pipes the text to `imessage-send.sh` on the Mac over ssh,
  addressed to `ref.handle`. Needs `IMESSAGE_SSH_HOST`, `FLIMESSAGE_REMOTE`
  (path to `imessage-send.sh` on the Mac), optional `IMESSAGE_SSH_BIN`.
- **email** → `floofmail` reply (`ref.reply_to_id`) or fresh send
  (`ref.to` / `ref.subject`). Needs `FLOOFMAIL_CMD`, `FLOOFMAIL_PROFILE`.

A channel is handled only when its configuration is present. Other channels
(`discord`, `irc`, and `cli`) are ignored because their native paths own
delivery. The cursor (`workspace/logs/.message_deliver_cursor`) prevents
ordinary reprocessing after a clean batch, but the bridge is best-effort, not
exactly-once: failed sends are not retried, and a crash after external
acceptance but before the cursor write can resend records from that batch.

```sh
./channels/message_deliver.sh --once     # process new deliveries, then exit
./channels/message_deliver.sh --follow   # follow the log (run as a service)
```

Run it from the bot's deployment directory (where `deliveries.jsonl` lives),
alongside the gateway. The intake half (turning incoming messages into bus
events, and tagging the sender in `ref` via `fclaw bus publish -a --ref`) lives
with each separately installed source integration.

The hermetic test stubs SSH and `floofmail` and uses no network:

```sh
./channels/message_deliver.test.sh
```
