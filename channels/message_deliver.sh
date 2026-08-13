#!/usr/bin/env bash
# message_deliver — external channel deliverer.
#
# Tails workspace/logs/deliveries.jsonl and delivers `message` results for
# channels bridged over ssh / external CLIs (iMessage, email). Compiled
# adapters (Discord, IRC, …) post their own channels; this covers the rest,
# so that ANY agent's `message` reaches the user on the channel they came in
# on — no per-agent send action needed.
#
# The recipient comes from the delivery's `ref` (set by the intake via
# `fclaw bus publish --ref`), exactly the field Discord's outbox reads for
# its channel_id. `ref` threads through work/operation hops, so a reply that
# comes back after a `work` task still knows where to go.
#
# This is the last-mile sender for the same integrations as the
# send_imessage / send_email actions, so it reads their deployment
# settings from config/floofclaw_config.json (actions.<id>). The runtime
# cannot inject them here — it never spawns this script — so it reads
# that file directly rather than depend on a sourced env file. Run it
# from the deployment directory. An explicit environment variable still
# wins, for ad-hoc runs.
#
# A channel is handled only if its config is present; others are ignored.
#   iMessage:  actions.send_imessage IMESSAGE_SSH_HOST, FLIMESSAGE_REMOTE
#              (imessage-send.sh on the Mac); IMESSAGE_SSH_BIN env (default ssh)
#   email:     actions.send_email FLOOFMAIL_CMD, FLOOFMAIL_PROFILE
#   general:   DELIVERIES_LOG (default workspace/logs/deliveries.jsonl)
#              DELIVER_CURSOR (default workspace/logs/.message_deliver_cursor)
#
#   message_deliver.sh --once     # process new deliveries once, then exit
#   message_deliver.sh --follow   # follow the log and deliver as they land
set -u
DELIV="${DELIVERIES_LOG:-workspace/logs/deliveries.jsonl}"
CURSOR="${DELIVER_CURSOR:-workspace/logs/.message_deliver_cursor}"
ssh_bin="${IMESSAGE_SSH_BIN:-ssh}"

command -v jq >/dev/null 2>&1 || { echo "message_deliver requires jq" >&2; exit 1; }

deliver_config() { # action_id, key
  [ -r config/floofclaw_config.json ] || return 0
  jq -r --arg a "$1" --arg k "$2" \
    '.actions[$a][$k] // empty' config/floofclaw_config.json 2>/dev/null
}

IMESSAGE_SSH_HOST="${IMESSAGE_SSH_HOST:-$(deliver_config send_imessage IMESSAGE_SSH_HOST)}"
FLIMESSAGE_REMOTE="${FLIMESSAGE_REMOTE:-$(deliver_config send_imessage FLIMESSAGE_REMOTE)}"
FLOOFMAIL_CMD="${FLOOFMAIL_CMD:-$(deliver_config send_email FLOOFMAIL_CMD)}"
FLOOFMAIL_PROFILE="${FLOOFMAIL_PROFILE:-$(deliver_config send_email FLOOFMAIL_PROFILE)}"

deliver_imessage() { # to, text
  local to="$1" text="$2"
  [ -n "${IMESSAGE_SSH_HOST:-}" ] && [ -n "${FLIMESSAGE_REMOTE:-}" ] || {
    echo "message_deliver: imessage not configured; skipping" >&2; return 1; }
  case "$to" in ''|*[!A-Za-z0-9+@._-]*)
    echo "message_deliver: unsafe/empty imessage handle; skipping" >&2; return 1;; esac
  printf '%s' "$text" | "$ssh_bin" -o BatchMode=yes "$IMESSAGE_SSH_HOST" "$FLIMESSAGE_REMOTE '$to'"
}

