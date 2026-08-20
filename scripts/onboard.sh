#!/usr/bin/env bash
# FloofClaw onboarding — interactive first-run (and re-run) wizard.
#
# Orchestrates the existing CLI verbs; it owns no state of its own. Every
# answer lands in config/floofclaw_config.json through `fclaw config`, every
# credential through `fclaw auth set-stdin`, every action verification
# through `fclaw action exec`. Re-running is safe: current values are the
# defaults, and nothing is reset.
#
# Dependencies: bash and ./bin/fclaw. Nothing else.
set -euo pipefail

cd "$(dirname "$0")/.."
FCLAW=./bin/fclaw

# --- presentation ------------------------------------------------------
# Colors and the spinner only when stdout is an interactive terminal;
# piped/scripted runs get plain deterministic text. ONBOARD_FORCE_COLOR=1
# is the test hook that exercises the interactive path through a pipe.
if [ "${ONBOARD_FORCE_COLOR:-0}" = 1 ] ||
   { [ -t 1 ] && [ "${TERM:-dumb}" != dumb ] && [ -z "${NO_COLOR:-}" ]; }; then
  TTY=1
  CYA=$'\033[38;5;87m'  MAG=$'\033[38;5;213m' GRN=$'\033[92m'
  YEL=$'\033[93m'       RED=$'\033[91m'       DIM=$'\033[2m'
  BLD=$'\033[1m'        RST=$'\033[0m'
else
  TTY=0
  CYA='' MAG='' GRN='' YEL='' RED='' DIM='' BLD='' RST=''
fi
# Glyphs need a UTF-8 locale and a terminal font that covers them; the
# Linux virtual console (TERM=linux) holds a few hundred glyphs and a
# C/POSIX locale mangles multibyte output entirely. ASCII speaks everywhere.
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
  *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) UNI=1 ;;
  *) UNI=0 ;;
esac
[ "${TERM:-}" = linux ] && UNI=0
if [ "$UNI" = 1 ]; then
  OK="${GRN}✓${RST}"; BAD="${RED}✗${RST}"; MASKED="${YEL}○${RST}"
  ARROW='▸'; RULE='─'; DASH='—'
  SPIN_FRAMES=('⠋' '⠙' '⠹' '⠸' '⠼' '⠴' '⠦' '⠧' '⠇' '⠏')
