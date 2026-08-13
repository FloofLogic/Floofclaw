#!/usr/bin/env bash
set -euo pipefail

# The cap driver terminates the gateway as soon as all RtRun slots are full,
# so only this first child exists. Replace the shell with sleep so the hold is
# one process, not a shell-plus-child pair, while the durable inbox fills.
exec /bin/sleep "${THRASH_CAP_HOLD_SECONDS:-60}"