deliver_email() { # line
  local line="$1" text reply_to to subject bf rc
  [ -n "${FLOOFMAIL_CMD:-}" ] && [ -n "${FLOOFMAIL_PROFILE:-}" ] || {
    echo "message_deliver: email not configured; skipping" >&2; return 1; }
  text="$(jq -r '.delivery.text // ""' <<<"$line")"
  reply_to="$(jq -r '.delivery.ref.reply_to_id // empty' <<<"$line")"
  to="$(jq -r '.delivery.ref.to // empty' <<<"$line")"
  subject="$(jq -r '.delivery.ref.subject // empty' <<<"$line")"
  bf="$(mktemp "${TMPDIR:-/tmp}/fclaw-deliver.XXXXXX")" || return 1
  printf '%s' "$text" > "$bf"
  if [ -n "$reply_to" ]; then
    case "$reply_to" in fm_*) ;; *) rm -f "$bf"; echo "message_deliver: bad reply id; skipping" >&2; return 1;; esac
    "$FLOOFMAIL_CMD" --profile "$FLOOFMAIL_PROFILE" reply "$reply_to" --text-file "$bf" --apply --yes >/dev/null 2>&1
  elif [ -n "$to" ]; then
    "$FLOOFMAIL_CMD" --profile "$FLOOFMAIL_PROFILE" send --to "$to" --subject "${subject:-(no subject)}" --text-file "$bf" --apply --yes >/dev/null 2>&1
  else
    rm -f "$bf"; echo "message_deliver: email delivery lacks reply_to_id/to; skipping" >&2; return 1
  fi
  rc=$?; rm -f "$bf"; return $rc
}

handle_line() { # one deliveries.jsonl line
  local line="$1" channel to
  channel="$(jq -r '.delivery.channel // empty' <<<"$line" 2>/dev/null)" || return 0
  case "$channel" in
    imessage)
      to="$(jq -r '.delivery.ref.handle // empty' <<<"$line")"
      if [ -z "$to" ]; then
        echo "message_deliver: imessage delivery missing ref.handle (origin $(jq -r '.delivery.origin_event_id // "?"' <<<"$line")); skipping" >&2
        return 0
      fi
      if deliver_imessage "$to" "$(jq -r '.delivery.text // ""' <<<"$line")"; then
        echo "message_deliver: imessage -> $to" >&2
      else
        echo "message_deliver: imessage delivery FAILED -> $to" >&2
      fi
      ;;
    email)
      if deliver_email "$line"; then echo "message_deliver: email sent" >&2
      else echo "message_deliver: email delivery FAILED" >&2; fi
      ;;
    *) : ;;  # discord / irc / cli deliver themselves
  esac
}

process_new() {
  local n total i=0 line
  n="$(cat "$CURSOR" 2>/dev/null || echo 0)"
  case "$n" in ''|*[!0-9]*) n=0 ;; esac
  [ -f "$DELIV" ] || return 0
  total="$(wc -l < "$DELIV" 2>/dev/null | tr -d ' ')"
  case "$total" in ''|*[!0-9]*) return 0 ;; esac
  [ "$total" -gt "$n" ] || return 0
  while IFS= read -r line; do
    i=$((i + 1))
    [ "$i" -le "$n" ] && continue
    [ -n "$line" ] && handle_line "$line"
  done < "$DELIV"
  printf '%s' "$total" > "$CURSOR"
}

mkdir -p "$(dirname "$CURSOR")" 2>/dev/null || true
mode="${1:---follow}"
case "$mode" in
  --once) process_new ;;
  --follow)
    # Poll, don't `tail -F | while read`: piping tail block-buffers, so a
    # single appended line never reaches the loop. process_new is cheap and
    # cursor-based, so a plain poll avoids rereading completed batches during
    # ordinary operation. The README documents the remaining crash window.
    interval="${DELIVER_POLL:-2}"
    while :; do process_new; sleep "$interval"; done
    ;;
  *) echo "usage: message_deliver.sh [--once|--follow]" >&2; exit 1 ;;
esac
