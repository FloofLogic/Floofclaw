#!/usr/bin/env bash
# Shared gcal implementation helpers. This file is sourced by run.sh and
# auth.sh; it is never an agent-facing executable.

GCAL_ALIAS_MAX=32
GCAL_GOG_MIN_VERSION="0.12.0"
GCAL_TOKEN_URL="${GCAL_TOKEN_URL:-https://oauth2.googleapis.com/token}"
GCAL_SCOPE_JSON='["https://www.googleapis.com/auth/calendar"]'
GCAL_ORIGINAL_SCOPE_JSON='["https://www.googleapis.com/auth/calendar.events"]'
GCAL_ORIGINAL_ACCOUNT="original"
SECRET_VALUE=""
SECRET_LIST=""
GCAL_BRIDGE=""
GCAL_GOG_STDOUT=""
GCAL_GOG_STDERR=""
GCAL_CREDENTIAL_LOCK=""
GCAL_PREPARE_STAGE=""
GCAL_PREPARE_CODE=""
GCAL_ACCESS_TOKEN=""

gcal_unlock_credentials() {
  if [ -n "$GCAL_CREDENTIAL_LOCK" ] &&
      [[ "$GCAL_CREDENTIAL_LOCK" == */.fclaw/action-locks/gcal-credentials.lock ]]; then
    rm -f -- "$GCAL_CREDENTIAL_LOCK/pid"
    rmdir -- "$GCAL_CREDENTIAL_LOCK" 2>/dev/null || true
  fi
  GCAL_CREDENTIAL_LOCK=""
}

gcal_lock_credentials() {
  local root="${FCLAW_HOME:-.}/.fclaw/action-locks"
  local lock="$root/gcal-credentials.lock" owner="" attempt
  [ -n "$GCAL_CREDENTIAL_LOCK" ] && return 0
  mkdir -p -- "$root" || return 1
  chmod 700 "${FCLAW_HOME:-.}/.fclaw" "$root" 2>/dev/null || true
  for attempt in {1..200}; do
    if mkdir -- "$lock" 2>/dev/null; then
      chmod 700 "$lock" 2>/dev/null || true
      printf '%s\n' "$$" >"$lock/pid" || { rmdir -- "$lock" 2>/dev/null || true; return 1; }
      chmod 600 "$lock/pid" 2>/dev/null || true
      GCAL_CREDENTIAL_LOCK="$lock"
      return 0
    fi
    owner=""
    if [ -r "$lock/pid" ]; then IFS= read -r owner <"$lock/pid" || true; fi
    if [[ "$owner" =~ ^[0-9]+$ ]] && ! kill -0 "$owner" 2>/dev/null; then
      rm -f -- "$lock/pid"
      rmdir -- "$lock" 2>/dev/null || true
      continue
    fi
    sleep 0.05
  done
  return 1
}

gcal_die() {
  printf '%s\n' "$2" >&2
  exit "${1:-1}"
}

gcal_require_tools() {
  command -v jq >/dev/null 2>&1 || gcal_die 1 "gcal requires jq"
  [ -n "${FCLAW_SECRET_FD-}" ] ||
    gcal_die 1 "gcal requires the FloofClaw private credential channel"
}

gcal_alias_valid() {
  local alias="${1-}"
  [ -n "$alias" ] && [ "${#alias}" -le "$GCAL_ALIAS_MAX" ] &&
    [[ "$alias" =~ ^[a-z0-9_-]+$ ]]
}

gcal_email_valid() {
  local email="${1-}"
  [ -n "$email" ] && [ "${#email}" -le 254 ] &&
    [[ "$email" =~ ^[^[:space:]@]+@[^[:space:]@]+\.[^[:space:]@]+$ ]]
}

