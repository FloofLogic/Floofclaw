#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
. "$DIR/lib.sh"
gcal_require_tools

GCAL_AUTH_BRIDGE=0
gcal_auth_exit_cleanup() {
  if [ "$GCAL_AUTH_BRIDGE" -eq 1 ]; then gcal_cleanup_bridge; fi
  gcal_unlock_credentials
}
trap gcal_auth_exit_cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
gcal_lock_credentials || gcal_die 1 "another gcal credential operation is active; retry shortly"

usage() {
  cat >&2 <<'EOF'
usage:
  fclaw action auth -h --name gcal -- client add <client> --credentials <desktop-client.json>
  fclaw action auth -h --name gcal -- client restore-original --credentials <desktop-client.json> --yes
  fclaw action auth -h --name gcal -- client list
  fclaw action auth -h --name gcal -- client remove <client> --yes
  fclaw action auth -h --name gcal -- account add <account> --email <email> --client <client>
  fclaw action auth -h --name gcal -- account list
  fclaw action auth -h --name gcal -- account default <account>
  fclaw action auth -h --name gcal -- account remove <account> --yes
  fclaw action auth -h --name gcal -- authorize <account> [--manual]
  fclaw action auth -h --name gcal -- authorize <account> --remote --step 1
  fclaw action auth -h --name gcal -- authorize <account> --remote --step 2 --redirect-url-stdin
  fclaw action auth -h --name gcal -- reauthorize <account> --force-consent
  fclaw action auth -h --name gcal -- status [<account>]
  fclaw action auth -h --name gcal -- migrate --client <client> --account <account> --email <email> --yes
EOF
  exit 2
}

need_alias() {
  gcal_alias_valid "${1-}" || gcal_die 2 "aliases must be lowercase [a-z0-9_-], 1-$GCAL_ALIAS_MAX characters"
}

confirm_remove() {
  local label="$1" yes="${2-}"
  [ "$yes" = --yes ] && return 0
  if [ -t 0 ]; then
    printf 'Remove %s? Type yes to continue: ' "$label" >&2
    local answer=""
    IFS= read -r answer
    [ "$answer" = yes ] && return 0
  fi
  gcal_die 2 "removal cancelled; rerun with --yes for explicit confirmation"
}

client_add() {
  [ "$#" -eq 3 ] && [ "$2" = --credentials ] || usage
  local alias="$1" file="$3" id secret old_id="" old_secret="" had_id=0 had_secret=0
  need_alias "$alias"
  [ "$alias" != "$GCAL_ORIGINAL_ACCOUNT" ] || gcal_die 2 "client alias $GCAL_ORIGINAL_ACCOUNT is reserved for the original credential layout"
  [ -f "$file" ] || gcal_die 2 "credentials file does not exist"
  jq -e '.installed|type=="object" and (.client_id|type=="string" and length>0) and (.client_secret|type=="string" and length>0) and (.redirect_uris|type=="array") and (.web|not)' "$file" >/dev/null 2>&1 ||
    gcal_die 2 "credentials must be valid Google Desktop app OAuth JSON (the installed object)"
  id="$(jq -r '.installed.client_id' "$file")"
  secret="$(jq -r '.installed.client_secret' "$file")"
  [ "${#id}" -le 8192 ] && [ "${#secret}" -le 8192 ] || gcal_die 2 "OAuth credentials are too large"
  if gcal_client_field "$alias" client_id; then old_id="$SECRET_VALUE"; had_id=1; fi
  if gcal_client_field "$alias" client_secret; then old_secret="$SECRET_VALUE"; had_secret=1; fi
  if ! gcal_secret_set "client__${alias}__client_id" "$id"; then gcal_die 1 "could not store OAuth client"; fi
  if ! gcal_secret_set "client__${alias}__client_secret" "$secret"; then
    if [ "$had_id" -eq 1 ]; then gcal_secret_set "client__${alias}__client_id" "$old_id" || true; else gcal_secret_delete "client__${alias}__client_id" || true; fi
    if [ "$had_secret" -eq 1 ]; then gcal_secret_set "client__${alias}__client_secret" "$old_secret" || true; fi
    gcal_die 1 "could not atomically store OAuth client; prior record was retained"
  fi
  printf 'stored OAuth client %s (secret values were not printed)\n' "$alias"
  printf 'next: fclaw action auth -h --name gcal -- account add <account> --email <email> --client %s\n' "$alias"
}

