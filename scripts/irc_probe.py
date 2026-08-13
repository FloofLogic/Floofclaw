#!/usr/bin/env python3
"""IRC probe for testing a FloofClaw bot — see docs/discord_and_irc.md.

Connects to a configured IRC server, registers as a chosen nick, sends one
message to the bot (DM by default, or in a channel with --channel),
waits for the reply, and records the interaction to a JSONL file
under workspace/tests/.

Zero external deps — Python 3 stdlib only.

Exit codes:
  0  probe completed cleanly (sent, at least one reply within timeout)
  1  connection or registration failed
  2  sent but no reply within timeout

Example:
  scripts/irc_probe.py --host 127.0.0.1 --nick probe1 --to floofclaw "Hi"
  scripts/irc_probe.py --nick probe2 --style channel "watch for kill tony tickets"
"""

import argparse
import json
import os
import select
import socket
import sys
import time
from pathlib import Path


DEFAULTS = {
    "host": "127.0.0.1",
    "port": 6667,
    "nick": "probe1",
    "to": "floofclaw",
    "channel": "#floofclaw",
    "timeout": 90,      # codex runs take up to ~4min; a chat reply is fast
    "record": "workspace/tests/irc_probe.jsonl",
}


def utc_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def send_line(sock, line):
    sock.sendall((line + "\r\n").encode("utf-8"))


def parse_irc_line(raw):
    """Return (prefix, command, params) or None on parse failure.
    params[-1] is the trailing param (message body) if present."""
    if not raw:
        return None
    s = raw
    prefix = ""
    if s.startswith(":"):
        sp = s.find(" ")
        if sp < 0:
            return None
        prefix = s[1:sp]
        s = s[sp + 1:]
    trailing = None
    ts = s.find(" :")
    if ts >= 0:
        trailing = s[ts + 2:]
        s = s[:ts]
    parts = s.split(" ")
    cmd = parts[0]
    params = [p for p in parts[1:] if p]
    if trailing is not None:
        params.append(trailing)
    return (prefix, cmd, params)


def nick_from_prefix(prefix):
    """user!ident@host -> user; server prefixes fall through as-is."""
    return prefix.split("!", 1)[0]