gcal_secret_get() {
  local suffix="$1" header len
  SECRET_VALUE=""
  printf 'GET %s\n' "$suffix" >&"$FCLAW_SECRET_FD" || return 1
  IFS= read -r header <&"$FCLAW_SECRET_FD" || return 1
  case "$header" in
    "NOT_FOUND") return 1 ;;
    VALUE\ *)
      len="${header#VALUE }"
      [[ "$len" =~ ^[0-9]+$ ]] || return 1
      [ "$len" -le 8192 ] || return 1
      if [ "$len" -gt 0 ]; then
        IFS= read -r -N "$len" SECRET_VALUE <&"$FCLAW_SECRET_FD" || return 1
      fi
      return 0
      ;;
    *) return 1 ;;
  esac
}

gcal_secret_set() {
  local suffix="$1" value="$2" header
  [ -n "$value" ] || return 1
  [ "${#value}" -le 8192 ] || return 1
  printf 'SET %s %d\n' "$suffix" "${#value}" >&"$FCLAW_SECRET_FD" || return 1
  printf '%s' "$value" >&"$FCLAW_SECRET_FD" || return 1
  IFS= read -r header <&"$FCLAW_SECRET_FD" || return 1
  [ "$header" = "OK" ]
}

gcal_secret_delete() {
  local suffix="$1" header
  printf 'DELETE %s\n' "$suffix" >&"$FCLAW_SECRET_FD" || return 1
  IFS= read -r header <&"$FCLAW_SECRET_FD" || return 1
  [ "$header" = "OK" ]
}

gcal_secret_list() {
  local header len
  SECRET_LIST=""
  printf 'LIST\n' >&"$FCLAW_SECRET_FD" || return 1
  IFS= read -r header <&"$FCLAW_SECRET_FD" || return 1
  case "$header" in
    LIST\ *)
      len="${header#LIST }"
      [[ "$len" =~ ^[0-9]+$ ]] || return 1
      [ "$len" -le 32700 ] || return 1
      if [ "$len" -gt 0 ]; then
        IFS= read -r -N "$len" SECRET_LIST <&"$FCLAW_SECRET_FD" || return 1
      fi
      return 0
      ;;
    *) return 1 ;;
  esac
}

gcal_original_ready() {
  local email=""
  gcal_secret_get account_email || return 1
  email="$SECRET_VALUE"
  gcal_email_valid "$email" || return 1
  gcal_secret_get client_id || return 1
  gcal_secret_get client_secret || return 1
  gcal_secret_get refresh_token || return 1
}

gcal_account_field() {
  local account="$1" field="$2"
  if [ "$account" = "$GCAL_ORIGINAL_ACCOUNT" ]; then
    case "$field" in
      email) gcal_secret_get account_email ;;
      client) SECRET_VALUE="$GCAL_ORIGINAL_ACCOUNT"; return 0 ;;
      refresh_token) gcal_secret_get refresh_token ;;
      # The shipped single-account setup used OAuth Playground with the
      # calendar.events grant. Preserve that exact authority instead of
      # claiming the broader calendar scope used by newer gog onboarding.
      scopes) SECRET_VALUE="$GCAL_ORIGINAL_SCOPE_JSON"; return 0 ;;
      *) gcal_secret_get "account__${account}__${field}" ;;
    esac
    return $?
  fi
  gcal_secret_get "account__${account}__${field}"
}

gcal_client_field() {
  local client="$1" field="$2"
  if [ "$client" = "$GCAL_ORIGINAL_ACCOUNT" ]; then
    case "$field" in
      client_id|client_secret) gcal_secret_get "$field" ;;
      *) return 1 ;;
    esac
    return $?
  fi
  gcal_secret_get "client__${client}__${field}"
}

gcal_list_aliases() {
  local record_kind="$1" field="$2" names aliases
  gcal_secret_list || return 1
  names="$SECRET_LIST"
  aliases="$(printf '%s' "$names" |
    sed -n "s/^${record_kind}__\([a-z0-9_-][a-z0-9_-]*\)__${field}$/\1/p")"
  if { [ "$record_kind:$field" = account:email ] ||
       [ "$record_kind:$field" = client:client_id ]; } &&
      gcal_original_ready; then
    aliases="${aliases}${aliases:+$'\n'}${GCAL_ORIGINAL_ACCOUNT}"
  fi
  printf '%s\n' "$aliases" | sed '/^$/d' | LC_ALL=C sort -u
}

