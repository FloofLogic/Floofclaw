#!/usr/bin/env bash
# The model sees only a gcal result's `text`. Before 2026-08-26 a search
# returned "Searched calendar events for <account>." whether it found ten
# events or none, and Truly searched in a loop until its rate limiter cut it
# off. Every kind must now say what happened, with ids the next call can use.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
# shellcheck source=/dev/null
. "$root/actions/common/gcal/render.sh"
fail() { printf 'gcal result text: %s\n' "$1" >&2; exit 1; }

ev1='{"id":"abc123","summary":"Call with Example Person","start":{"dateTime":"2026-08-27T14:00:00-05:00"},"end":{"dateTime":"2026-08-27T14:30:00-05:00"},"startLocal":"2026-08-27T14:00:00-05:00","endLocal":"2026-08-27T14:30:00-05:00","location":"Zoom"}'
ev2='{"id":"def456","summary":"Dentist","start":{"date":"2026-08-27"},"end":{"date":"2026-08-28"}}'

t="$(gcal_result_text search original "[$ev1,$ev2]" "Example Person" "2026-08-27T00:00:00-05:00" "2026-08-27T23:59:59-05:00")"
[[ "$t" == *'Found 2 calendar events for "Example Person" between 2026-08-27T00:00:00-05:00 and 2026-08-27T23:59:59-05:00 on original: '* ]] || fail "search header: $t"
[[ "$t" == *'Call with Example Person — 2026-08-27T14:00:00-05:00 to 2026-08-27T14:30:00-05:00 at Zoom [id abc123]'* ]] || fail "search event 1: $t"
[[ "$t" == *'Dentist — 2026-08-27 to 2026-08-28 [id def456]'* ]] || fail "all-day event uses its date: $t"

t="$(gcal_result_text search original '[]' "Example Person" "2026-08-27T00:00:00-05:00" "2026-08-27T23:59:59-05:00")"
[[ "$t" == 'No calendar events found for "Example Person" between 2026-08-27T00:00:00-05:00 and 2026-08-27T23:59:59-05:00 on original.' ]] || fail "empty search must say so: $t"

t="$(gcal_result_text list original '{"items":['"$ev2"']}' "" "" "")"
[[ "$t" == 'Found 1 calendar event on original: Dentist — 2026-08-27 to 2026-08-28 [id def456].' ]] || fail "list accepts an items object: $t"

t="$(gcal_result_text create original "$ev1" "" "" "")"
[[ "$t" == 'Created event Call with Example Person — 2026-08-27T14:00:00-05:00 to 2026-08-27T14:30:00-05:00 at Zoom [id abc123] on original.' ]] || fail "create names the event and id: $t"

t="$(gcal_result_text delete original 'null' "" "" "" "abc123")"
[[ "$t" == 'Deleted event abc123 on original.' ]] || fail "delete: $t"

t="$(gcal_result_text calendars original '[{"id":"primary","summary":"Truly"},{"id":"x@group.calendar.google.com","summary":"Ops"}]' "" "" "")"
[[ "$t" == 'Listed 2 calendars on original: Truly [primary]; Ops [x@group.calendar.google.com].' ]] || fail "calendars: $t"

t="$(gcal_result_text freebusy original '{"calendars":{"primary":{"busy":[{"start":"2026-08-27T14:00:00Z","end":"2026-08-27T15:00:00Z"}]}}}' "" "2026-08-27T00:00:00Z" "2026-08-27T23:59:59Z")"
[[ "$t" == 'Busy on original between 2026-08-27T00:00:00Z and 2026-08-27T23:59:59Z: 2026-08-27T14:00:00Z to 2026-08-27T15:00:00Z.' ]] || fail "freebusy: $t"
t="$(gcal_result_text freebusy original '{"calendars":{"primary":{"busy":[]}}}' "" "a" "b")"
[[ "$t" == 'No busy intervals on original between a and b.' ]] || fail "free: $t"

# Bounded: 200 events render as 20 plus a count, under the 8 KiB result cap.
many="$(jq -cn '[range(200) | {id: ("id" + tostring), summary: ("Event " + tostring), start: {date: "2026-09-01"}, end: {date: "2026-09-02"}}]')"
t="$(gcal_result_text list original "$many" "" "" "")"
[[ "$t" == *'(+180 more).' ]] || fail "listing is bounded with a remainder: ${t: -40}"
[ "${#t}" -le 3500 ] || fail "listing exceeds the bound: ${#t}"
printf 'gcal result text: PASS\n'