def probe(args):
    started = time.time()
    result = {
        "ts": utc_iso(),
        "nick": args.nick,
        "target": args.channel if args.style == "channel" else args.to,
        "style": args.style,
        "sent": None,
        "replies": [],
        "elapsed_s": None,
        "exit": None,
        "error": None,
    }

    # 1. Connect.
    try:
        sock = socket.create_connection((args.host, args.port), timeout=15)
    except OSError as e:
        result["error"] = "connect_failed: %s" % e
        result["exit"] = 1
        result["elapsed_s"] = round(time.time() - started, 3)
        return result

    sock.setblocking(False)
    inbuf = b""
    registered = False
    sent = False
    saw_target_reply = False
    joined_channel = False

    # 2. IRC registration.
    send_line(sock, "NICK %s" % args.nick)
    send_line(sock, "USER %s 0 * :%s probe" % (args.nick, args.nick))

    deadline = started + args.timeout

    def maybe_send():
        """Send the probe message once registered (and channel-joined if needed)."""
        nonlocal sent
        if sent or not registered:
            return
        if args.style == "channel":
            if not joined_channel:
                return
            send_line(sock, "PRIVMSG %s :%s" % (args.channel, args.text))
            result["sent"] = {"target": args.channel, "text": args.text,
                              "ts": utc_iso()}
        else:
            send_line(sock, "PRIVMSG %s :%s" % (args.to, args.text))
            result["sent"] = {"target": args.to, "text": args.text,
                              "ts": utc_iso()}
        sent = True

    while time.time() < deadline:
        # Stop waiting shortly after we've seen at least one reply from the
        # intended target — codex-triggered replies are the exception where
        # we might want to wait longer, but the caller controls that via
        # --timeout.
        if saw_target_reply and (time.time() - started) > 4.0 and sent:
            break
        r, _, _ = select.select([sock], [], [], 1.0)
        if not r:
            continue
        try:
            chunk = sock.recv(4096)
        except (BlockingIOError, InterruptedError):
            continue
        except OSError as e:
            result["error"] = "socket_recv: %s" % e
            break
        if not chunk:
            result["error"] = "server_closed_connection"
            break
        inbuf += chunk
        while b"\r\n" in inbuf:
            line_b, inbuf = inbuf.split(b"\r\n", 1)
            try:
                line = line_b.decode("utf-8", errors="replace")
            except Exception:
                continue
            parsed = parse_irc_line(line)
            if not parsed:
                continue
            prefix, cmd, params = parsed

            # Server keepalive.
            if cmd == "PING":
                token = params[-1] if params else ""
                send_line(sock, "PONG :%s" % token)
                continue

            # 001 = welcome; registration complete.
            if cmd == "001":
                registered = True
                if args.style == "channel":
                    send_line(sock, "JOIN %s" % args.channel)
                else:
                    maybe_send()
                continue

            # JOIN confirmation for our nick.
            if cmd == "JOIN" and args.style == "channel":
                who = nick_from_prefix(prefix)
                joined = params[0] if params else ""
                if who == args.nick and joined.lower() == args.channel.lower():
                    joined_channel = True
                    maybe_send()
                continue

            # Nick already in use — bump and retry.
            if cmd == "433":
                # 433 <me> <nick> :Nickname is already in use.
                new_nick = args.nick + str(int(time.time()) % 1000)
                send_line(sock, "NICK %s" % new_nick)
                result["nick"] = new_nick
                args.nick = new_nick
                continue

            # PRIVMSG — is it aimed at us? From our target?
            if cmd == "PRIVMSG" and len(params) >= 2:
                from_nick = nick_from_prefix(prefix)
                target = params[0]
                body = params[1]
                addressed_to_us = (target.lower() == args.nick.lower() or
                                   target.lower() == args.channel.lower())
                if addressed_to_us and from_nick.lower() == args.to.lower():
                    result["replies"].append({
                        "from": from_nick,
                        "to": target,
                        "text": body,
                        "ts": utc_iso(),
                    })
                    saw_target_reply = True

    try:
        send_line(sock, "QUIT :probe done")
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass

    result["elapsed_s"] = round(time.time() - started, 3)
    if not sent:
        if result["error"] is None:
            result["error"] = "registration_or_join_timeout"
        result["exit"] = 1
    elif not result["replies"]:
        if result["error"] is None:
            result["error"] = "no_reply_within_timeout"
        result["exit"] = 2
    else:
        result["exit"] = 0
    return result


def main():
    ap = argparse.ArgumentParser(description="IRC probe for a FloofClaw bot.")
    ap.add_argument("text", nargs="?", default=None,
                    help="Message text to send. Alt: pass --text.")
    ap.add_argument("--text", dest="text_arg", default=None,
                    help="Message text (overrides positional).")
    ap.add_argument("--nick", default=DEFAULTS["nick"])
    ap.add_argument("--to", default=DEFAULTS["to"],
                    help="Target nick for DM style / expected sender for channel.")
    ap.add_argument("--style", choices=("dm", "channel"), default="dm")
    ap.add_argument("--channel", default=DEFAULTS["channel"])
    ap.add_argument("--host", default=DEFAULTS["host"])
    ap.add_argument("--port", type=int, default=DEFAULTS["port"])
    ap.add_argument("--timeout", type=float, default=DEFAULTS["timeout"])
    ap.add_argument("--record", default=DEFAULTS["record"],
                    help="Append the JSON result line here. Empty = don't record.")
    ap.add_argument("--quiet", action="store_true",
                    help="Don't print result JSON to stdout.")
    args = ap.parse_args()

    if args.text_arg is not None:
        args.text = args.text_arg
    if not args.text:
        ap.error("message text required (positional or --text)")

    result = probe(args)

    if args.record:
        rec_path = Path(args.record)
        rec_path.parent.mkdir(parents=True, exist_ok=True)
        with rec_path.open("a") as fp:
            fp.write(json.dumps(result) + "\n")

    if not args.quiet:
        print(json.dumps(result, indent=2))

    sys.exit(result["exit"])


if __name__ == "__main__":
    main()