else
  OK="${GRN}[ok]${RST}"; BAD="${RED}[x]${RST}"; MASKED="${YEL}[  ]${RST}"
  ARROW='>'; RULE='-'; DASH='--'
  SPIN_FRAMES=('-' '\' '|' '/')
fi

say()  { printf '%s\n' "$*"; }
note() { printf '  %s%s%s\n' "$DIM" "$*" "$RST"; }
good() { printf '  %s %s\n' "$OK" "$*"; }
bad()  { printf '  %s %s\n' "$BAD" "$*"; }
hdr()  {
  printf '\n%s[%s]%s %s%s%s\n' "$BLD$CYA" "$1" "$RST" "$BLD" "$2" "$RST"
  printf '%s' "$DIM"; printf -- "$RULE%.0s" {1..46}; printf '%s\n' "$RST"
}
die()  { printf '%sonboard:%s %s\n' "$RED" "$RST" "$*" >&2; exit 1; }

# Deliberately flat two-color, NOT the binary's 24-color gradient
# (logo.c): the operator chose this so the wizard reads as its own room —
# you can tell at a glance you're in setup, not the gateway.
banner() {
  local claw=(
    '⠀⠀⠀⠔⠢⠀⡄⠢⡀⠀⠀'
    '⡠⢄⡰⠀⠀⣦⠁⠀⡇⡠⢄'
    '⢇⠀⠱⠣⡴⠥⢦⡔⡎⠀⢸'
    '⠈⠒⡸⠎⠀⠀⠀⠱⢕⠐⠁'
    '⠀⠠⡀⠀⠀⠀⠀⠀⢀⠆⠀'
    '⠀⠀⠑⠠⠔⠒⠢⠤⠊⠀⠀'
  )
  local word=(
    ''
    "${MAG}▄▖▜     ▐▘  ▜      ${RST}"
    "${MAG}▙▖▐ ▛▌▛▌▜▘▛▘▐ ▀▌▌▌▌${RST}"
    "${MAG}▌ ▐▖▙▌▙▌▐ ▙▖▐▖█▌▚▚▘${RST}"
    ''
    "${DIM}tiny code. sharp claws.${RST}"
  )
  echo
  if [ "$UNI" = 1 ]; then
    for i in 0 1 2 3 4 5; do
      printf '  %s%s%s  %s\n' "$CYA" "${claw[i]}" "$RST" "${word[i]}"
      [ "$TTY" = 1 ] && sleep 0.06
    done
    echo
  fi
  printf '  %sFloofClaw onboarding%s %s one floofclaw, one directory.\n' "$BLD" "$RST" "$DASH"
  note "Re-running is safe: existing state is kept unless you change it."
}

SPIN_LOG="${TMPDIR:-/tmp}/fclaw-onboard-$$.log"
trap 'rm -f "$SPIN_LOG"' EXIT

# spin "label" cmd...  — run with a live spinner + elapsed time; the
# command's output goes to SPIN_LOG and the last lines surface on failure.
spin() {
  local label="$1"; shift
  if [ "$TTY" = 0 ]; then
    printf '  ... %s\n' "$label"
    if "$@" >"$SPIN_LOG" 2>&1; then good "$label"; return 0; fi
    bad "$label"; sed 's/^/      /' <(tail -n 10 "$SPIN_LOG"); return 1
  fi
  "$@" >"$SPIN_LOG" 2>&1 &
  local pid=$! t0=$SECONDS i=0 rc cols avail last
  local nframes=${#SPIN_FRAMES[@]}
  cols="$( (tput cols) 2>/dev/null || echo 100)"
  trap 'kill "$pid" 2>/dev/null' INT
  while kill -0 "$pid" 2>/dev/null; do
    # Live ticker: the command's own latest line streams past the label.
    last="$(tail -n 1 "$SPIN_LOG" 2>/dev/null |
            sed "s/$(printf '\033')\[[0-9;]*m//g" | tr -d '\r')"
    avail=$((cols - ${#label} - 14))
    [ "$avail" -lt 8 ] && last='' || last="${last:0:avail}"
    printf '\r\033[2K  %s%s%s %s %s(%ss)%s %s%s%s' \
      "$CYA" "${SPIN_FRAMES[i % nframes]}" "$RST" "$label" \
      "$DIM" "$((SECONDS - t0))" "$RST" "$DIM" "$last" "$RST"
    i=$((i + 1)); sleep 0.1
  done
  trap - INT
  rc=0; wait "$pid" || rc=$?
  printf '\r\033[2K'
  if [ "$rc" -eq 0 ]; then
    printf '  %s %s %s(%ss)%s\n' "$OK" "$label" "$DIM" "$((SECONDS - t0))" "$RST"
  else
    printf '  %s %s %s(%ss)%s\n' "$BAD" "$label" "$DIM" "$((SECONDS - t0))" "$RST"
    sed 's/^/      /' <(tail -n 10 "$SPIN_LOG")
  fi
  return "$rc"
}

# ask VAR "prompt" "default"
ask() {
  local __var="$1" __prompt="$2" __default="${3:-}" __answer
  if [ -n "$__default" ]; then
    read -r -p "  ${CYA}${ARROW}${RST} $__prompt ${DIM}[$__default]${RST}: " __answer
    printf -v "$__var" '%s' "${__answer:-$__default}"
  else
    read -r -p "  ${CYA}${ARROW}${RST} $__prompt: " __answer
    printf -v "$__var" '%s' "$__answer"
  fi
}

# yesno "prompt" default(y|n) -> return 0 for yes
yesno() {
  local prompt="$1" default="${2:-n}" answer suffix
  [ "$default" = y ] && suffix="${DIM}[Y/n]${RST}" || suffix="${DIM}[y/N]${RST}"
  read -r -p "  ${CYA}${ARROW}${RST} $prompt $suffix " answer
  answer="${answer:-$default}"
  case "$answer" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

cfg_get() { "$FCLAW" config get -h "$1" 2>/dev/null || true; }
cfg_set() {  # key value — show the runtime's own factual ack
  local out
  out="$("$FCLAW" config set -h "$1" "$2")" || die "config set $1 failed"
  note "$out"
}

main() {
[ -x "$FCLAW" ] || die "bin/fclaw is not built; fix: make"
[ -f config/floofclaw_config.json ] ||
  die "config/floofclaw_config.json is missing; run from a floofclaw checkout or release"

banner

# ---------------------------------------------------------------- 1. build
hdr 1/7 "Build"
good "bin/fclaw $("$FCLAW" --version -a 2>/dev/null | head -c 60 || echo '?')"
note "releases ship proven; run make test yourself only when hacking on the engine"

# ------------------------------------------------------------- 2. identity
hdr 2/7 "Identity"
ask bot_name "bot_name" "$(cfg_get bot_name)"
cfg_set bot_name "$bot_name"
ask port "gateway port" "$(cfg_get gateway.port)"
cfg_set gateway.port "$port"

# ------------------------------------------------------------- 3. provider
hdr 3/7 "Model provider"
existing="$(ls secrets 2>/dev/null | sed -n 's/^endpoint://p' | tr '\n' ' ' || true)"
[ -n "$existing" ] && good "stored endpoint secrets: $existing"
say "    1) gemini_key   2) openai_key   3) openrouter_key   4) anthropic_key"
say "    5) keep what is stored / mock (offline demo $DASH no key needed)"
ask provider_choice "choice" "5"
endpoint=""
case "$provider_choice" in
  1) endpoint=gemini_key ;;  2) endpoint=openai_key ;;
  3) endpoint=openrouter_key ;;  4) endpoint=anthropic_key ;;
esac
if [ -n "$endpoint" ]; then
  read -rs -p "  ${CYA}${ARROW}${RST} paste the $endpoint value (input hidden): " secret; echo
  [ -n "$secret" ] || die "empty key"
  printf %s "$secret" | "$FCLAW" auth set-stdin "$endpoint" >/dev/null
  unset secret
  good "stored secrets/endpoint:$endpoint"
  note "verified live by the health probe in step 7"
fi

# ---------------------------------------------------------------- 4. floop
hdr 4/7 "Floop"
note "available: $(ls floops 2>/dev/null | tr '\n' ' ')"
ask floop "default_floop" "$(cfg_get default_floop)"
[ -d "floops/$floop" ] || die "floops/$floop does not exist"
cfg_set default_floop "$floop"

# ------------------------------------------------------------- 5. channels
hdr 5/7 "Channels"
if yesno "enable discord?" n; then
  read -rs -p "  ${CYA}${ARROW}${RST} discord bot token (input hidden): " token; echo
  [ -n "$token" ] || die "empty token"
  printf %s "$token" | "$FCLAW" auth set-stdin discord_token >/dev/null
  unset token
  good "stored secrets/endpoint:discord_token"
  ask guild "guild_id" "$(cfg_get channels.discord.guild_id)"
  ask home "home_channel_id" "$(cfg_get channels.discord.home_channel_id)"
  # Discord snowflakes are strings; pre-quote so they never become numbers.
  cfg_set channels.discord.guild_id "\"$guild\""
  cfg_set channels.discord.home_channel_id "\"$home\""
  cfg_set channels.discord.token_key discord_token
  cfg_set channels.discord.enabled true
  note "READY is proven in step 7"
else
  note "channels stay as configured; see config/floofclaw_config.example.json"
fi

# -------------------------------------------------------------- 6. actions
hdr 6/7 "Actions $DASH the force_disable walk"
note "masked actions need operator setup; each is enabled only after its"
note "own verification. See actions/common/<id>/SETUP_*.md for details."
masked="$("$FCLAW" config get -a force_disable 2>/dev/null |
          grep -o '"[a-z_]*"' | tr -d '"' || true)"
[ -n "$masked" ] || good "force_disable is empty $DASH nothing masked"

# `fclaw action exec` is a transport: it publishes onto the bus and waits
# for a runtime owner to execute the action. Without a running gateway it
# waits out a 3-minute timeout looking hung — so the walk keeps one up.
gateway_started_for_walk=0
ensure_gateway() {
  "$FCLAW" gateway status >/dev/null 2>&1 && return 0
  note "starting the gateway so verifications can execute"
  if [ "$UNI" = 1 ]; then
    "$FCLAW" gateway start -h || die "gateway did not start"
  else
    # The binary's startup logo is unicode art; keep a C-locale/VC screen clean.
    "$FCLAW" gateway start -a >/dev/null || die "gateway did not start"
    good "gateway started"
  fi
  gateway_started_for_walk=1
  sleep 1
}

# Prompt-free variant: when the prerequisite is already satisfied, the
# proof is cheap — verify and enable without asking.
auto_verify_enable() {  # id args
  local id="$1" args="$2" excerpt
  ensure_gateway
  if spin "live $id call through the bus" \
      "$FCLAW" action exec -a --name "$id" --args "$args"; then
    excerpt="$(grep -o '"title":"[^"]*"' "$SPIN_LOG" | head -1 | cut -d'"' -f4 || true)"
    [ -n "$excerpt" ] && note "fetched: $excerpt"
    note "$("$FCLAW" config enable -h "$id")"
  else
    bad "$id verification failed $DASH leaving it masked"
  fi
}

verify_and_enable() {  # id args
  local id="$1" args="$2" excerpt
  if yesno "verify $id with a live call now?" y; then
    ensure_gateway
    if spin "live $id call through the bus" \
        "$FCLAW" action exec -a --name "$id" --args "$args"; then
      excerpt="$(grep -o '"title":"[^"]*"' "$SPIN_LOG" | head -1 | cut -d'"' -f4 || true)"
      [ -n "$excerpt" ] && note "fetched: $excerpt"
    else
      bad "$id verification failed $DASH leaving it masked"
      return
    fi
  fi
  if yesno "enable $id (remove from force_disable)?" y; then
    note "$("$FCLAW" config enable -h "$id")"
  fi
}

enable_after_check() {  # id "ready message"
  good "$2"
  if yesno "enable $1?" y; then
    note "$("$FCLAW" config enable -h "$1")"
  fi
}

for id in $masked; do
  case "$id" in
    web_read)
      if [ -x actions/common/web_read/_pluck/zig-out/bin/pluck ]; then
        good "web_read: pluck is built"
        auto_verify_enable web_read '{"op":"start","url":"https://example.com/"}'
      elif command -v zig >/dev/null 2>&1; then
        printf '  %s web_read: pluck not built; zig %s found\n' \
          "$MASKED" "$(zig version)"
        if yesno "build pluck now?" y &&
           spin "zig build pluck (ReleaseFast)" \
             bash actions/common/web_read/build.sh; then
          verify_and_enable web_read '{"op":"start","url":"https://example.com/"}'
        fi
      else
        printf '  %s web_read: needs Zig 0.15.2 + libxml2, then: bash actions/common/web_read/build.sh\n' "$MASKED"
      fi ;;
    gcal)
      printf '  %s gcal: needs Google OAuth credentials %s actions/common/gcal/SETUP_GCAL.md\n' "$MASKED" "$DASH"
      note "      (fclaw action auth --name gcal -- ... ; re-run me after)" ;;
    git_github)
      if command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1; then
        enable_after_check git_github "git_github: gh is installed and authenticated"
      else
        printf '  %s git_github: needs the gh CLI authenticated (gh auth login)\n' "$MASKED"
      fi ;;
    manage_codex)
      if command -v codex >/dev/null 2>&1; then
        enable_after_check manage_codex "manage_codex: codex CLI found"
      else
        printf '  %s manage_codex: codex CLI not on PATH\n' "$MASKED"
      fi ;;
    manage_claude)
      if command -v claude >/dev/null 2>&1; then
        enable_after_check manage_claude "manage_claude: claude CLI found"
      else
        printf '  %s manage_claude: claude CLI not on PATH\n' "$MASKED"
      fi ;;
    manage_hermes)
      if [ -n "${HERMES_API_URL:-}" ]; then
        enable_after_check manage_hermes "manage_hermes: HERMES_API_URL is set"
      else
        printf '  %s manage_hermes: HERMES_API_URL is not set\n' "$MASKED"
      fi ;;
    *)
      printf '  %s %s: no automated check; enable with: fclaw config enable %s\n' \
        "$MASKED" "$id" "$id" ;;
  esac