client_restore_original() {
  [ "$#" -eq 3 ] && [ "$1" = --credentials ] && [ "$3" = --yes ] || usage
  local file="$2" id secret old_id="" old_secret="" had_id=0 had_secret=0
  [ -f "$file" ] || gcal_die 2 "credentials file does not exist"
  jq -e '
    (.installed | type == "object") and
    (.installed.client_id | type == "string" and length > 0) and
    (.installed.client_secret | type == "string" and length > 0) and
    (.installed.redirect_uris | type == "array" and index("http://localhost") != null) and
    (has("web") | not)
  ' "$file" >/dev/null 2>&1 ||
    gcal_die 2 "original credentials must be Google Desktop app OAuth JSON with http://localhost registered"
  id="$(jq -r '.installed.client_id' "$file")"
  secret="$(jq -r '.installed.client_secret' "$file")"
  [ "${#id}" -le 8192 ] && [ "${#secret}" -le 8192 ] || gcal_die 2 "OAuth credentials are too large"
  if gcal_secret_get client_id; then old_id="$SECRET_VALUE"; had_id=1; fi
  if gcal_secret_get client_secret; then old_secret="$SECRET_VALUE"; had_secret=1; fi
  gcal_secret_set client_id "$id" || gcal_die 1 "could not store original OAuth client id"
  if ! gcal_secret_set client_secret "$secret"; then
    if [ "$had_id" -eq 1 ]; then gcal_secret_set client_id "$old_id" || true; else gcal_secret_delete client_id || true; fi
    if [ "$had_secret" -eq 1 ]; then gcal_secret_set client_secret "$old_secret" || true; fi
    gcal_die 1 "could not atomically restore original OAuth client; prior record was retained"
  fi
  printf 'restored original OAuth client from Desktop credentials (secret values were not printed)\n'
  printf 'next: %s\n' "$(gcal_reauth_command original)"
}

client_list() {
  local aliases alias present
  aliases="$(gcal_list_aliases client client_id)" || gcal_die 1 "could not list clients"
  printf 'CLIENT\tCONFIGURED\n'
  while IFS= read -r alias; do
    [ -n "$alias" ] || continue
    present=no
    if gcal_client_field "$alias" client_secret; then present=yes; fi
    printf '%s\t%s\n' "$alias" "$present"
  done <<<"$aliases"
}

client_remove() {
  [ "$#" -eq 2 ] || usage
  local alias="$1" yes="$2" account aliases linked=""
  need_alias "$alias"; [ "$yes" = --yes ] || confirm_remove "OAuth client $alias" "$yes"
  [ "$alias" != "$GCAL_ORIGINAL_ACCOUNT" ] || gcal_die 2 "the original credential layout is managed with fclaw auth, not client remove"
  aliases="$(gcal_list_aliases account email)"
  while IFS= read -r account; do
    [ -n "$account" ] || continue
    if gcal_account_field "$account" client && [ "$SECRET_VALUE" = "$alias" ]; then linked="${linked}${linked:+, }$account"; fi
  done <<<"$aliases"
  [ -z "$linked" ] || gcal_die 2 "client $alias is still used by account(s): $linked; remove or reassign them first"
  gcal_secret_delete "client__${alias}__client_id" || gcal_die 1 "could not remove client"
  gcal_secret_delete "client__${alias}__client_secret" || gcal_die 1 "could not remove client"
  printf 'removed OAuth client %s\n' "$alias"
}

account_add() {
  [ "$#" -eq 5 ] && [ "$2" = --email ] && [ "$4" = --client ] || usage
  local alias="$1" email="$3" client="$5" old_email="" old_client="" had_email=0 had_client=0
  need_alias "$alias"; need_alias "$client"; gcal_email_valid "$email" || gcal_die 2 "invalid account email"
  [ "$alias" != "$GCAL_ORIGINAL_ACCOUNT" ] || gcal_die 2 "account alias $GCAL_ORIGINAL_ACCOUNT is reserved for the original credential layout"
  [ "$client" != "$GCAL_ORIGINAL_ACCOUNT" ] || gcal_die 2 "client alias $GCAL_ORIGINAL_ACCOUNT is reserved for the original credential layout"
  gcal_client_field "$client" client_id >/dev/null || gcal_die 2 "OAuth client $client is not configured"
  if gcal_account_field "$alias" email; then old_email="$SECRET_VALUE"; had_email=1; fi
  if gcal_account_field "$alias" client; then old_client="$SECRET_VALUE"; had_client=1; fi
  gcal_secret_set "account__${alias}__email" "$email" || gcal_die 1 "could not store account email"
  if ! gcal_secret_set "account__${alias}__client" "$client"; then
    if [ "$had_email" -eq 1 ]; then gcal_secret_set "account__${alias}__email" "$old_email" || true; else gcal_secret_delete "account__${alias}__email" || true; fi
    if [ "$had_client" -eq 1 ]; then gcal_secret_set "account__${alias}__client" "$old_client" || true; fi
    gcal_die 1 "could not atomically store account; prior record was retained"
  fi
  printf 'stored account %s (%s) using client %s; no grant exists yet\n' "$alias" "$email" "$client"
  printf 'next: fclaw action auth -h --name gcal -- authorize %s\n' "$alias"
}

