#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

seconds="${FUZZ_SECONDS:-3600}"
max_len="${FUZZ_MAX_LEN:-65536}"
work="$root/tmp/fuzz"
rm -rf "$work"
mkdir -p "$work"

pids=()
names=(json fjson_repair event_reducer)
bins=(bin/fuzz_json bin/fuzz_fjson_repair bin/fuzz_event_reducer)

cleanup() {
  for pid in "${pids[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
}
trap cleanup INT TERM EXIT

for i in "${!names[@]}"; do
  name="${names[$i]}"
  mkdir -p "$work/$name/corpus" "$work/$name/artifacts"
  "${bins[$i]}" "$work/$name/corpus" "tests/fuzz/corpus/$name" \
    -max_total_time="$seconds" -max_len="$max_len" \
    -timeout=10 -rss_limit_mb=2048 \
    -artifact_prefix="$work/$name/artifacts/" \
    >"$work/$name/fuzzer.log" 2>&1 &
  pids+=("$!")
done

status=0
for i in "${!pids[@]}"; do
  if ! wait "${pids[$i]}"; then
    status=1
  fi
done
trap - INT TERM EXIT

for name in "${names[@]}"; do
  printf '\n== %s ==\n' "$name"
  tail -n 8 "$work/$name/fuzzer.log"
done

exit "$status"
