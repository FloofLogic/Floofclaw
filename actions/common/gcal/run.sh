#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
. "$DIR/lib.sh"
gcal_require_tools

input="$(cat)"
bridge_active=0
gcal_exit_cleanup() {
  if [ "$bridge_active" -eq 1 ]; then gcal_cleanup_bridge; fi
  gcal_unlock_credentials
}
trap gcal_exit_cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

emit_failure() {
  local code="$1" message="$2" account="${3-}" retryable=false reauth="" recovery_url=""
  case "$code" in RATE_LIMITED|RETRYABLE) retryable=true ;; esac
  if [ "$code" = REAUTHORIZATION_REQUIRED ] && [ -n "$account" ]; then
    reauth="$(gcal_reauth_command "$account")"
  fi
  if [ "$code" = API_DISABLED ]; then
    recovery_url="$(gcal_api_enable_url "$GCAL_GOG_STDERR" "$account" || true)"
  fi
  jq -cn --arg code "$code" --arg message "$message" --arg account "$account" \
    --arg reauth "$reauth" --arg recovery_url "$recovery_url" --argjson retryable "$retryable" \
    '{events:[],result:{},error:({code:$code,message:$message,retryable:$retryable}
      + (if $account!="" then {account:$account} else {} end)
      + (if $reauth!="" then {reauth_command:$reauth} else {} end)
      + (if $recovery_url!="" then {recovery_url:$recovery_url} else {} end))}'
  exit 1
}

finish() {
  local handle="$1" kind="$2" account="$3" text="$4" data="${5:-null}"
  jq -cn --arg handle "$handle" --arg kind "$kind" --arg account "$account" \
    --arg text "$text" --argjson data "$data" \
    '{events:[],result:({status:"finished",handle:$handle,kind:$kind,text:$text,data:$data}
      + (if $account!="" then {account:$account} else {} end)),error:null}'
}

jq -e 'type=="object" and (.args|type=="object")' >/dev/null 2>&1 <<<"$input" ||
  emit_failure BAD_ARGS "Expected a FloofClaw action input envelope."
args="$(jq -c '.args' <<<"$input")"
request_id="$(jq -r '.request_id // "request"' <<<"$input" | tr -cd '[:alnum:]_-')"
[ -n "$request_id" ] || request_id=request
handle="gcal_${request_id:0:80}"
op="$(jq -r '.op // ""' <<<"$args")"
[ "$op" = start ] || emit_failure BAD_ARGS "op must be start."
kind="$(jq -r '.kind // ""' <<<"$args")"
case "$kind" in diagnose|accounts|calendars|list|get|search|create|update|delete|freebusy) ;; *)
  emit_failure BAD_ARGS "kind must be diagnose, accounts, calendars, list, get, search, create, update, delete, or freebusy." ;;
esac
gcal_lock_credentials || emit_failure RETRYABLE "Calendar credential state is busy; retry this operation."

allowed='["op","kind","account"]'
case "$kind" in
  diagnose|accounts|calendars) ;;
  list) allowed='["op","kind","account","calendar_id","time_min","time_max","max_results"]' ;;
  get) allowed='["op","kind","account","calendar_id","event_id"]' ;;
  search) allowed='["op","kind","account","calendar_id","query","time_min","time_max","max_results"]' ;;
  create|update) allowed='["op","kind","account","calendar_id","event_id","event"]' ;;
  delete) allowed='["op","kind","account","calendar_id","event_id","confirm"]' ;;
  freebusy) allowed='["op","kind","account","calendar_id","time_min","time_max"]' ;;
esac
jq -e --argjson allowed "$allowed" '([keys[]] - $allowed)|length==0' >/dev/null <<<"$args" ||
  emit_failure BAD_ARGS "The selected kind contains unknown or conflicting fields."
jq -e '
  ((has("account")|not) or (.account|type=="string")) and
  ((has("calendar_id")|not) or (.calendar_id|type=="string")) and
  ((has("event_id")|not) or (.event_id|type=="string")) and
  ((has("time_min")|not) or (.time_min|type=="string")) and
  ((has("time_max")|not) or (.time_max|type=="string")) and
  ((has("query")|not) or (.query|type=="string")) and
  ((has("max_results")|not) or (.max_results|type=="number" and floor==.)) and
  ((has("confirm")|not) or (.confirm|type=="boolean"))' >/dev/null <<<"$args" ||
  emit_failure BAD_ARGS "Calendar arguments must use the documented JSON types."

