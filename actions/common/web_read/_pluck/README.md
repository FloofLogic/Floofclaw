# Pluck — Zig CLI to extract readable content from the web

`pluck` is a fast Zig CLI that extracts the main readable text from a URL, local HTML file, or stdin. It uses `libxml2` for broken-HTML recovery and a Readability-inspired scoring pipeline implemented in Zig.

## Requirements

- Zig `0.15.2`
- `curl` for URL fetching
- `libxml2` headers and runtime
- Optional for `--render`: Node.js plus `playwright`

On macOS, run:

```sh
./scripts/bootstrap-macos.sh
```

## Build

```sh
zig build
zig build test
```

For an optimized binary:

```sh
zig build -Doptimize=ReleaseSafe
./zig-out/bin/pluck --help
```

## Usage

```sh
pluck https://example.com/post
pluck --file testdata/sample_article.html
cat page.html | pluck --stdin
pluck --json https://example.com/post
pluck --render https://example.com/post
```

Flags:

- `--json`: emit structured JSON
- `--render`: enable the optional Playwright sidecar if raw extraction looks poor
- `--timeout <ms>`: network and renderer timeout budget
- `--max-bytes <n>`: cap fetched/read HTML size
- `--user-agent <value>`: override the default user agent

## Output

Default output is markdown-ish readable text. `--json` emits:

- `title`
- `byline`
- `site_name`
- `url`
- `excerpt`
- `text_markdown`
- `text_plain`
- `length`
- `rendered`

## Render Sidecar

`--render` does not launch a browser unless the first raw extraction pass looks weak. If you want that path available:

```sh
npm install playwright
```

The sidecar script lives at [`scripts/render_page.mjs`](scripts/render_page.mjs).

## Benchmarks

There is a small benchmark helper at
[`scripts/benchmark.sh`](scripts/benchmark.sh) that compares `pluck` against an
optional Node baseline using Mozilla Readability.

## Current Limits

- JS-heavy pages need `--render`
- The extractor aims for behaviorally similar output, not Firefox parity
- Very short pages and app-like layouts can still fail the article heuristic

## Releases

The public repository is intentionally a release-only history: one snapshot
commit and one annotated `vX.Y.Z` tag per version. Commit messages describe
user-visible features and fixes rather than the private development process.
The public tree is complete source code and can be built normally.

Release mechanics and the private/public history boundary are documented in
[`RELEASING.md`](RELEASING.md).

## License

Pluck is available under the [MIT License](LICENSE).