gcal_select_account() {
  local requested="${1-}" aliases count
  # Existing single-account installations remain authoritative. When their
  # original credential names plus account_email are present, every ordinary
  # call uses that record regardless of requested/default aliases or partially
  # configured newer records.
  if gcal_original_ready; then
    printf '%s' "$GCAL_ORIGINAL_ACCOUNT"
    return 0
  fi
  if [ -n "$requested" ]; then
    gcal_alias_valid "$requested" || return 2
    gcal_account_field "$requested" email || return 3
    printf '%s' "$requested"
    return 0
  fi
  if gcal_secret_get default_account && gcal_alias_valid "$SECRET_VALUE"; then
    requested="$SECRET_VALUE"
    if gcal_account_field "$requested" email; then
      printf '%s' "$requested"
      return 0
    fi
  fi
  aliases="$(gcal_list_aliases account email)" || return 4
  count="$(printf '%s\n' "$aliases" | sed '/^$/d' | wc -l | tr -d ' ')"
  if [ "$count" = "1" ]; then
    printf '%s' "$aliases"
    return 0
  fi
  [ "$count" = "0" ] && return 3
  return 5
}

gcal_version() {
  local raw
  command -v gog >/dev/null 2>&1 || return 1
  raw="$(gog --version 2>/dev/null || gog version 2>/dev/null || true)"
  printf '%s' "$raw" | sed -nE 's/.*v?([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -n1
}

gcal_version_compatible() {
  local got="$1" need="$GCAL_GOG_MIN_VERSION"
  local ga gb gc na nb nc
  IFS=. read -r ga gb gc <<<"$got"
  IFS=. read -r na nb nc <<<"$need"
  [[ "$ga" =~ ^[0-9]+$ && "$gb" =~ ^[0-9]+$ && "$gc" =~ ^[0-9]+$ ]] || return 1
  [ "$ga" -gt "$na" ] && return 0
  [ "$ga" -lt "$na" ] && return 1
  [ "$gb" -gt "$nb" ] && return 0
  [ "$gb" -lt "$nb" ] && return 1
  [ "$gc" -ge "$nc" ]
}

gcal_temp_base() {
  local candidate="${TMPDIR:-/tmp}" resolved="" cwd="" workspace="" store=""
  resolved="$(cd "$candidate" 2>/dev/null && pwd -P)" || resolved=/tmp
  cwd="$(pwd -P)"
  if [ -n "${FCLAW_WORKSPACE_ROOT-}" ]; then
    workspace="$(cd "$FCLAW_WORKSPACE_ROOT" 2>/dev/null && pwd -P)" || workspace=""
  fi
  if [ -n "${FCLAW_HOME-}" ]; then
    store="$(cd "$FCLAW_HOME" 2>/dev/null && pwd -P)" || store=""
  fi
  if [[ "$resolved/" == "$cwd/"* ]] ||
      { [ -n "$workspace" ] && [[ "$resolved/" == "$workspace/"* ]]; } ||
      { [ -n "$store" ] && [[ "$resolved/" == "$store/"* ]]; } ||
      { [ -n "${HOME-}" ] && [[ "$resolved/" == "$HOME/"* ]]; }; then
    resolved=/tmp
  fi
  [ -d "$resolved" ] && [ -w "$resolved" ] || return 1
  printf '%s' "$resolved"
}

gcal_cleanup_abandoned() {
  local base
  base="$(gcal_temp_base)" || return 0
  find "$base" -maxdepth 1 -type d -name 'fclaw-gcal-bridge.*' -mtime +1 \
    -exec rm -rf -- {} + 2>/dev/null || true
}

gcal_cleanup_bridge() {
  if [ -n "$GCAL_BRIDGE" ] && [[ "$GCAL_BRIDGE" == */fclaw-gcal-bridge.* ]]; then
    rm -rf -- "$GCAL_BRIDGE"
  fi
  GCAL_BRIDGE=""
  GCAL_ACCESS_TOKEN=""
  unset GOG_HOME GOG_KEYRING_BACKEND GOG_KEYRING_PASSWORD
}

