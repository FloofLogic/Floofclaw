#!/usr/bin/env bash
# Result text for the gcal action. The model sees only a result's `text`
# (never its `data`), so the text must say what happened: which events were
# found, or that none were, or what was created — with ids the next call can
# use. Before this, a search returned "Searched calendar events for <account>."
# whether it found ten events or none, and the model searched in a loop.
#
# gcal_result_text KIND ACCOUNT DATA_JSON QUERY TIME_MIN TIME_MAX EVENT_ID
# prints one bounded paragraph (about 3.5 KiB at most).

gcal_result_text() {
  local kind="$1" account="$2" data="${3:-null}" query="${4:-}" tmin="${5:-}" tmax="${6:-}" event_id="${7:-}"
  jq -rn --arg kind "$kind" --arg account "$account" --argjson data "$data" \
         --arg query "$query" --arg tmin "$tmin" --arg tmax "$tmax" --arg event_id "$event_id" '
    def when: (.startLocal // .start.dateTime // .start.date // "?") + " to " + (.endLocal // .end.dateTime // .end.date // "?");
    def title: (.summary // "(no title)");
    def one: (title) + " — " + when + (if .location then " at " + .location else "" end) + " [id " + (.id // "?") + "]";
    def range: (if $tmin != "" or $tmax != "" then " between " + ($tmin // "") + " and " + ($tmax // "") else "" end);
    def events: (if ($data|type) == "array" then $data elif ($data|type) == "object" then ($data.items // $data.events // []) else [] end);
    def listing(max): (events) as $e
      | if ($e|length) == 0 then "No calendar events found" + (if $query != "" then " for \"" + $query + "\"" else "" end) + range + " on " + $account + "."
        else "Found " + ($e|length|tostring) + " calendar event" + (if ($e|length) == 1 then "" else "s" end)
             + (if $query != "" then " for \"" + $query + "\"" else "" end) + range + " on " + $account + ": "
             + ([$e[:max][] | one] | join("; "))
             + (if ($e|length) > max then "; (+" + (($e|length) - max|tostring) + " more)" else "" end) + "." end;
    if $kind == "search" or $kind == "list" then listing(20)
    elif $kind == "calendars" then
      (events) as $c | if ($c|length) == 0 then "No calendars on " + $account + "."
        else "Listed " + ($c|length|tostring) + " calendars on " + $account + ": " + ([$c[:30][] | (.summary // .id // "?") + " [" + (.id // "?") + "]"] | join("; ")) + "." end
    elif $kind == "create" or $kind == "update" or $kind == "get" then
      (if ($data|type) == "object" then $data else {} end) as $ev
      | (if $kind == "create" then "Created" elif $kind == "update" then "Updated" else "Retrieved" end)
        + " event " + ($ev | one) + " on " + $account + "."
    elif $kind == "delete" then "Deleted event " + $event_id + " on " + $account + "."
    elif $kind == "freebusy" then
      ([ (if ($data|type) == "object" then ($data.calendars // {} | to_entries[] | .value.busy // [] | .[]) else empty end) ]) as $busy
      | if ($busy|length) == 0 then "No busy intervals on " + $account + range + "."
        else "Busy on " + $account + range + ": " + ([$busy[:40][] | (.start // "?") + " to " + (.end // "?")] | join("; ")) + "." end
    else "Completed " + $kind + " on " + $account + "." end
  ' | head -c 3500
}
