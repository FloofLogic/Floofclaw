# Discord setup

**Preconditions:** build with OpenSSL support, have permission to add a bot to
a Discord server, and choose a server text channel. This guide never places
the bot token or real Discord IDs in a committed file.

## 1. Create the bot

1. Open the Discord Developer Portal and choose **New Application**.
2. Open **Bot**, choose **Reset Token**, and copy the token into a temporary
   password-safe location. Treat it like a password.
3. Still on **Bot**, enable **Message Content Intent** under Privileged Gateway
   Intents. Without it, the bot connects but ordinary messages arrive with
   empty content and are ignored.

FloofClaw's gateway requests the message-content intent, but Discord also
requires that portal switch. No Presence or Server Members privileged intent
is needed for normal chat.

## 2. Invite it to the server

Open **OAuth2 → URL Generator**:

- select the `bot` scope;
- grant **View Channels**, **Send Messages**, and **Read Message History**;
- open the generated URL, choose the intended server, and authorize it.

If a channel has explicit role overrides, confirm the bot can view and send in
that channel after the invite.

## 3. Copy the IDs

In the Discord desktop or web client, open **User Settings → Advanced** and
enable **Developer Mode**. Then:

- right-click the server and choose **Copy Server ID** for `guild_id`;
- right-click the text channel and choose **Copy Channel ID** for
  `allowed_channel_ids`;
- optionally copy the proactive destination for `home_channel_id`.

IDs are identifiers, not secrets, but use placeholders in shared examples.

## 4. Store the token

Store it through stdin so it does not appear in shell history or process
arguments:

```bash
./bin/fclaw auth set-stdin -h discord_token
```

Paste the token, press Return, then send EOF (Ctrl-D). The config contains only
the auth-store name `discord_token`, never the token value.

## 5. Configure the adapter

Replace the disabled `channels.discord` object in
`config/floofclaw_config.json` with:

```json
"discord": {
  "enabled": true,
  "debug": true,
  "tls_verify": true,
  "allow_bots": false,
  "allow_dms": true,
  "require_mention_in_guild": true,
  "guild_id": "YOUR_GUILD_ID",
  "allowed_channel_ids": ["YOUR_CHANNEL_ID"],
  "token_key": "discord_token",
  "home_channel_id": "YOUR_HOME_CHANNEL_ID"
}
```

Required fields are `enabled`, `guild_id`, at least one
`allowed_channel_ids` entry, and `token_key`. TLS verification must remain
enabled for Discord.

The remaining fields are optional policy:

- `debug` prints protocol diagnostics; keep it on through first connection,
  then turn it off unless troubleshooting.
- `allow_bots` defaults false to avoid bot loops.
- `allow_dms` controls direct messages and defaults false.
- `require_mention_in_guild` defaults true; DMs never require a mention.
- `home_channel_id` is the optional proactive-message destination. Without
  it, proactive reviews use the required `allowed_channel_ids[0]` entry.

Keep the surrounding JSON valid—this object normally follows the `irc`
object, so the preceding property needs a comma.

## 6. Verify startup

Start attached for the first check:

```bash
./bin/fclaw gateway run -h
```

With `debug: true`, a successful Discord handshake writes a `discord: READY`
diagnostic. In daemon mode, verify it with:

```bash
grep "discord: READY" workspace/logs/gateway.log
```

Session transitions and failures are also written to
`workspace/logs/narration.jsonl`. A fresh session says `via IDENTIFY`; a
recovered session says `via RESUME`. Every reconnect line carries its cause,
including the WebSocket close code/reason, server-requested RECONNECT,
invalid-session resumability, a missed heartbeat ACK, or a transport failure.
REST 429 responses honor Discord's `Retry-After` and retain the delivery at
the queue head, so rate limiting does not drop or reorder replies; excessive
delays clamp to 24 hours. If the bounded outbound ring cannot fit every chunk
of a reply, the delivery-log cursor stays put and narration names the deferred
channel instead of sending a prefix or skipping later replies. Once a POST is
fully written, a lost, malformed, or timed-out response has an ambiguous
outcome: FloofClaw retries it once, then drops it with narration rather than
risk an unbounded stream of duplicate messages.