gcal_new_direct_bridge() {
  local base
  GCAL_PREPARE_STAGE=temp_home
  gcal_cleanup_abandoned
  base="$(gcal_temp_base)" || return 1
  GCAL_BRIDGE="$(mktemp -d "$base/fclaw-gcal-bridge.XXXXXX")" || return 1
  chmod 700 "$GCAL_BRIDGE"
  mkdir -m 700 "$GCAL_BRIDGE/home"
  export GOG_HOME="$GCAL_BRIDGE/home"
}

# Original installations already own a refresh-token bundle. Exchange it
# directly, then give gog only the short-lived access token through its
# documented environment input. This avoids translating the original grant
# through gog's durable keyring schema or newer account records.
gcal_prepare_original_access() {
  local client_id client_secret refresh token_cfg token_response curl_rc error
  GCAL_PREPARE_CODE=""
  GCAL_PREPARE_STAGE=token_material
  command -v curl >/dev/null 2>&1 || {
    GCAL_PREPARE_CODE=DEPENDENCY_MISSING
    return 1
  }
  gcal_client_field "$GCAL_ORIGINAL_ACCOUNT" client_id || return 1
  client_id="$SECRET_VALUE"
  gcal_client_field "$GCAL_ORIGINAL_ACCOUNT" client_secret || return 1
  client_secret="$SECRET_VALUE"
  gcal_account_field "$GCAL_ORIGINAL_ACCOUNT" refresh_token || return 1
  refresh="$SECRET_VALUE"
  gcal_new_direct_bridge || return 1
  token_cfg="$GCAL_BRIDGE/token.curl"
  token_response="$GCAL_BRIDGE/token.json"
  {
    printf 'data-urlencode = "client_id=%s"\n' "$client_id"
    printf 'data-urlencode = "client_secret=%s"\n' "$client_secret"
    printf 'data-urlencode = "refresh_token=%s"\n' "$refresh"
    printf 'data-urlencode = "grant_type=refresh_token"\n'
  } >"$token_cfg" || return 1
  chmod 600 "$token_cfg"
  GCAL_PREPARE_STAGE=token_exchange
  set +e
  curl --silent --show-error --proto '=https' --connect-timeout 10 --max-time 30 \
    --request POST --config "$token_cfg" "$GCAL_TOKEN_URL" >"$token_response"
  curl_rc=$?
  set -e
  chmod 600 "$token_response" 2>/dev/null || true
  if [ "$curl_rc" -ne 0 ]; then
    GCAL_PREPARE_CODE=RETRYABLE
    return 1
  fi
  GCAL_ACCESS_TOKEN="$(jq -r '.access_token // empty' "$token_response" 2>/dev/null || true)"
  if [ -z "$GCAL_ACCESS_TOKEN" ]; then
    error="$(jq -r '.error // empty' "$token_response" 2>/dev/null || true)"
    case "$error" in
      invalid_grant) GCAL_PREPARE_CODE=REAUTHORIZATION_REQUIRED ;;
      invalid_client|unauthorized_client) GCAL_PREPARE_CODE=CLIENT_INVALID ;;
      temporarily_unavailable) GCAL_PREPARE_CODE=RETRYABLE ;;
      *) GCAL_PREPARE_CODE=GOG_PROTOCOL_ERROR ;;
    esac
    return 1
  fi
  GCAL_PREPARE_STAGE=access_token_ready
}

