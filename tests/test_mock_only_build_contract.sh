#!/usr/bin/env bash
# The mock-only build is advertised in README.md, the constitution, and
# `make metrics` ("mock-only build can omit both"). It stopped compiling —
# a helper the load path calls unconditionally had drifted inside the
# libcurl #ifdef — and nothing noticed, because every gate builds with both
# libraries present. This compiles and RUNS the advertised build.
set -euo pipefail

fail() { printf 'mock-only build contract: %s\n' "$1" >&2; exit 1; }

# 1. Absent libcurl is a refusal, not a silent degraded binary.
if make CURL_LIBS= CURL_CFLAGS= -n >/dev/null 2>/tmp/fclaw_mockonly_err.$$; then
  fail "a build with no libcurl and no MOCK_ONLY=1 was accepted"
fi
grep -Fq 'make MOCK_ONLY=1' /tmp/fclaw_mockonly_err.$$ ||
  fail "the libcurl refusal does not name the MOCK_ONLY=1 escape"
rm -f /tmp/fclaw_mockonly_err.$$

# 2. A command-line CFLAGS must not disarm the feature defines.
make CFLAGS='-std=c11 -O0' -n 2>/dev/null | grep -Fq -- '-DFCLAW_HAVE_LIBCURL' ||
  fail "a command-line CFLAGS dropped -DFCLAW_HAVE_LIBCURL"

# 3. MOCK_ONLY=1 omits both optional libraries...
make MOCK_ONLY=1 -n 2>/dev/null | grep -Fq -- '-DFCLAW_HAVE_LIBCURL' &&
  fail "MOCK_ONLY=1 still defined FCLAW_HAVE_LIBCURL"
make MOCK_ONLY=1 -n 2>/dev/null | grep -Fq -- '-DFCLAW_HAVE_OPENSSL' &&
  fail "MOCK_ONLY=1 still defined FCLAW_HAVE_OPENSSL"

# 4. ...and the result compiles clean and runs the shipped mock floop.
make clean >/dev/null
if ! make MOCK_ONLY=1 -j4 >/tmp/fclaw_mockonly_build.$$ 2>&1; then
  sed -n '1,40p' /tmp/fclaw_mockonly_build.$$ >&2
  fail "the mock-only build did not compile"
fi
if grep -Eq 'warning:' /tmp/fclaw_mockonly_build.$$; then
  grep -E 'warning:' /tmp/fclaw_mockonly_build.$$ | head -5 >&2
  fail "the mock-only build is not warning-clean"
fi
rm -f /tmp/fclaw_mockonly_build.$$

# 5. The binary really omits both libraries, not just their defines.
linked=""
if command -v otool >/dev/null 2>&1; then
  linked="$(otool -L bin/fclaw 2>/dev/null || true)"
elif command -v ldd >/dev/null 2>&1; then
  linked="$(ldd bin/fclaw 2>/dev/null || true)"
fi
if [ -n "$linked" ]; then
  printf '%s' "$linked" | grep -Eiq 'libcurl|libssl|libcrypto' &&
    fail "the mock-only binary still links an optional library"
fi

mkdir -p workspace
out="$(./bin/fclaw run -a --text 'hello from the mock-only build')" ||
  fail "the mock-only binary could not complete a run"
case "$out" in
  *'"status":"done"'*) ;;
  *) fail "the mock-only hello run did not finish: $out" ;;
esac

printf 'mock-only build contract: PASS\n'
