#!/usr/bin/env bash
set -euo pipefail

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

cmp config/pricing.json app/tokenwatch/pricing.json

python3 app/tokenwatch/generate.py --repo-root . \
  --fixture tests/fixtures/tokenwatch/mixed.json \
  --recent 10 \
  --out "$tmp_dir/tokenwatch.json" >/dev/null

python3 - "$tmp_dir/tokenwatch.json" app/tokenwatch/pricing.json <<'PY'
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8"))
pricing = json.load(open(sys.argv[2], encoding="utf-8"))
agents = {item["id"]: item for item in data["agents"]}
assert set(agents) == {"chat_manager", "local_worker", "zero_call_agent"}
assert not [r for r in data["usage_records"] if r["agent"] == "zero_call_agent"]
assert any(r["provider_cached_input_tokens"] is None for r in data["usage_records"])
assert len(data["cacheview_records"]["chat_manager"]) == 3
assert data["cacheview_records"]["chat_manager"][1]["provider_cached_input_tokens"] == 8900
assert pricing["schema_version"] == 3
assert pricing["last_updated"] == "2026-09-02"
assert pricing["media_input_tokens_per_item"] == 8192
rows = {(item["provider"], item["model"]) for item in pricing["models"]}
assert len(rows) == len(pricing["models"])
assert ("gemini_key", "gemini-2.5-flash") in rows
assert ("anthropic", "claude-sonnet-4-6") in rows
assert ("anthropic_key", "claude-sonnet-4-6") in rows
assert ("anthropic", "claude-opus-5") in rows
assert ("anthropic_key", "claude-sonnet-5") in rows
anthropic_rows = [row for row in pricing["models"] if row["provider"].startswith("anthropic")]
assert all(row["cache_creation_input"] > row["input"] for row in anthropic_rows)
assert not any(model == "qwen-fixture" for _, model in rows)
external = [row for row in pricing["models"] if row["provider"] != "mock"]
assert all(row["source"].startswith("https://") for row in external)
assert all(row["source_label"] for row in pricing["models"])
assert all(row["checked_at"] for row in pricing["models"])
PY

if command -v node >/dev/null 2>&1; then
  node --check app/tokenwatch/app.js
fi

grep -q 'Pricing assumptions' app/tokenwatch/pricing.html
grep -q 'pricing-updated' app/tokenwatch/pricing.html
grep -q 'Last known pricing' app/tokenwatch/app.js
grep -q 'Summary ·' app/tokenwatch/app.js
grep -q 'id="summary-range"' app/tokenwatch/index.html
grep -q 'id="range-start"' app/tokenwatch/index.html
grep -q 'id="range-end"' app/tokenwatch/index.html
grep -q 'function setSummaryRange' app/tokenwatch/app.js
grep -q 'data-request-format="raw"' app/tokenwatch/index.html
grep -q 'red.*input' app/tokenwatch/app.js
grep -q 'purple.*output' app/tokenwatch/app.js

echo "tokenwatch fixture passed"
