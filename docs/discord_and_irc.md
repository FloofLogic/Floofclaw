# Discord and IRC — channel conventions

**Discord is the human channel.** A configured Discord deployment serves
real product conversation, not an automated test surface.

**IRC is the assistant / probe channel.** Anything that programmatically
tests, probes, or exercises a deployed FloofClaw product runs on a dedicated
IRC test deployment. Automated regression checks, baseline flows, and one-off
"does this still work" verifications all belong on IRC.

This is a hard convention, not a preference. Automated probes never run on
Discord—not even a quick one-shot check by an assistant working on the
codebase.

## Why the split

Discord serves a live user. Every message there — from anyone — is part
of the product's memory and affects future turns. A test message from an
assistant working on the code becomes:

- a `user_message` event in the wrong context (`chat:discord:...`)
- a permanent line in `workspace/memory/memory.jsonl` that muddies the
  distilled conversation summary
- possibly a spurious `affair_open` or `work` call the human didn't ask
  for
- noise the human has to read past when they check their bot

IRC has none of that pressure when it is a dedicated test deployment. Configure
the bot and probe for a local or operator-owned IRC server, preferably with
`require_mention_in_channel: false` in its test channel. Probes connect as
different nicks; DMs stay isolated.

## How to probe

Two shapes both work.

### DM (default for automated tests)

Probe connects to the configured ircd, registers as a nick (for example
`probe1`), sends `PRIVMSG floofclaw :<your message>`, and waits for
`PRIVMSG probe1 :<reply>`. Fully isolated — nobody else sees the exchange,
and the bot's response uses that probe's own `chat:irc:dm:probe1` context,
so a probe never reads or writes the memory of a real conversation.

Preferred whenever the test is about whether the configured product responded
correctly to a message.

### Channel

Probe joins `#floofclaw` and sends a message there. The bot responds
in-channel. Any human in the channel (including whoever's watching)
sees it. Useful ONLY when the test is specifically about adapter
behavior: mention detection, `require_mention_in_channel` gating,
etc. Otherwise DM.

## The probe tool

`scripts/irc_probe.py` is a grandfathered Python utility with zero external
dependencies (stdlib `socket` and `select`).
One invocation per test flow:

```bash
scripts/irc_probe.py --host 127.0.0.1 --port 6667 \
  --nick probe1 --to floofclaw "Hi"
```

Records exact request, exact reply, and latency to
`workspace/tests/irc_probe.jsonl`. Exits with:

- 0 — probe completed cleanly (message sent, reply received)
- 1 — connection or registration failed
- 2 — timed out with no response

## Discord safety

Anyone writing automation, tests, or one-off scripts against a FloofClaw
deployment must not open a Discord socket, invoke a Discord webhook, or send a
bus envelope with `channel:"discord"`.

The one legitimate way an assistant interacts with the Discord side is
observationally — reading run logs, inspecting `workspace/logs/
deliveries.jsonl`, tailing narration. Never sending.

## When this convention could change

If Discord acquires a distinct test-only channel (for example,
`#floofclaw-tests`) and the bot is configured to also join and respond there
under a probe-specific gate, then automated probes could legitimately hit that
channel. This is a nontrivial config change — introduces bot config
that only exists for test purposes, requires a distinct guild/channel
ID pair per bot, and would benefit from a mirroring IRC-style
`require_mention_in_channel` policy. It's not currently justified.

For now: IRC only.

## Related

- [IRC setup](getting-started/irc.md) — configure an IRC deployment.
- [IRC probing](testing/irc_probing.md) — operate the probe safely.
- The original v0.18 F1–F8 evidence remains in private development history;
  it established this convention but is not a current release gate.