requested_account="$(jq -r '.account // ""' <<<"$args")"
if [ -n "$requested_account" ] && ! gcal_alias_valid "$requested_account"; then
  emit_failure BAD_ARGS "account must be a lowercase alias of 1-32 characters using a-z, 0-9, underscore, or hyphen."
fi

accounts_result() {
  local aliases alias email client default="" rows='[]' refresh=false access=false
  if gcal_secret_get default_account; then default="$SECRET_VALUE"; fi
  aliases="$(gcal_list_aliases account email)" || emit_failure GOG_PROTOCOL_ERROR "Credential inventory failed."
  while IFS= read -r alias; do
    [ -n "$alias" ] || continue
    gcal_account_field "$alias" email || continue; email="$SECRET_VALUE"
    gcal_account_field "$alias" client || continue; client="$SECRET_VALUE"
    refresh=false; access=false
    if gcal_account_field "$alias" refresh_token; then refresh=true; fi
    if gcal_account_field "$alias" access_token; then access=true; fi
    rows="$(jq -cn --argjson rows "$rows" --arg alias "$alias" --arg email "$email" \
      --arg client "$client" --arg default "$default" --argjson refresh "$refresh" \
      --argjson access "$access" \
      '$rows + [{alias:$alias,email:$email,client:$client,configured:$refresh,default:($alias==$default),health:(if $refresh then "configured" else "authorization_required" end),access_token_present:$access}]')"
  done <<<"$aliases"
  finish "$handle" accounts "" "Configured Google Calendar accounts." "$rows"
}

if [ "$kind" = accounts ]; then accounts_result; exit 0; fi

set +e
account="$(gcal_select_account "$requested_account")"
select_rc=$?
set -e
case "$select_rc" in
  0) ;;
  2) emit_failure BAD_ARGS "The account alias is invalid." ;;
  3) emit_failure ACCOUNT_NOT_FOUND "The requested Google Calendar account is not configured." "$requested_account" ;;
  5) emit_failure ACCOUNT_REQUIRED "More than one account is configured; specify account or configure a default." ;;
  *) emit_failure GOG_PROTOCOL_ERROR "Credential inventory failed." ;;
esac

gcal_account_field "$account" email || emit_failure ACCOUNT_NOT_FOUND "The selected account has no email record." "$account"
email="$SECRET_VALUE"
gcal_account_field "$account" client || emit_failure CLIENT_NOT_FOUND "The selected account has no OAuth client record." "$account"
client="$SECRET_VALUE"

version="$(gcal_version || true)"
found=false compatible=false
if [ -n "$version" ]; then found=true; if gcal_version_compatible "$version"; then compatible=true; fi; fi