account_list() {
  local aliases alias email client default="" auth
  if gcal_secret_get default_account; then default="$SECRET_VALUE"; fi
  aliases="$(gcal_list_aliases account email)" || gcal_die 1 "could not list accounts"
  printf 'ACCOUNT\tEMAIL\tCLIENT\tDEFAULT\tAUTHORIZED\n'
  while IFS= read -r alias; do
    [ -n "$alias" ] || continue
    gcal_account_field "$alias" email || continue; email="$SECRET_VALUE"
    gcal_account_field "$alias" client || continue; client="$SECRET_VALUE"
    auth=no; if gcal_account_field "$alias" refresh_token; then auth=yes; fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$alias" "$email" "$client" "$([ "$alias" = "$default" ] && printf yes || printf no)" "$auth"
  done <<<"$aliases"
}

account_default() {
  [ "$#" -eq 1 ] || usage
  local alias="$1"; need_alias "$alias"
  gcal_account_field "$alias" email >/dev/null || gcal_die 2 "account $alias is not configured"
  gcal_secret_set default_account "$alias" || gcal_die 1 "could not store default account"
  printf 'default Google Calendar account is now %s\n' "$alias"
}

account_remove() {
  [ "$#" -eq 2 ] || usage
  local alias="$1" yes="$2" field
  need_alias "$alias"; confirm_remove "account $alias and its OAuth tokens" "$yes"
  [ "$alias" != "$GCAL_ORIGINAL_ACCOUNT" ] || gcal_die 2 "the original credential layout is managed with fclaw auth, not account remove"
  gcal_account_field "$alias" email >/dev/null || gcal_die 2 "account $alias is not configured"
  for field in email client refresh_token access_token access_token_expires_at scopes remote_state_name remote_state_json; do
    gcal_secret_delete "account__${alias}__${field}" || gcal_die 1 "could not remove account $alias"
  done
  if gcal_secret_get default_account && [ "$SECRET_VALUE" = "$alias" ]; then gcal_secret_delete default_account || true; fi
  printf 'removed account %s and its stored grant\n' "$alias"
}

persist_authorized_token() {
  local account="$1" client="$2" email="$3"
  gcal_sync_account "$account" "$client" "$email" ||
    gcal_die 1 "authorization succeeded in disposable gog state, but FloofClaw token persistence failed; prior durable grant was retained where possible"
  printf 'authorized account %s; OAuth tokens were stored only in FloofClaw\n' "$account"
  printf 'verify: fclaw action auth -h --name gcal -- status %s\n' "$account"
}

