#!/usr/bin/env bash
# Self-contained test for message_deliver. Stubs ssh + floofmail; synthetic
# deliveries.jsonl. No network, no Mac, no mail. NOT part of `make test`.
#   ./channels/message_deliver.test.sh
set -uo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/msgdeliver-test.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# stub ssh: record host + remote command + piped body
cat > "$TMP/ssh" <<'FAKE'
#!/usr/bin/env bash
host=""; remote=""
for a in "$@"; do case "$a" in -o|BatchMode=*) continue;; esac
  if [ -z "$host" ]; then host="$a"; else remote="$a"; fi; done
body="$(cat)"
{ echo "HOST=$host"; echo "REMOTE=$remote"; echo "BODY=$body"; } >> "$SSH_LOG"
FAKE
chmod +x "$TMP/ssh"

# stub floofmail: record args + body
cat > "$TMP/floofmail" <<'FAKE'
#!/usr/bin/env bash
args="$*"; bf=""
for ((i=1;i<=$#;i++)); do [ "${!i}" = "--text-file" ] && { j=$((i+1)); bf="${!j}"; }; done
{ echo "ARGS=$args"; [ -n "$bf" ] && echo "BODY=$(cat "$bf")"; } >> "$MAIL_LOG"
FAKE
chmod +x "$TMP/floofmail"

DELIV="$TMP/deliveries.jsonl"
cat > "$DELIV" <<'JSON'
{"run_id":"r1","delivery":{"delivery_id":"d1","origin_event_id":"bus_1","channel":"imessage","ref":{"handle":"person@example.test"},"text":"hi over imessage"}}
{"run_id":"r2","delivery":{"delivery_id":"d2","origin_event_id":"bus_2","channel":"discord","ref":{"channel_id":"999"},"text":"discord handled elsewhere"}}
{"run_id":"r3","delivery":{"delivery_id":"d3","origin_event_id":"bus_3","channel":"email","ref":{"reply_to_id":"fm_42"},"text":"email reply body"}}
{"run_id":"r4","delivery":{"delivery_id":"d4","origin_event_id":"bus_4","channel":"imessage","text":"no handle here"}}
JSON

export SSH_LOG="$TMP/ssh.log"; : > "$SSH_LOG"
export MAIL_LOG="$TMP/mail.log"; : > "$MAIL_LOG"
export DELIVERIES_LOG="$DELIV"
export DELIVER_CURSOR="$TMP/cursor"
export IMESSAGE_SSH_BIN="$TMP/ssh"
export IMESSAGE_SSH_HOST="relay@example.test"
export FLIMESSAGE_REMOTE="/opt/example/imessage-send.sh"
export FLOOFMAIL_CMD="$TMP/floofmail"
export FLOOFMAIL_PROFILE="test-profile"

fail=0
check() { if printf '%s' "$3" | grep -qF -- "$2"; then echo "ok: $1"; else echo "FAIL: $1 — wanted: $2"; echo "  got: $3"; fail=1; fi; }
absent(){ if printf '%s' "$3" | grep -qF -- "$2"; then echo "FAIL: $1 — did NOT want: $2"; fail=1; else echo "ok: $1"; fi; }

bash "$DIR/message_deliver.sh" --once 2>"$TMP/err.log"

check "imessage hits the Mac host"   'HOST=relay@example.test' "$(cat "$SSH_LOG")"
check "imessage targets the handle"  "imessage-send.sh 'person@example.test'" "$(cat "$SSH_LOG")"
check "imessage body piped"          'BODY=hi over imessage' "$(cat "$SSH_LOG")"
absent "discord is not sent by us"   'discord handled elsewhere' "$(cat "$SSH_LOG")$(cat "$MAIL_LOG")"
check "email reply via floofmail"    'reply fm_42' "$(cat "$MAIL_LOG")"
check "email applied"                '--apply --yes' "$(cat "$MAIL_LOG")"
check "email body piped"             'BODY=email reply body' "$(cat "$MAIL_LOG")"
check "missing-handle imessage skipped" 'missing ref.handle' "$(cat "$TMP/err.log")"
absent "missing-handle not sent"     'no handle here' "$(cat "$SSH_LOG")"

# idempotency: a second --once with no new lines must send nothing more
: > "$SSH_LOG"; : > "$MAIL_LOG"
bash "$DIR/message_deliver.sh" --once 2>/dev/null
absent "no re-send on second pass (imsg)" 'HOST=' "$(cat "$SSH_LOG")"
absent "no re-send on second pass (mail)" 'ARGS=' "$(cat "$MAIL_LOG")"

# a newly appended delivery is picked up
echo '{"run_id":"r5","delivery":{"delivery_id":"d5","origin_event_id":"bus_5","channel":"imessage","ref":{"handle":"+15551234567"},"text":"fresh one"}}' >> "$DELIV"
bash "$DIR/message_deliver.sh" --once 2>/dev/null
check "new delivery picked up" 'BODY=fresh one' "$(cat "$SSH_LOG")"

echo "---"
[ "$fail" -eq 0 ] && echo "ALL MESSAGE_DELIVER TESTS PASSED" || { echo "MESSAGE_DELIVER TESTS FAILED"; exit 1; }