done

# ---------------------------------------------------------------- 7. health
hdr 7/7 "Health"
if yesno "start the gateway and run one probe?" y; then
  # The gateway reads force_disable and channel config at startup, so a
  # restart here makes it load exactly what the walk just decided.
  if [ "$gateway_started_for_walk" = 1 ] ||
     "$FCLAW" gateway status >/dev/null 2>&1; then
    note "restarting the gateway to load the walk's decisions"
    "$FCLAW" gateway stop -h >/dev/null 2>&1 || true
  fi
  if [ "$UNI" = 1 ]; then
    "$FCLAW" gateway start -h || die "gateway did not start"
  else
    "$FCLAW" gateway start -a >/dev/null || die "gateway did not start"
    good "gateway started"
  fi
  sleep 2
  "$FCLAW" gateway status -h | head -2 || true
  # Only this boot's mask lines — the log is cumulative across restarts.
  { awk '/gateway: started loop=/{m=""} /force_disabled/{m=m $0 "\n"} END{printf "%s", m}' \
      workspace/logs/gateway.log 2>/dev/null || true; } |
    while read -r line; do note "$line"; done
  if spin "probe: one-shot run through floop '$floop'" \
      "$FCLAW" run -a --text "hello"; then
    note "$(head -c 100 "$SPIN_LOG")"
  else
    bad "probe failed $DASH check workspace/logs/gateway.log and provider secrets"
  fi
  yesno "leave the gateway running?" y || "$FCLAW" gateway stop -h
elif [ "$gateway_started_for_walk" = 1 ]; then
  "$FCLAW" gateway stop -h >/dev/null 2>&1 || true
  note "stopped the gateway the action walk started"
fi

echo
printf '  %sDone.%s Re-run %sscripts/onboard.sh%s anytime %s it verifies and repairs.\n' \
  "$BLD$GRN" "$RST" "$CYA" "$RST" "$DASH"
note "reference card: ./bin/fclaw setup -h"
printf '  %stiny code. sharp claws.%s\n\n' "$MAG" "$RST"
}

main "$@"