gcal_new_bridge() {
  local client="$1" client_id client_secret base
  GCAL_PREPARE_STAGE=temp_home
  gcal_cleanup_abandoned
  base="$(gcal_temp_base)" || return 1
  GCAL_BRIDGE="$(mktemp -d "$base/fclaw-gcal-bridge.XXXXXX")" || return 1
  chmod 700 "$GCAL_BRIDGE"
  mkdir -m 700 "$GCAL_BRIDGE/home"
  export GOG_HOME="$GCAL_BRIDGE/home"
  export GOG_KEYRING_BACKEND=file
  GOG_KEYRING_PASSWORD="$(od -An -N32 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')"
  [ -n "$GOG_KEYRING_PASSWORD" ] || return 1
  export GOG_KEYRING_PASSWORD
  GCAL_PREPARE_STAGE=client_credentials
  gcal_client_field "$client" client_id || return 2
  client_id="$SECRET_VALUE"
  gcal_client_field "$client" client_secret || return 2
  client_secret="$SECRET_VALUE"
  jq -cn --arg id "$client_id" --arg secret "$client_secret" \
    '{installed:{client_id:$id,client_secret:$secret,auth_uri:"https://accounts.google.com/o/oauth2/auth",token_uri:"https://oauth2.googleapis.com/token",redirect_uris:["http://localhost"]}}' \
    >"$GCAL_BRIDGE/credentials.json" || return 1
  chmod 600 "$GCAL_BRIDGE/credentials.json"
  GCAL_PREPARE_STAGE=client_import
  if ! gog --home "$GOG_HOME" --client "$client" --no-input --json \
      auth credentials "$GCAL_BRIDGE/credentials.json" \
      >"$GCAL_BRIDGE/credentials.out" 2>"$GCAL_BRIDGE/credentials.err"; then
    return 3
  fi
}

gcal_seed_account() {
  local account="$1" client="$2" email="$3"
  local refresh access expires scopes token_json
  GCAL_PREPARE_STAGE=token_material
  gcal_account_field "$account" refresh_token || return 2
  refresh="$SECRET_VALUE"
  access=""; expires=""; scopes="$GCAL_SCOPE_JSON"
  if gcal_account_field "$account" access_token; then access="$SECRET_VALUE"; fi
  if gcal_account_field "$account" access_token_expires_at; then expires="$SECRET_VALUE"; fi
  if gcal_account_field "$account" scopes && jq -e 'type=="array"' >/dev/null 2>&1 <<<"$SECRET_VALUE"; then
    scopes="$SECRET_VALUE"
  fi
  token_json="$(jq -cn --arg email "$email" --arg client "$client" \
      --arg refresh "$refresh" --arg access "$access" --arg expires "$expires" \
      --argjson scopes "$scopes" \
      '{email:$email,client:$client,services:["calendar"],scopes:$scopes,refresh_token:$refresh}
       + (if $access!="" then {access_token:$access} else {} end)
       + (if $expires!="" then {access_token_expires_at:$expires} else {} end)')" || return 1
  printf '%s\n' "$token_json" >"$GCAL_BRIDGE/token-import.json"
  chmod 600 "$GCAL_BRIDGE/token-import.json"
  GCAL_PREPARE_STAGE=token_import
  gog --home "$GOG_HOME" --client "$client" --no-input --json --force \
    auth tokens import "$GCAL_BRIDGE/token-import.json" \
    >"$GCAL_BRIDGE/import.out" 2>"$GCAL_BRIDGE/import.err"
}

