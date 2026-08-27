#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$root"
bash -n scripts/build_binary_archive.sh
# The public tree never carries GitHub Actions. The private release gate owns
# the separate assertion that its non-shipping manifest declares this path.
[ ! -e .github/workflows ]
printf 'release packaging contract: PASS\n'