if [ "$kind" = diagnose ]; then
  client_present=false refresh_present=false access_present=false
  if gcal_client_field "$client" client_id && gcal_client_field "$client" client_secret; then client_present=true; fi
  if gcal_account_field "$account" refresh_token; then refresh_present=true; fi
  if gcal_account_field "$account" access_token; then access_present=true; fi
  code=""; message=""; stage=preflight; reachable=false; oauth_state=unknown; scopes_ok=false
  if [ "$found" != true ]; then code=GOG_MISSING
  elif [ "$compatible" != true ]; then code=GOG_INCOMPATIBLE
  elif [ "$client_present" != true ]; then code=CLIENT_NOT_FOUND
  elif [ "$refresh_present" != true ]; then code=AUTH_MISSING
  else
    bridge_active=1
    if ! gcal_prepare_account "$account"; then
      code="${GCAL_PREPARE_CODE:-GOG_PROTOCOL_ERROR}"
      stage="$GCAL_PREPARE_STAGE"
    else
      stage=calendar_api
      set +e
      # The original single-account setup grants calendar.events, which is
      # enough for event work but not CalendarList.list. Probe the exact API
      # surface the action needs so a healthy original grant is not rejected.
      gcal_run_capture events primary --max 1 --results-only
      gog_rc=$?
      set -e
      if [ "$gog_rc" -ne 0 ]; then
        # Preserve the provider failure. A failed request may leave nothing
        # exportable; writeback must not mask invalid_grant/scope/network.
        gcal_sync_account "$account" "$GCAL_CLIENT" "$GCAL_EMAIL" || true
        code="$(gcal_error_code "$GCAL_GOG_STDERR" "$gog_rc")"
      elif ! gcal_sync_account "$account" "$GCAL_CLIENT" "$GCAL_EMAIL"; then
        code=GOG_PROTOCOL_ERROR
        stage=token_writeback
      else
        stage=healthy
        reachable=true; oauth_state=healthy; scopes_ok=true
      fi
    fi
  fi
  if [ -n "$code" ]; then message="$(gcal_safe_message "$code")"; [ "$code" = REAUTHORIZATION_REQUIRED ] && oauth_state=reauthorization_required; fi
  reauth=""; recovery_url=""; retryable=false
  [ "$code" = REAUTHORIZATION_REQUIRED ] && reauth="$(gcal_reauth_command "$account")"
  [ "$code" = API_DISABLED ] && recovery_url="$(gcal_api_enable_url "$GCAL_GOG_STDERR" "$account" || true)"
  case "$code" in RATE_LIMITED|RETRYABLE) retryable=true ;; esac
  data="$(jq -cn --arg account "$account" --arg client "$client" --arg version "$version" \
    --argjson found "$found" --argjson compatible "$compatible" --argjson cp "$client_present" \
    --argjson rp "$refresh_present" --argjson ap "$access_present" --arg oauth "$oauth_state" \
    --argjson scopes "$scopes_ok" --argjson reachable "$reachable" --arg code "$code" \
    --arg message "$message" --arg reauth "$reauth" --arg recovery_url "$recovery_url" --argjson retryable "$retryable" --arg stage "$stage" \
    '{ok:($code==""),stage:$stage,account:$account,client:$client,gog:{found:$found,version:$version,compatible:$compatible},credentials:{client_present:$cp,refresh_token_present:$rp,access_token_present:$ap},oauth:{state:$oauth,scopes_ok:$scopes},calendar_api:{reachable:$reachable}}
     + (if $code!="" then {error:({code:$code,message:$message,retryable:$retryable}
       + (if $reauth!="" then {reauth_command:$reauth} else {} end)
       + (if $recovery_url!="" then {recovery_url:$recovery_url} else {} end))} else {} end)')"
  if [ -z "$code" ]; then
    diagnostic_text="Google Calendar is connected for $email."
  elif [ "$code" = REAUTHORIZATION_REQUIRED ]; then
    printf -v diagnostic_text \
      'Google Calendar authorization is required for %s. Run this exact command on the host:\n%s\nComplete the browser consent, then reply authorized after the terminal reports success; I will retry your saved calendar request.' \
      "$email" "$reauth"
  elif [ "$code" = API_DISABLED ] && [ -n "$recovery_url" ]; then
    printf -v diagnostic_text \
      'The Google Calendar API is disabled for %s. Enable it here:\n%s\nAfter Google reports Enabled, reply enabled; I will retry your saved calendar request.' \
      "$email" "$recovery_url"
  else
    diagnostic_text="Google Calendar diagnostic for $email failed with $code: $message"
  fi
  finish "$handle" diagnose "$account" "$diagnostic_text" "$data"
  exit 0
fi

[ "$found" = true ] || emit_failure GOG_MISSING "$(gcal_safe_message GOG_MISSING)" "$account"
[ "$compatible" = true ] || emit_failure GOG_INCOMPATIBLE "gog $GCAL_GOG_MIN_VERSION or newer is required; found ${version:-unknown}." "$account"
gcal_client_field "$client" client_id >/dev/null || emit_failure CLIENT_NOT_FOUND "The OAuth client is missing." "$account"
gcal_account_field "$account" refresh_token >/dev/null || emit_failure AUTH_MISSING "The account has no stored OAuth grant." "$account"