gcal_sync_account() {
  local account="$1" client="$2" email="$3"
  local exported refresh access expires scopes
  local old_refresh="" old_access="" old_expires="" old_scopes=""
  local had_refresh=0 had_access=0 had_expires=0 had_scopes=0 failed=0
  if [ "$account" = "$GCAL_ORIGINAL_ACCOUNT" ] && [ -n "$GCAL_ACCESS_TOKEN" ]; then
    return 0
  fi
  rm -f -- "$GCAL_BRIDGE/token-export.json"
  gog --home "$GOG_HOME" --client "$client" --no-input --json \
    auth tokens export "$email" --out "$GCAL_BRIDGE/token-export.json" --overwrite \
    >"$GCAL_BRIDGE/export.out" 2>"$GCAL_BRIDGE/export.err" || return 1
  exported="$GCAL_BRIDGE/token-export.json"
  jq -e --arg email "$email" '.email==$email and (.refresh_token|type=="string" and length>0)' \
    "$exported" >/dev/null 2>&1 || return 1
  refresh="$(jq -r '.refresh_token' "$exported")"
  access="$(jq -r '.access_token // empty' "$exported")"
  expires="$(jq -r '.access_token_expires_at // empty' "$exported")"
  scopes="$(jq -c '.scopes // []' "$exported")"

  if [ "$account" = "$GCAL_ORIGINAL_ACCOUNT" ]; then
    # The original layout has one durable rotating field. The exported token
    # was validated above, so this single replacement is the atomic commit.
    gcal_secret_set refresh_token "$refresh"
    return $?
  fi

  if gcal_account_field "$account" refresh_token; then old_refresh="$SECRET_VALUE"; had_refresh=1; fi
  if gcal_account_field "$account" access_token; then old_access="$SECRET_VALUE"; had_access=1; fi
  if gcal_account_field "$account" access_token_expires_at; then old_expires="$SECRET_VALUE"; had_expires=1; fi
  if gcal_account_field "$account" scopes; then old_scopes="$SECRET_VALUE"; had_scopes=1; fi

  if [ -n "$access" ]; then
    gcal_secret_set "account__${account}__access_token" "$access" || failed=1
  else
    gcal_secret_delete "account__${account}__access_token" || failed=1
  fi
  if [ "$failed" -eq 0 ]; then
    if [ -n "$expires" ]; then
      gcal_secret_set "account__${account}__access_token_expires_at" "$expires" || failed=1
    else
      gcal_secret_delete "account__${account}__access_token_expires_at" || failed=1
    fi
  fi
  [ "$failed" -ne 0 ] || gcal_secret_set "account__${account}__scopes" "$scopes" || failed=1
  # Commit the refresh token last. It is the durable recovery authority and
  # must remain unchanged if any companion field could not be stored.
  [ "$failed" -ne 0 ] || gcal_secret_set "account__${account}__refresh_token" "$refresh" || failed=1
  [ "$failed" -eq 0 ] && return 0

  if [ "$had_access" -eq 1 ]; then gcal_secret_set "account__${account}__access_token" "$old_access" || true
  else gcal_secret_delete "account__${account}__access_token" || true; fi
  if [ "$had_expires" -eq 1 ]; then gcal_secret_set "account__${account}__access_token_expires_at" "$old_expires" || true
  else gcal_secret_delete "account__${account}__access_token_expires_at" || true; fi
  if [ "$had_scopes" -eq 1 ]; then gcal_secret_set "account__${account}__scopes" "$old_scopes" || true
  else gcal_secret_delete "account__${account}__scopes" || true; fi
  if [ "$had_refresh" -eq 1 ]; then gcal_secret_set "account__${account}__refresh_token" "$old_refresh" || true
  else gcal_secret_delete "account__${account}__refresh_token" || true; fi
  return 1
}

gcal_prepare_account() {
  local account="$1"
  GCAL_PREPARE_CODE=""
  GCAL_PREPARE_STAGE=account_email
  gcal_account_field "$account" email || return 2
  GCAL_EMAIL="$SECRET_VALUE"
  GCAL_PREPARE_STAGE=account_client
  gcal_account_field "$account" client || return 3
  GCAL_CLIENT="$SECRET_VALUE"
  gcal_alias_valid "$GCAL_CLIENT" || return 3
  if [ "$account" = "$GCAL_ORIGINAL_ACCOUNT" ]; then
    gcal_prepare_original_access
    return $?
  fi
  gcal_new_bridge "$GCAL_CLIENT" || return $?
  gcal_seed_account "$account" "$GCAL_CLIENT" "$GCAL_EMAIL"
}

gcal_run_capture() {
  GCAL_GOG_STDOUT="$GCAL_BRIDGE/gog.out"
  GCAL_GOG_STDERR="$GCAL_BRIDGE/gog.err"
  if [ -n "$GCAL_ACCESS_TOKEN" ]; then
    GOG_ACCESS_TOKEN="$GCAL_ACCESS_TOKEN" gog --home "$GOG_HOME" \
      --account "$GCAL_EMAIL" --no-input --json --enable-commands calendar calendar "$@" \
      >"$GCAL_GOG_STDOUT" 2>"$GCAL_GOG_STDERR"
  else
    gog --home "$GOG_HOME" --client "$GCAL_CLIENT" --account "$GCAL_EMAIL" \
      --no-input --json --enable-commands calendar calendar "$@" \
      >"$GCAL_GOG_STDOUT" 2>"$GCAL_GOG_STDERR"
  fi
}

