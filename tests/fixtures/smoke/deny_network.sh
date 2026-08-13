#!/usr/bin/env bash
set -euo pipefail

marker="${FCLAW_SMOKE_NETWORK_ATTEMPT_FILE:?missing network-attempt marker path}"
: > "$marker"
printf 'network command blocked by hermetic smoke: %s\n' "$0" >&2
exit 97