calendar_id="$(jq -r '.calendar_id // "primary"' <<<"$args")"
valid_resource_id() { [ -n "$1" ] && [ "${#1}" -le 512 ] && [[ "$1" != -* ]] && [[ "$1" != *$'\n'* ]] && [[ "$1" != *$'\r'* ]]; }
valid_timestamp() { [[ "$1" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}([.][0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})$ ]]; }
timestamp_epoch() {
  local value="$1" normalized
  if date -d "$value" +%s 2>/dev/null; then return 0; fi
  normalized="$(printf '%s' "$value" |
    sed -E 's/\.[0-9]+(Z|[+-][0-9]{2}:[0-9]{2})$/\1/; s/Z$/+0000/; s/([+-][0-9]{2}):([0-9]{2})$/\1\2/')"
  date -j -f '%Y-%m-%dT%H:%M:%S%z' "$normalized" +%s 2>/dev/null
}
valid_resource_id "$calendar_id" || emit_failure BAD_ARGS "calendar_id is malformed." "$account"
event_id="$(jq -r '.event_id // ""' <<<"$args")"
time_min="$(jq -r '.time_min // ""' <<<"$args")"
time_max="$(jq -r '.time_max // ""' <<<"$args")"
[ -z "$time_min" ] || valid_timestamp "$time_min" || emit_failure BAD_ARGS "time_min must be an absolute RFC3339 timestamp." "$account"
[ -z "$time_max" ] || valid_timestamp "$time_max" || emit_failure BAD_ARGS "time_max must be an absolute RFC3339 timestamp." "$account"
if [ -n "$time_min" ] && [ -n "$time_max" ]; then
  min_epoch="$(timestamp_epoch "$time_min")" || emit_failure BAD_ARGS "time_min is not a real RFC3339 timestamp." "$account"
  max_epoch="$(timestamp_epoch "$time_max")" || emit_failure BAD_ARGS "time_max is not a real RFC3339 timestamp." "$account"
  [ "$max_epoch" -gt "$min_epoch" ] || emit_failure BAD_ARGS "time_max must be after time_min." "$account"
fi

cmd=()
case "$kind" in
  calendars) cmd+=(calendars --max 100 --results-only) ;;
  list)
    max="$(jq -r '.max_results // 10' <<<"$args")"
    [[ "$max" =~ ^[0-9]+$ ]] && [ "$max" -ge 1 ] && [ "$max" -le 50 ] || emit_failure BAD_ARGS "max_results must be 1-50." "$account"
    cmd+=(events "$calendar_id" --max "$max" --results-only)
    [ -z "$time_min" ] || cmd+=(--from "$time_min")
    [ -z "$time_max" ] || cmd+=(--to "$time_max")
    ;;
  get)
    valid_resource_id "$event_id" || emit_failure BAD_ARGS "event_id is required and must be well formed for get." "$account"
    cmd+=(event "$calendar_id" "$event_id" --results-only)
    ;;
  search)
    query="$(jq -r '.query // ""' <<<"$args")"
    [ -n "$query" ] && [ "${#query}" -le 512 ] || emit_failure BAD_ARGS "query is required for search and is limited to 512 characters." "$account"
    max="$(jq -r '.max_results // 25' <<<"$args")"
    [[ "$max" =~ ^[0-9]+$ ]] && [ "$max" -ge 1 ] && [ "$max" -le 50 ] || emit_failure BAD_ARGS "max_results must be 1-50." "$account"
    cmd+=(search "$query" --calendar "$calendar_id" --max "$max" --results-only)
    [ -z "$time_min" ] || cmd+=(--from "$time_min")
    [ -z "$time_max" ] || cmd+=(--to "$time_max")
    ;;
  create|update)
    jq -e '.event|type=="object" and ((keys - ["summary","description","location","start","end","timezone","attendees"])|length==0)' >/dev/null <<<"$args" ||
      emit_failure BAD_ARGS "event must be an object containing only documented event fields." "$account"
    jq -e '.event |
      ((has("summary")|not) or (.summary|type=="string")) and
      ((has("description")|not) or (.description|type=="string")) and
      ((has("location")|not) or (.location|type=="string")) and
      ((has("start")|not) or (.start|type=="string")) and
      ((has("end")|not) or (.end|type=="string")) and
      ((has("timezone")|not) or (.timezone|type=="string")) and
      ((has("attendees")|not) or (.attendees|type=="array"))' >/dev/null <<<"$args" ||
      emit_failure BAD_ARGS "event fields must use the documented JSON types." "$account"
    if [ "$kind" = create ]; then
      jq -e '.event.summary|type=="string" and length>0' >/dev/null <<<"$args" || emit_failure BAD_ARGS "create requires event.summary." "$account"
      jq -e '.event.start|type=="string" and length>0' >/dev/null <<<"$args" || emit_failure BAD_ARGS "create requires event.start." "$account"
      jq -e '.event.end|type=="string" and length>0' >/dev/null <<<"$args" || emit_failure BAD_ARGS "create requires event.end." "$account"
      cmd+=(create "$calendar_id")
    else
      valid_resource_id "$event_id" || emit_failure BAD_ARGS "update requires event_id." "$account"
      jq -e '.event|length>0' >/dev/null <<<"$args" || emit_failure BAD_ARGS "update requires at least one event field." "$account"
      cmd+=(update "$calendar_id" "$event_id")
    fi
    for field in summary description location; do
      if jq -e --arg f "$field" '.event|has($f)' >/dev/null <<<"$args"; then
        value="$(jq -r --arg f "$field" '.event[$f]' <<<"$args")"
        cmd+=("--$field" "$value")
      fi
    done
    start="$(jq -r '.event.start // ""' <<<"$args")"; end="$(jq -r '.event.end // ""' <<<"$args")"
    [ -z "$start" ] || { valid_timestamp "$start" || emit_failure BAD_ARGS "event.start must be an absolute RFC3339 timestamp." "$account"; cmd+=(--from "$start"); }
    [ -z "$end" ] || { valid_timestamp "$end" || emit_failure BAD_ARGS "event.end must be an absolute RFC3339 timestamp." "$account"; cmd+=(--to "$end"); }
    if [ -n "$start" ] && [ -n "$end" ]; then
      start_epoch="$(timestamp_epoch "$start")" || emit_failure BAD_ARGS "event.start is not a real RFC3339 timestamp." "$account"
      end_epoch="$(timestamp_epoch "$end")" || emit_failure BAD_ARGS "event.end is not a real RFC3339 timestamp." "$account"
      [ "$end_epoch" -gt "$start_epoch" ] || emit_failure BAD_ARGS "event.end must be after event.start." "$account"
    fi
    timezone="$(jq -r '.event.timezone // ""' <<<"$args")"
    [ -z "$timezone" ] || { [[ "$timezone" =~ ^[A-Za-z_+-]+(/[A-Za-z0-9_+-]+)+$ ]] || emit_failure BAD_ARGS "event.timezone must be an IANA timezone." "$account"; cmd+=(--timezone "$timezone"); }
    if jq -e '.event|has("attendees")' >/dev/null <<<"$args"; then
      jq -e '.event.attendees|type=="array" and length<=100 and all(.[]; type=="string" and length<=254 and test("^[^[:space:]@]+@[^[:space:]@]+\\.[^[:space:]@]+$"))' >/dev/null <<<"$args" ||
        emit_failure BAD_ARGS "event.attendees must contain valid email addresses." "$account"
      attendees="$(jq -r '.event.attendees|join(",")' <<<"$args")"
      cmd+=(--attendees "$attendees")
    fi
    cmd+=(--send-updates all --results-only)
    ;;
  delete)
    valid_resource_id "$event_id" || emit_failure BAD_ARGS "delete requires event_id." "$account"
    jq -e '.confirm==true' >/dev/null <<<"$args" || emit_failure BAD_ARGS "delete requires confirm:true because deletion is permanent." "$account"
    cmd+=(delete "$calendar_id" "$event_id" --send-updates all --force --results-only)
    ;;
  freebusy)
    [ -n "$time_min" ] && [ -n "$time_max" ] || emit_failure BAD_ARGS "freebusy requires time_min and time_max." "$account"
    cmd+=(freebusy "$calendar_id" --from "$time_min" --to "$time_max" --results-only)
    ;;
