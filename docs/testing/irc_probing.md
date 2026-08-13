# IRC probing — `scripts/irc_probe.py`

Operational how-to for using the IRC probe. The permanent
convention (Discord for humans, IRC for probes) lives in
[../discord_and_irc.md](../discord_and_irc.md).

## Quick start

The bot must be running (`./bin/fclaw gateway start -h --floop floofclaw`) and
connected to a dedicated local or operator-owned ircd. Verify the gateway with
`./bin/fclaw gateway status -h`; use the ircd's own logs to verify registration.

```bash
scripts/irc_probe.py --host 127.0.0.1 --to floofclaw "Hi"
```

The default nick is `probe1`, default target is `floofclaw`. The probe
sends one PRIVMSG DM to `floofclaw`, waits for a reply, and prints the
recorded interaction as JSON.

## Common shapes

DM (default — isolated, other IRC users don't see it):

```bash
scripts/irc_probe.py --host 127.0.0.1 --nick probe1 \
  "Watch for ticket availability"
```

Channel (visible in `#floofclaw` — useful for adapter parsing
tests only; noisy otherwise):

```bash
scripts/irc_probe.py --style channel --channel '#floofclaw' \
  --nick probe1 "hey floofclaw, what's up?"
```

Long worker-triggered flows (bigger timeout so the reply arrives):

```bash
scripts/irc_probe.py --timeout 300 \
  "Find comedy shows at the Paramount Theatre this weekend"
```

Note: the probe closes the socket after receiving one reply from
the target. Codex-follow-up messages that arrive minutes later
won't be received by a disconnected probe — verify those via
`workspace/logs/deliveries.jsonl` and `./bin/fclaw view -a <run_id>`
instead.

## Args

- `text` (positional) or `--text TEXT` — the message to send.
- `--nick NICK` — probe's IRC nick. Default `probe1`. If in use,
  the probe automatically bumps to `probe1<N>` on 433 nick-in-use.
- `--to NICK` — target nick for DM style / expected sender for
  channel-style filtering. Default `floofclaw`.
- `--style dm | channel` — default `dm`.
- `--channel #x` — channel to join in channel style. Default
  `#floofclaw`.
- `--host HOST` `--port PORT` — server. Defaults to
  `127.0.0.1:6667`; pass the configured test ircd explicitly for any other
  deployment.
- `--timeout SECONDS` — total probe deadline. Default 90. Longer
  for worker-triggered flows.
- `--record PATH` — append the JSON result to this JSONL. Default
  `workspace/tests/irc_probe.jsonl`. Empty string disables
  recording.
- `--quiet` — don't print the JSON to stdout (still records to
  `--record`).

## Exit codes

- `0` — probe completed cleanly: message sent, at least one reply
  received within timeout.
- `1` — connection or registration failed; message never sent.
- `2` — message sent but no reply within timeout. Check the run's event
  log before classifying it: the run may have failed, a longer managed
  operation may still be active, or the selected floop may intentionally
  produce no delivery. The current `floofclaw` chat manager, specifically,
  requires a reply to every direct `user_message`.

## When to use it

- Verifying behavior after any prompt / floop / engine change.
- Reproducing a specific conversation shape while diagnosing.
- Building explicitly versioned baseline evidence (the historical v0.18
  baseline lives in the development repo).

Not a load test. Not a replacement for real user conversation on
Discord — that's a real product surface. This tool exists so
we can verify correctness without polluting the human's
conversation.
