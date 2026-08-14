# Installation and Build

**Preconditions:** run these commands from the repository root on macOS or
Linux with a C11 compiler and `make` installed. No account, API key, or network
access is needed for the build or hermetic tests.

## Requirements

- a C11 toolchain compatible with the project `Makefile` (Apple clang or gcc)
- `make`
- a POSIX userland; the build targets macOS and Linux

Needed for real providers and network channels:

- `OpenSSL 3` — enables nonblocking TLS in `runtime/support/net_tls.c` and
  the WebSocket SHA-1 handshake. The Makefile autodetects a Homebrew install
  via `brew --prefix openssl@3`; elsewhere pass the installation prefix as
  `OPENSSL_PREFIX=/path/to/openssl` when invoking `make`.
- `libcurl` — enables every current non-mock LLM transport (`http`,
  `openai_compat`, `openai_responses`, and `gemini_key`), including local
  loopback OpenAI-compatible endpoints. The Makefile discovers it through
  `curl-config`; mock-provider execution does not use the library.

TLS-backed channels refuse startup with a fix-naming error when OpenSSL support
is absent. A build without libcurl warns that real LLM HTTP calls are
unavailable; selecting one later fails at the provider call. Neither library
is needed for the mock-backed first run.

## Bundled actions

FloofClaw ships actions only under `actions/common/` and `actions/core/`.
Other action areas are local installation surface and are ignored by Git.
Public source releases contain `web_read`'s pinned `pluck` source as ordinary
files, so a single clone is complete and no private repository access is
required. The extractor source is also available at
[FloofLogic/pluck](https://github.com/FloofLogic/pluck).

The `web_read` action needs its `pluck` helper built before first use. Install
Zig 0.15.2 and libxml2, then run
`bash actions/common/web_read/build.sh` from the repository root. Until then,
`web_read` returns a `PLUCK_MISSING` error naming that command. This helper is
not needed to compile `bin/fclaw`, run the mock-backed first floop, or run the
routine suite.

A private development clone, rather than a flattened public source release,
first initializes that one public dependency:

```bash
git submodule update --init actions/common/web_read/_pluck
```

## Build

```bash
make -j4
```

Produces the `bin/fclaw` runtime. Test and robustness targets may also place
harness binaries under `bin/`.

## Verify

```bash
./bin/fclaw --version -h
```

Expected output (current branch):

```text
fclaw 0.25.0
```

Then run the routine suite:

```bash
make test
```

It currently runs 32 unit cases, 139 integration cases, auxiliary contract
probes, and the end-to-end smoke:

- `tests/smoke_lookup_and_poem.sh` — runs a mock-backed lookup-and-poem prompt
  through `openclaw` and a mock-backed delegated Frogger app prompt through
  `floofclaw`. The first path reads a committed local `example.com` fixture
  and blocks accidental `curl` use.

If `make test` is green, the build is good.

`make test` uses isolated temporary copies and mock model responses. It is
offline and does not call a live provider. The opt-in live smoke defaults to
the `responses` profile, which the committed configuration currently maps to
Gemini. Store matching authentication, then run:

```bash
FCLAW_LIVE_SMOKE=1 make live-smoke
```

To exercise another configured profile, select it explicitly:

```bash
FCLAW_LIVE_SMOKE=1 FCLAW_LIVE_PROFILE_REF=my_profile make live-smoke
```