esac

bridge_active=1
if ! gcal_prepare_account "$account"; then
  code="${GCAL_PREPARE_CODE:-GOG_PROTOCOL_ERROR}"
  emit_failure "$code" "$(gcal_safe_message "$code")" "$account"
fi
set +e
gcal_run_capture "${cmd[@]}"
gog_rc=$?
set -e
sync_ok=true
gcal_sync_account "$account" "$GCAL_CLIENT" "$GCAL_EMAIL" || sync_ok=false
if [ "$gog_rc" -ne 0 ]; then
  code="$(gcal_error_code "$GCAL_GOG_STDERR" "$gog_rc")"
  emit_failure "$code" "$(gcal_safe_message "$code")" "$account"
fi
[ "$sync_ok" = true ] || emit_failure GOG_PROTOCOL_ERROR "gog token writeback could not be persisted atomically." "$account"
data="$(jq -c '.' "$GCAL_GOG_STDOUT" 2>/dev/null)" || emit_failure GOG_PROTOCOL_ERROR "gog returned invalid JSON." "$account"
case "$kind" in
  calendars) text="Listed calendars for $account." ;;
  list) text="Listed calendar events for $account." ;;
  get) text="Retrieved event $event_id for $account." ;;
  search) text="Searched calendar events for $account." ;;
  create) text="Created a calendar event for $account." ;;
  update) text="Updated event $event_id for $account." ;;
  delete) text="Deleted event $event_id for $account." ;;
  freebusy) text="Retrieved free/busy information for $account." ;;
esac
finish "$handle" "$kind" "$account" "$text" "$data"
