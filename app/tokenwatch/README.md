# Tokenwatch

Tokenwatch is the static usage, cache, and estimated-cost view for the active
FloofClaw floop. It shows every configured LLM agent, including agents with no
calls in the current usage-log segment.

The generator reads the existing `workspace/logs/llm_usage.jsonl` segment and
numbered `workspace/runs/*/provider_calls/` artifacts. It writes only
`tokenwatch.json`; it does not create a runtime cache or index. The browser owns
price lookup, cost math, graph series, stable-prefix estimates, and heatmap
buckets. Provider-reported cache tokens remain distinct from FloofClaw's
byte-prefix observation.

Generate and serve from the repository root:

```bash
python3 app/tokenwatch/generate.py --repo-root .
cd app/tokenwatch && python3 -m http.server 8004
```

Then open `http://127.0.0.1:8004/`. `scripts/up.sh` starts the server and
refreshes the artifact automatically.

Pricing lives in `pricing.json` and is an app-owned estimate in USD per one
million tokens. `pricing.html` lists each provider/model pair as a separate
line with the exact input, cached-input, and output assumptions plus the date
and outbound source for its last known pricing. The document-level
`last_updated` date is shown in the page header; each row keeps its independent
source-check date. Unlisted provider/model pairs remain unpriced. The recorded
provider cache count is used for discounted input
only when it is available; missing cache evidence is displayed as not reported,
never as zero. Provider cache percentages use only calls that included a cache
count and show that reporting coverage separately.

The request selector opens on `Summary`, which mirrors the aggregate human
`cacheview` across the latest ten text-only requests. Summary shows the
whole-request heatmap and the complete baseline request. Raw preserves the
recorded bytes; Pretty recursively decodes JSON-shaped string fields and real
newlines. Both mark the exact shared byte boundary, highlight the cached
beginning, and dim the remainder. Selecting one request switches the inspector
to that request's divergence.

Summary also has Beginning and End sliders over the currently loaded request
list. They default to the full `0 → N` window and filter the agent detail
metrics, token and cost charts, heatmap, and request inspector in the browser.
The app shows the actual run/call IDs at both ends of the window; the streamed
artifact and inspection APIs stay unchanged.

For deterministic UI work:

```bash
python3 app/tokenwatch/generate.py --repo-root . \
  --fixture tests/fixtures/tokenwatch/mixed.json
```

The runtime also exposes authenticated file-backed NDJSON streams at
`/v1/usage/records` and `/v1/cacheview/records?agent=<id>&n=<1..10>` for local
clients that need the same raw evidence without a static artifact.
