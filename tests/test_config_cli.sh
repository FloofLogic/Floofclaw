#!/usr/bin/env bash
# fclaw config get/set/enable/disable — span-splice edits of the deployment
# config. The proof cares that every edit leaves valid JSON, preserves the
# bytes it did not target, and that enable/disable are idempotent facts.
set -euo pipefail

fail() {
  printf 'config cli test: %s\n' "$1" >&2
  exit 1
}

# This proof edits config/floofclaw_config.json in place, so it only runs
# inside the throwaway copy run_in_isolated_copy.sh provides.
if [[ "${FCLAW_TEST_ISOLATED:-0}" != "1" || ! -f .fclaw-test-isolated ]]; then
  printf '%s\n' 'config cli test: refusing to run outside an isolated copy' >&2
  printf '%s\n' '  fix: ./tests/run_in_isolated_copy.sh bash ./tests/test_config_cli.sh' >&2
  exit 2
fi

cfg=config/floofclaw_config.json

# The checked-in config differs per deployment branch (bot_name, port), so
# the expected values come from the file under test: this proves the CLI
# reads and prints what is there, not that the shipped values never moved.
cfg_value() {
  python3 -c 'import json,sys,functools
print(functools.reduce(lambda d, k: d[k], sys.argv[2].split("."), json.load(open(sys.argv[1]))))' "$cfg" "$1"
}
expect_name="$(cfg_value bot_name)"
expect_port="$(cfg_value gateway.port)"

# Edits must preserve keys the CLI does not know; the shipped file carries a
# _comment, a deployment branch may not. Plant one so every branch proves it.
grep -q '_comment' "$cfg" || python3 - "$cfg" <<'PY'
import json, sys
p = sys.argv[1]
cfg = json.load(open(p))
json.dump({"_comment": "planted by test_config_cli", **cfg}, open(p, "w"), indent=2)
PY

./bin/fclaw config get bot_name -h | grep -qxF "$expect_name" ||
  fail 'get bot_name did not print the checked-in value'
./bin/fclaw config get gateway.port -h | grep -qxF "$expect_port" ||
  fail 'get gateway.port did not print the checked-in number'
./bin/fclaw config get bot_name -a | grep -qxF "\"$expect_name\"" ||
  fail 'agent-mode get did not print the raw JSON value'

./bin/fclaw config set bot_name Melba -h >/dev/null ||
  fail 'set bot_name failed'
./bin/fclaw config get bot_name -h | grep -qx 'Melba' ||
  fail 'set bot_name did not round-trip'
./bin/fclaw config set gateway.port 41007 -h >/dev/null ||
  fail 'set gateway.port failed'
grep -q '"port": 41007' "$cfg" ||
  fail 'numeric value was not spliced as a JSON number'
sibling_before="$(./bin/fclaw config get channels.discord.enabled -a)"
./bin/fclaw config set channels.discord.guild_id '"123456789"' -h >/dev/null ||
  fail 'insertion of a new leaf into an existing object failed'
./bin/fclaw config get channels.discord.guild_id -a | grep -qx '"123456789"' ||
  fail 'pre-quoted value did not stay a JSON string'
# Whatever the checked-in value is (false shipped, true on a Discord bot's
# branch), the insertion next to it must leave it alone.
[[ "$(./bin/fclaw config get channels.discord.enabled -a)" == "$sibling_before" ]] ||
  fail 'sibling key was disturbed by the insertion'
grep -q '_comment' "$cfg" ||
  fail 'an edit dropped the _comment entry'

if ./bin/fclaw config set no.such.parent x -h 2>/dev/null; then
  fail 'set under a missing parent unexpectedly succeeded'
fi

# The list under test is planted, not the checked-in one: a deployment
# branch masks whatever that bot has not set up (Truly masks nothing), and
# the enable/disable mechanics below are what this proof is about.
python3 - "$cfg" <<'PY'
import json, sys
p = sys.argv[1]
cfg = json.load(open(p))
cfg["force_disable"] = ["web_read", "gcal", "git_github",
                        "manage_codex", "manage_claude", "manage_hermes"]
json.dump(cfg, open(p, "w"), indent=2)
PY
./bin/fclaw config get force_disable -a | grep -q 'web_read' ||
  fail 'planted force_disable list did not read back'
./bin/fclaw config enable web_read -h | grep -q 'removed from force_disable' ||
  fail 'enable did not report the removal'
./bin/fclaw config get force_disable -a | grep -q 'web_read' &&
  fail 'enable left web_read in force_disable'
./bin/fclaw config enable web_read -h | grep -q 'already enabled' ||
  fail 'second enable was not an idempotent fact'
./bin/fclaw config disable web_read -h | grep -q 'added to force_disable' ||
  fail 'disable did not report the addition'
./bin/fclaw config disable web_read -h | grep -q 'already in force_disable' ||
  fail 'second disable was not an idempotent fact'

# Remove every entry, then the last removal must leave a valid empty array.
for id in web_read gcal git_github manage_codex manage_claude manage_hermes; do
  ./bin/fclaw config enable "$id" >/dev/null || fail "enable $id failed"
done
./bin/fclaw config get force_disable -a | grep -qx '\[\]' ||
  fail 'emptying force_disable did not leave []'
./bin/fclaw config disable web_read >/dev/null ||
  fail 'disable into an empty list failed'
./bin/fclaw config get force_disable -a | grep -q 'web_read' ||
  fail 'disable into an empty list did not add the id'

# The registry consumes the edited file: with only web_read disabled, the
# operator listing must mark exactly that entry.
mkdir -p workspace
./bin/fclaw action list -a > workspace/config-cli-catalog.json 2>/dev/null ||
  fail 'action list failed against the edited config'
grep -q '"force_disabled":true' workspace/config-cli-catalog.json ||
  fail 'edited mask did not reach the operator listing'

printf 'config cli test passed\n'
