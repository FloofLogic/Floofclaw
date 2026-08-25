# Telegram

The Telegram adapter connects a BotFather bot to a FloofClaw deployment.
It is the simplest channel FloofClaw ships: plain HTTPS both ways, no
websocket, no heartbeat, no reconnect state machine to reason about.

**Preconditions:** a built `bin/fclaw` that contains the Telegram adapter
(see [Build the adapter in](#0-build-the-adapter-in)), and an OpenSSL-linked
build — the Bot API is HTTPS-only.

## 0. Build the adapter in

Channels are chosen at build time. Check what this binary contains:

```bash
./bin/fclaw adapters -a
```

If `telegram` is not listed, rebuild with it:

```bash
make FCLAW_ADAPTERS="ws telegram"
```

`ws` backs `fclaw -i` and the local client API, so keep it unless you know
you do not want them. `make FCLAW_ADAPTERS=all` builds every channel.
Enabling a channel this binary does not contain refuses startup and names
the rebuild — it never starts and quietly answers nothing.

## 1. Create the bot

Message [@BotFather](https://t.me/BotFather) on Telegram:

```text
/newbot
```

Answer the name and username prompts. BotFather replies with a token that
looks like `123456789:AAExampleExampleExampleExampleExample`. That token is
the whole credential — treat it like a password.

## 2. Store the token

The token goes in the auth store, never in config:

```bash
printf '%s' 'YOUR_BOT_TOKEN' | ./bin/fclaw auth set-stdin telegram_token
```

`config/floofclaw_config.json` carries only the *key name*
(`token_key: "telegram_token"`), so the config file stays safe to read,
diff, and share.

## 3. Find your chat IDs

Group traffic requires an explicit allowlist. To find a group's id, add the
bot to the group, send a message, and read the id from the adapter's own
refusal in `workspace/logs/narration.jsonl`:

```text
telegram: dropped a message from unallowed supergroup chat -1001234567890;
fix: add it to channels.telegram.allowed_chat_ids (or set allow_dms)
```

That refusal is the discovery mechanism: the bot tells you the id of the
room it declined to read.

## 4. Configure the adapter

```json
"telegram": {
  "enabled": true,
  "allow_dms": true,
  "allow_bots": false,
  "allowed_chat_ids": ["-1001234567890"],
  "token_key": "telegram_token",
  "tls_verify": true
}
```

| key | meaning |
|---|---|
| `enabled` | Connect the adapter at gateway start. |
| `allow_dms` | Accept private chats. With this false and no allowlist, startup refuses — the adapter would have nothing it may read. |
| `allow_bots` | Accept messages from other bots. Leave false. |
| `allowed_chat_ids` | Group and supergroup ids the bot may read and answer in, up to 16. Unlisted groups are refused and narrated. |
| `token_key` | Auth-store key holding the token. A key name, never the token. |
| `tls_verify` | Verify the `api.telegram.org` certificate. Leave true. |

`scripts/onboard.sh` walks all of this, and offers the rebuild when the
adapter is not in the binary yet.

## 5. Verify startup

```bash
./bin/fclaw gateway run -h --floop hello
```

Then message the bot. A working setup shows the inbound envelope and the
outbound delivery:

```bash
./bin/fclaw bus log -a --channel telegram -n 5
tail -n 5 workspace/logs/deliveries.jsonl
```

## Conversations and memory

Each Telegram conversation is its own context, with its own conversational
memory and its own serialization lane:

| where the message came from | context |
|---|---|
| a group or supergroup | `chat:telegram:<chat_id>` |
| a private chat | `chat:telegram:dm:<user_id>` |

Nothing recalls another conversation's history: what someone says in a DM is
not visible from a group.

## What v1 does not do

Stated so you do not go looking:

- **Media** — inbound photos, documents, and voice notes are ignored. Only
  text messages become events.
- **Editing and typing indicators** — replies are plain `sendMessage`.
- **Inline keyboards and callbacks** — not wired.
- **Webhooks** — long polling only. No public URL or TLS termination
  needed, which is the point.

## Troubleshooting

**Nothing arrives.** Check the adapter is in the binary
(`./bin/fclaw adapters -a`), then look for refusals in
`workspace/logs/narration.jsonl` — an unlisted chat, a bot-authored message,
and a missing token each say so by name.

**`could not build getUpdates`.** The token is not in the auth store under
the configured `token_key`. Re-run the `auth set-stdin` step.

**Rate limited.** The adapter honours Telegram's own `retry_after` and keeps
the message queued; it does not drop replies to back off.

## Test seam

`channels.telegram.api_base` exists for the hermetic transport test only.
It is refused at startup unless `FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK=1` is
set, and then accepts exactly `http://127.0.0.1:<port>`. It is not a proxy
or regional-endpoint override; production traffic always goes to
`api.telegram.org` over verified TLS.