gcal_error_code() {
  local file="$1" exit_code="${2-1}" text
  text="$(tr '[:upper:]' '[:lower:]' <"$file" 2>/dev/null | head -c 8192)"
  case "$text" in
    *invalid_grant*|*refresh\ token*missing*|*reauth*|*authentication*required*) printf REAUTHORIZATION_REQUIRED ;;
    *accessnotconfigured*|*google\ calendar\ api*not*used*|*google\ calendar\ api*disabled*) printf API_DISABLED ;;
    *insufficient*scope*|*insufficient*permission*) printf INSUFFICIENT_SCOPE ;;
    *permission*denied*|*forbidden*|*" 403"*) printf PERMISSION_DENIED ;;
    *rate*limit*|*resource_exhausted*|*" 429"*) printf RATE_LIMITED ;;
    *not*found*|*" 404"*) printf NOT_FOUND ;;
    *timeout*|*timed*out*|*network*|*connection*|*temporary*|*unavailable*) printf RETRYABLE ;;
    *)
      case "$exit_code" in
        4) printf REAUTHORIZATION_REQUIRED ;;
        5) printf NOT_FOUND ;;
        6) printf PERMISSION_DENIED ;;
        7) printf RATE_LIMITED ;;
        8) printf RETRYABLE ;;
        *) printf GOG_PROTOCOL_ERROR ;;
      esac
      ;;
  esac
}

gcal_safe_message() {
  case "$1" in
    REAUTHORIZATION_REQUIRED) printf 'Google rejected the stored grant.' ;;
    API_DISABLED) printf 'The Google Calendar API is disabled for this OAuth project.' ;;
    INSUFFICIENT_SCOPE) printf 'The stored Google grant lacks Calendar scope.' ;;
    PERMISSION_DENIED) printf 'Google denied this Calendar operation.' ;;
    RATE_LIMITED) printf 'Google rate-limited this Calendar operation.' ;;
    RETRYABLE) printf 'The Calendar request failed because of a temporary network or service problem.' ;;
    NOT_FOUND) printf 'The requested calendar resource was not found.' ;;
    GOG_MISSING) printf 'The gog executable is not installed.' ;;
    GOG_INCOMPATIBLE) printf 'The installed gog version is incompatible.' ;;
    AUTH_MISSING) printf 'The account has no stored OAuth grant.' ;;
    CLIENT_INVALID) printf 'Google rejected the stored OAuth client.' ;;
    DEPENDENCY_MISSING) printf 'The Calendar action is missing a required local dependency.' ;;
    *) printf 'gog returned an unsupported or malformed result.' ;;
  esac
}

gcal_api_enable_url() {
  local file="${1-}" account="${2-}" url="" client="" client_id="" project=""
  if [ -f "$file" ]; then
    url="$(grep -Eo 'https://console\.developers\.google\.com/apis/api/calendar-json\.googleapis\.com/overview\?project=[0-9]+' "$file" 2>/dev/null | head -n1 || true)"
  fi
  if [ -n "$url" ]; then printf '%s' "$url"; return 0; fi
  [ -n "$account" ] || return 1
  gcal_account_field "$account" client || return 1
  client="$SECRET_VALUE"
  gcal_client_field "$client" client_id || return 1
  client_id="$SECRET_VALUE"
  project="${client_id%%-*}"
  [[ "$project" =~ ^[0-9]+$ ]] || return 1
  printf 'https://console.cloud.google.com/apis/library/calendar-json.googleapis.com?project=%s' "$project"
}

gcal_reauth_command() {
  local account="$1" root
  root="$(pwd -P)"
  printf 'cd %q && ./bin/fclaw action auth -h --name gcal -- reauthorize %q --force-consent' \
    "$root" "$account"
}
