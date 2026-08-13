# IRC in two minutes

**Preconditions:** build with OpenSSL for public TLS IRC, choose a unique nick,
and choose either a public network/channel or a local IRC daemon. IRC needs no
FloofClaw token or developer portal.

IRC is the simplest real channel supported by the gateway. Direct messages
always reach the floop; channel messages follow
`require_mention_in_channel`.

## Public example: Libera Chat

Replace the disabled `channels.irc` object in
`config/floofclaw_config.json` with:

```json
"irc": {
  "enabled": true,
  "server": "irc.libera.chat",
  "port": 6697,
  "tls": true,
  "tls_verify": true,
  "nick": "YOUR_UNIQUE_BOT_NICK",
  "username": "floofclaw",
  "realname": "FloofClaw",
  "require_mention_in_channel": true,
  "join": ["#YOUR_CHANNEL"]
}
```

Start the gateway attached:

```bash
./bin/fclaw gateway run -h
```

From an ordinary IRC client, join the same channel and send
`YOUR_UNIQUE_BOT_NICK: hello`. A direct message does not need the mention.
Stop the gateway with Ctrl-C.

The adapter retries a nick-in-use error with a few underscore suffixes. Pick a
genuinely unique configured nick rather than relying on that recovery.

## Local ircd example

For an IRC daemon listening on the same machine without TLS:

```json
"irc": {
  "enabled": true,
  "server": "127.0.0.1",
  "port": 6667,
  "tls": false,
  "nick": "floofclaw",
  "username": "floofclaw",
  "realname": "FloofClaw",
  "require_mention_in_channel": false,
  "join": ["#floofclaw"]
}
```

Use `tls_verify: false` only for a TLS server on localhost. Remote TLS servers
must keep verification enabled; startup rejects a remote opt-out.

## Optional fields

- `join` accepts up to eight channels and may be omitted for DM-only use.
- `server` is required. `port` may be omitted; it defaults to 6697 when TLS is
  enabled and 6667 for plaintext.
- `require_mention_in_channel` defaults true and never affects DMs.
- `ops_channel` joins one additional channel and mirrors runtime narration
  there. Omit it for a normal chat-only setup.
- Missing `nick`, `username`, or `realname` values default to `fclaw`, `fclaw`,
  and `FloofClaw` respectively, but explicit values are easier to recognize.

## Current authentication limit

FloofClaw does not implement IRC SASL or NickServ identification yet. It works
on networks and channels that permit an unregistered client. If a network or
channel requires an authenticated account, use a local ircd, choose another
channel, or wait for SASL support; do not put a server password in the config.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Startup says `server/port is missing` | Set `channels.irc.server`; `port` may be omitted to use the TLS-sensitive default, or set it to a positive override. |
| TLS support is missing | Install OpenSSL 3 and run `make clean && make -j4`, or use plaintext only with a trusted local ircd. |
| Remote `tls_verify:false` is rejected | Set it true; insecure TLS is intentionally localhost-only. |
| Bot joins but ignores channel text | Mention its active nick, or deliberately set `require_mention_in_channel` false. |
| Bot never joins a restricted channel | The channel may require NickServ/SASL, which is not implemented. |
| No proactive narration appears | Set `ops_channel`; normal replies return to the inbound DM or channel automatically. |

For automated behavior checks, follow
[IRC probing](../testing/irc_probing.md). Probes belong on a dedicated test
IRC deployment, never Discord or a human conversation channel.