authorize_account() {
  [ "$#" -ge 1 ] || usage
  local account="$1" mode=browser step=0 force=0 redirect_stdin=0 arg
  shift
  while [ "$#" -gt 0 ]; do
    arg="$1"; shift
    case "$arg" in
      --manual) mode=manual ;;
      --remote) mode=remote ;;
      --step) [ "$#" -gt 0 ] || usage; step="$1"; shift ;;
      --redirect-url-stdin) redirect_stdin=1 ;;
      --force-consent) force=1 ;;
      *) usage ;;
    esac
  done
  need_alias "$account"
  gcal_account_field "$account" email || gcal_die 2 "account $account is not configured"; local email="$SECRET_VALUE"
  gcal_account_field "$account" client || gcal_die 2 "account $account has no client"; local client="$SECRET_VALUE"
  local version="$(gcal_version || true)"
  [ -n "$version" ] || gcal_die 1 "gog is not installed (minimum supported version $GCAL_GOG_MIN_VERSION)"
  gcal_version_compatible "$version" || gcal_die 1 "gog $GCAL_GOG_MIN_VERSION or newer is required; found $version"
  GCAL_AUTH_BRIDGE=1
  gcal_new_bridge "$client" || gcal_die 1 "could not prepare isolated gog auth state"
  local force_args=(); [ "$force" -eq 1 ] && force_args+=(--force-consent)
  if [ "$mode" = remote ]; then
    case "$step:$redirect_stdin" in
      1:0)
        gog --home "$GOG_HOME" --client "$client" --json auth add "$email" \
          --services calendar --remote --step 1 "${force_args[@]}"
        local state_file state_name state_json
        state_file="$(find "$GOG_HOME/config" -maxdepth 1 -type f -name 'oauth-manual-state-*.json' -print 2>/dev/null | head -n1)"
        [ -n "$state_file" ] && [ -f "$state_file" ] || gcal_die 1 "gog remote step 1 did not create PKCE state"
        state_name="$(basename "$state_file")"; state_json="$(cat "$state_file")"
        gcal_secret_set "account__${account}__remote_state_name" "$state_name" || gcal_die 1 "could not persist remote PKCE state"
        gcal_secret_set "account__${account}__remote_state_json" "$state_json" || { gcal_secret_delete "account__${account}__remote_state_name" || true; gcal_die 1 "could not persist remote PKCE state"; }
        printf 'next: paste the browser redirect URL through stdin within 10 minutes:\n'
        printf "  printf '%%s' '<redirect-url>' | fclaw action auth -h --name gcal -- authorize %s --remote --step 2 --redirect-url-stdin\n" "$account"
        return 0
        ;;
      2:1)
        gcal_account_field "$account" remote_state_name || gcal_die 2 "remote step 1 state is missing or expired"; local state_name="$SECRET_VALUE"
        gcal_account_field "$account" remote_state_json || gcal_die 2 "remote step 1 state is missing or expired"; local state_json="$SECRET_VALUE"
        [[ "$state_name" =~ ^oauth-manual-state-[A-Za-z0-9_-]+\.json$ ]] || gcal_die 1 "stored remote state name is invalid"
        mkdir -p "$GOG_HOME/config"; chmod 700 "$GOG_HOME/config"
        printf '%s' "$state_json" >"$GOG_HOME/config/$state_name"; chmod 600 "$GOG_HOME/config/$state_name"
        local redirect_url="" expected_state=""
        IFS= read -r redirect_url || true
        [ -n "$redirect_url" ] && [ "${#redirect_url}" -le 4096 ] && [[ "$redirect_url" =~ ^https?:// ]] || gcal_die 2 "stdin must contain the complete OAuth redirect URL"
        expected_state="$(jq -r '.state // empty' <<<"$state_json")"
        [[ "$expected_state" =~ ^[A-Za-z0-9_-]+$ ]] || gcal_die 1 "stored remote PKCE state is invalid"
        [[ "$redirect_url" =~ (^|[?\&])state=${expected_state}([\&#]|$) ]] ||
          gcal_die 2 "OAuth redirect state did not match remote step 1"
        # Manual mode reads the redirect URL from stdin, so the authorization
        # code never appears in this process's or gog's argv.
        printf '%s\n' "$redirect_url" | gog --home "$GOG_HOME" --client "$client" --json \
          auth add "$email" --services calendar --manual "${force_args[@]}"
        gcal_secret_delete "account__${account}__remote_state_name" || true
        gcal_secret_delete "account__${account}__remote_state_json" || true
        persist_authorized_token "$account" "$client" "$email"
        return 0
        ;;
      *) gcal_die 2 "remote authorization requires --step 1, or --step 2 with --redirect-url-stdin" ;;
    esac
  fi
  [ "$step" -eq 0 ] && [ "$redirect_stdin" -eq 0 ] || usage
  if [ "$mode" = manual ]; then
    gog --home "$GOG_HOME" --client "$client" --json auth add "$email" --services calendar --manual "${force_args[@]}"
  else
    gog --home "$GOG_HOME" --client "$client" --json auth add "$email" --services calendar "${force_args[@]}"
  fi
  persist_authorized_token "$account" "$client" "$email"
}

status_account() {
  local requested="${1-}" account version client email code=HEALTHY
  set +e; account="$(gcal_select_account "$requested")"; local select_rc=$?; set -e
  case "$select_rc" in 0) ;; 3) gcal_die 2 "account not found" ;; 5) gcal_die 2 "multiple accounts exist; name one or configure a default" ;; *) gcal_die 2 "invalid account selection" ;; esac
  gcal_account_field "$account" email; email="$SECRET_VALUE"
  gcal_account_field "$account" client; client="$SECRET_VALUE"
  version="$(gcal_version || true)"
  if [ -z "$version" ]; then code=GOG_MISSING
  elif ! gcal_version_compatible "$version"; then code=GOG_INCOMPATIBLE
  elif ! gcal_client_field "$client" client_id || ! gcal_client_field "$client" client_secret; then code=CLIENT_NOT_FOUND
  elif ! gcal_account_field "$account" refresh_token; then code=AUTH_MISSING
  else
    GCAL_AUTH_BRIDGE=1
    if ! gcal_prepare_account "$account"; then code="${GCAL_PREPARE_CODE:-GOG_PROTOCOL_ERROR}"
    else
      set +e; gcal_run_capture events primary --max 1 --results-only; local gog_rc=$?; set -e
      if [ "$gog_rc" -ne 0 ]; then
        gcal_sync_account "$account" "$GCAL_CLIENT" "$GCAL_EMAIL" || true
        code="$(gcal_error_code "$GCAL_GOG_STDERR" "$gog_rc")"
      elif ! gcal_sync_account "$account" "$GCAL_CLIENT" "$GCAL_EMAIL"; then code=GOG_PROTOCOL_ERROR
      fi
    fi
  fi
  printf 'account\t%s\nemail\t%s\nclient\t%s\ngog_version\t%s\nstatus\t%s\n' "$account" "$email" "$client" "${version:-missing}" "$code"
  if [ "$code" = REAUTHORIZATION_REQUIRED ]; then
    printf 'next\t%s\n' "$(gcal_reauth_command "$account")"
  fi
  [ "$code" = HEALTHY ]
}