Stop the attached gateway with Ctrl-C. A real test message is deliberately a
human/operator verification: send it in the allowed channel and mention the
bot when `require_mention_in_guild` is true. Automated checks must not send to
Discord.

## Conversations and memory

Each Discord conversation is its own context, so each has its own
conversational memory and its own serialization lane:

| where the message came from | context |
|---|---|
| a guild channel | `chat:discord:<channel_id>` |
| a thread | `chat:discord:<thread_channel_id>` |
| a direct message | `chat:discord:dm:<user_id>` |

A thread gets its own context automatically, because Discord gives it its
own channel id. Nothing recalls another conversation's history: what someone
says in a DM is not visible from a public channel.

**Upgrading a deployment that ran before 0.28.0:** every conversation shared
one context, `chat:discord:discord-main`, and the records already in
`workspace/memory/memory.jsonl` carry that tag. After the upgrade nothing
reads them, so each conversation starts with empty memory. Decide before
restarting: accept the reset (nothing to do — the old records stay on disk,
inert), or retag the lines you want to keep to the specific
`chat:discord:<channel_id>` they belong to. There is no automatic migration,
deliberately: only the operator knows which of those mixed records belonged
to which conversation.

## Attachments and model media

Discord image-only and text-plus-attachment messages use the same
`user_message` path. FloofClaw accepts up to 10 attachments, 25 MiB per item,
and 50 MiB total. Discord authenticates the event and restricts source URLs to
its attachment CDN paths; the downloader follows no redirects and applies one
shared 30-second deadline across the message. Downloads occur only inside the
bounded LLM child, never in the gateway reactor callback. Signed Discord CDN
URLs are kept in private content-addressed manifests and are not written to
bus or gateway logs. Downloaded files and raw provider requests are
owner-private artifacts.

Media capability is provider-dependent. Gemini profiles can send images,
PDFs, audio, and video through the generic media contract. A Gemini inline
serialized request must remain below 20 MB; a larger request fails before
network I/O and names the future Files API upload boundary. Other configured
providers accept only the MIME types their wire protocol supports; an
unsupported pairing fails visibly instead of continuing with text alone.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Startup says `missing token` | Run `./bin/fclaw auth set-stdin -h discord_token` and keep `token_key` set to `discord_token`. |
| Startup says `missing guild_id` | Copy the server ID in Developer Mode and set `channels.discord.guild_id`. |
| Startup says `allowed_channel_ids is empty` | Add at least one copied text-channel ID. |
| Bot connects but messages have empty content | Enable **Message Content Intent** on the Developer Portal's Bot page, then restart. |
| Guild messages are ignored | Confirm the channel ID is allowed and mention the bot, or deliberately set `require_mention_in_guild` false. |
| DMs are ignored | Set `allow_dms` true. |
| An attachment turn fails before a model response | Check the run's agent stderr and provider metadata. Oversized input, an expired/invalid Discord CDN download, or a provider/MIME pair without a native representation is rejected rather than silently omitting the attachment. |
| No proactive review appears | Set `home_channel_id`, or ensure `allowed_channel_ids[0]` is a writable channel. |
| TLS/OpenSSL startup error | Install OpenSSL 3, then run `make clean && make -j4`; do not disable verification for Discord. |
| A reply never arrives and `workspace/logs/narration.jsonl` shows `discord: REST post to channel <id> failed status=401` (or `403 code=50001 message=Missing Access`) `; reply dropped` | Discord refused the post permanently, so the reply was dropped rather than retried. 401: reset the bot token and store the replacement. 403: give the bot View Channel and Send Messages in that channel. A `status=5xx … after one retry` line means Discord failed twice in a row; the reply is lost, the next one is attempted normally. |
| Narration says a REST post had `ambiguous completion` | Discord may have accepted the POST but its response could not be proven. FloofClaw retries once; a second ambiguous result is dropped to bound possible duplicates. Check the channel before resending manually. |

The permanent safety convention is in
[Discord and IRC — channel conventions](../discord_and_irc.md): Discord is a
human conversation surface, never an automated probe target.
