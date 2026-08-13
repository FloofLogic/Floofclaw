#!/usr/bin/env bash
# rss_soak.sh — hourly RSS + disk sampler for the running gateway.
#
# Writes one JSON line per hour to workspace/logs/rss_soak.jsonl:
#   ts, pid, rss_kb, disk_bytes, runs_count, jsonl_lines[]
#
# Run this in a tmux/screen alongside the gateway for whatever ad-hoc
# observation window is useful. This sampler is not a release gate:
#
#   ./scripts/rss_soak.sh &
#
# Stop by killing the pid. If termination lands during the final append, ignore
# a partial last line when analyzing this ad-hoc diagnostic log.
#
# Interpretation guide (in docs/performance/rss_baseline.md):
#   - Judge sustained RSS growth across the chosen window after normal
#     startup and traffic bursts settle.
#   - runs_count currently grows because terminal-run pruning is blocked by a
#     known runstate field mismatch; use this metric to quantify that defect.
#   - Every jsonl in workspace/logs/ should rotate after reaching its
#     appliance.logs.targets.<name>.max_size_bytes and the janitor cadence.

set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$repo_root"

out="workspace/logs/rss_soak.jsonl"
mkdir -p "$(dirname "$out")"

sample_once() {
  local pid rss disk runs ts bus deliv narr usage
  ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  pid="$(cat .fclaw/run/gateway.pid 2>/dev/null || echo 0)"
  if [ "$pid" = "0" ] || ! kill -0 "$pid" 2>/dev/null; then
    printf '{"ts":"%s","pid":0,"rss_kb":0,"gateway":"not_running"}\n' "$ts" >>"$out"
    return
  fi
  # Portable RSS in kB (BSD ps -o rss= gives kB on both macOS and Linux).
  rss="$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')"
  [ -z "$rss" ] && rss=0
  disk="$(du -sk workspace 2>/dev/null | awk '{print $1 * 1024}')"
  runs="$(ls workspace/runs/ 2>/dev/null | wc -l | tr -d ' ')"
  bus="$(wc -l <workspace/logs/bus.jsonl 2>/dev/null | tr -d ' ' || echo 0)"
  deliv="$(wc -l <workspace/logs/deliveries.jsonl 2>/dev/null | tr -d ' ' || echo 0)"
  narr="$(wc -l <workspace/logs/narration.jsonl 2>/dev/null | tr -d ' ' || echo 0)"
  usage="$(wc -l <workspace/logs/llm_usage.jsonl 2>/dev/null | tr -d ' ' || echo 0)"
  printf '{"ts":"%s","pid":%s,"rss_kb":%s,"disk_bytes":%s,"runs_count":%s,"jsonl_lines":{"bus":%s,"deliveries":%s,"narration":%s,"llm_usage":%s}}\n' \
         "$ts" "$pid" "$rss" "$disk" "$runs" "$bus" "$deliv" "$narr" "$usage" >>"$out"
}

echo "rss_soak: sampling once per hour to $out (Ctrl-C to stop)"
sample_once   # initial sample so t=0 is recorded even if the loop is killed
while true; do
  sleep 3600
  sample_once
done