migrate_legacy() {
  local client="" account="" email="" yes=0
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --client) client="${2-}"; shift 2 ;;
      --account) account="${2-}"; shift 2 ;;
      --email) email="${2-}"; shift 2 ;;
      --yes) yes=1; shift ;;
      *) usage ;;
    esac
  done
  need_alias "$client"; need_alias "$account"; gcal_email_valid "$email" || gcal_die 2 "invalid account email"
  [ "$client" != "$GCAL_ORIGINAL_ACCOUNT" ] && [ "$account" != "$GCAL_ORIGINAL_ACCOUNT" ] ||
    gcal_die 2 "the alias $GCAL_ORIGINAL_ACCOUNT is reserved for the original credential layout"
  [ "$yes" -eq 1 ] || gcal_die 2 "migration needs --yes before old keys may be deleted"
  gcal_secret_get client_id || gcal_die 2 "legacy client_id is missing"; local id="$SECRET_VALUE"
  gcal_secret_get client_secret || gcal_die 2 "legacy client_secret is missing"; local secret="$SECRET_VALUE"
  gcal_secret_get refresh_token || gcal_die 2 "legacy refresh_token is missing"; local refresh="$SECRET_VALUE"
  gcal_secret_set "client__${client}__client_id" "$id" || gcal_die 1 "migration write failed"
  gcal_secret_set "client__${client}__client_secret" "$secret" || gcal_die 1 "migration write failed; legacy keys retained"
  gcal_secret_set "account__${account}__email" "$email" || gcal_die 1 "migration write failed; legacy keys retained"
  gcal_secret_set "account__${account}__client" "$client" || gcal_die 1 "migration write failed; legacy keys retained"
  gcal_secret_set "account__${account}__refresh_token" "$refresh" || gcal_die 1 "migration write failed; legacy keys retained"
  gcal_secret_set "account__${account}__scopes" "$GCAL_SCOPE_JSON" || gcal_die 1 "migration write failed; legacy keys retained"
  if status_account "$account"; then
    gcal_secret_delete client_id || true; gcal_secret_delete client_secret || true; gcal_secret_delete refresh_token || true
    printf 'legacy credentials migrated and old keys removed after successful verification\n'
  else
    gcal_die 1 "migration verification failed; legacy keys were retained"
  fi
}

[ "$#" -gt 0 ] || usage
case "$1" in
  client)
    shift; [ "$#" -gt 0 ] || usage
    sub="$1"; shift
    case "$sub" in add) client_add "$@" ;; restore-original) client_restore_original "$@" ;; list) [ "$#" -eq 0 ] || usage; client_list ;; remove) client_remove "$@" ;; *) usage ;; esac
    ;;
  account)
    shift; [ "$#" -gt 0 ] || usage
    sub="$1"; shift
    case "$sub" in add) account_add "$@" ;; list) [ "$#" -eq 0 ] || usage; account_list ;; default) account_default "$@" ;; remove) account_remove "$@" ;; *) usage ;; esac
    ;;
  authorize) shift; authorize_account "$@" ;;
  reauthorize)
    shift
    [ "$#" -eq 2 ] && [ "$2" = --force-consent ] || usage
    authorize_account "$@"
    ;;
  status) shift; [ "$#" -le 1 ] || usage; status_account "${1-}" ;;
  migrate) shift; migrate_legacy "$@" ;;
  *) usage ;;
esac
