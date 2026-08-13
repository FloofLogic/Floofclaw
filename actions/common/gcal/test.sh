#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd -P)"
cd "$ROOT"
make -j4 bin/fclaw
exec ./tests/run_in_isolated_copy.sh bash ./tests/test_action_cli.sh
