#!/usr/bin/env bash
# Channels are chosen at build time. The binary must contain exactly what
# was asked for, switching the selection must actually relink (timestamps
# alone do not catch it), and a config naming a channel this binary lacks
# must refuse to start rather than look healthy and answer nothing.
set -euo pipefail
# These are deliberate default and selected builds in a throwaway copy.
# Invoked from `make FCLAW_ADAPTERS="ws discord" test`, the parent make
# would hand that selection to every inner make through MAKEFLAGS and the
# "default build" below would not be one. Sever them from the parent.
unset MAKEFLAGS MFLAGS

fail() { printf 'adapter selection: %s\n' "$1" >&2; exit 1; }

ids() { ./bin/fclaw adapters -a | tr ',' '\n' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p' | tr '\n' ' '; }

# Default is ws only: `fclaw -i` and the local client API ride on it.
make -j4 >/dev/null 2>&1 || fail "default build failed"
[ "$(ids)" = "ws " ] || fail "default build is not ws only: $(ids)"

# An explicit set, and ws is always present even when not named.
make -j4 FCLAW_ADAPTERS="telegram" >/dev/null 2>&1 || fail "telegram build failed"
[ "$(ids)" = "telegram ws " ] || fail "telegram build wrong: $(ids)"

# all takes every directory.
make -j4 FCLAW_ADAPTERS=all >/dev/null 2>&1 || fail "all build failed"
[ "$(ids)" = "discord irc telegram ws " ] || fail "all build wrong: $(ids)"

# Narrowing again must relink. This is the case timestamps miss: the
# registry is usually rewritten in the same second as the previous link.
make -j4 >/dev/null 2>&1 || fail "narrowing rebuild failed"
[ "$(ids)" = "ws " ] || fail "narrowing did not relink: $(ids)"

# A name that is not an adapter directory is refused, with the list.
if make -n FCLAW_ADAPTERS="ws nosuch" >/dev/null 2>/tmp/fclaw_sel_err.$$; then
  fail "an unknown adapter name was accepted"
fi
grep -q "no such adapter: nosuch" /tmp/fclaw_sel_err.$$ ||
  fail "the refusal does not name the bad adapter"
grep -q "available:" /tmp/fclaw_sel_err.$$ ||
  fail "the refusal does not list the available adapters"
rm -f /tmp/fclaw_sel_err.$$

# Enabling a channel this binary lacks refuses startup and names the fix.
mkdir -p workspace/logs
printf '%s\n' '{"bot_name":"F","default_floop":"hello","gateway":{"host":"127.0.0.1","port":41993},"channels":{"telegram":{"enabled":true,"allow_dms":true}}}' \
  > config/floofclaw_config.json
if ./bin/fclaw gateway run -a --floop hello >/tmp/fclaw_sel_gw.$$ 2>&1; then
  fail "the gateway started with a channel it does not contain"
fi
grep -q "built without the telegram adapter" /tmp/fclaw_sel_gw.$$ ||
  fail "startup refusal does not name the missing adapter"
grep -q "FCLAW_ADAPTERS" /tmp/fclaw_sel_gw.$$ ||
  fail "startup refusal does not name the rebuild"
rm -f /tmp/fclaw_sel_gw.$$

# A channel that IS built in still starts normally.
make -j4 FCLAW_ADAPTERS=all >/dev/null 2>&1 || fail "rebuild with all failed"
printf '%s\n' '{"bot_name":"F","default_floop":"hello","gateway":{"host":"127.0.0.1","port":41993},"channels":{"telegram":{"enabled":false}}}' \
  > config/floofclaw_config.json
./bin/fclaw run -a --text "hello" >/dev/null 2>&1 || fail "a built-in selection broke ordinary runs"

printf 'adapter selection: PASS\n'
