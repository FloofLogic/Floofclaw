#!/usr/bin/env bash
# Build the native, relocatable macOS arm64 FloofClaw binary archive. This
# verifies and packages; it never uploads or changes a remote.
#
# macOS arm64 is the only prebuilt target. A portable Linux binary would need
# a musl toolchain -- a container or a cross-compiler -- and neither is part
# of the release process; Linux installs build from source with `make`.
set -euo pipefail

allow_dirty=0
if [ "${1:-}" = "--allow-dirty" ]; then
  allow_dirty=1
  shift
fi
[ "$#" -eq 1 ] || {
  echo "usage: $0 [--allow-dirty] OUTPUT_DIRECTORY" >&2
  exit 2
}

root="$(cd "$(dirname "$0")/.." && pwd -P)"
[ -f "$root/runtime/version.h" ] || {
  echo "binary-archive: run from a FloofClaw source tree" >&2
  exit 1
}
if [ "$allow_dirty" -ne 1 ] &&
   git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
   [ -n "$(git -C "$root" status --porcelain --untracked-files=normal)" ]; then
  echo "binary-archive: worktree is dirty; commit release inputs first" >&2
  exit 1
fi

case "$(uname -s):$(uname -m)" in
  Darwin:arm64) target="darwin-arm64" ;;
  *)
    echo "binary-archive: the prebuilt archive is built on macOS arm64 only;" \
         "on $(uname -s) $(uname -m) build from source with make" >&2
    exit 1 ;;
esac

version="$(sed -n 's/^#define FCLAW_VERSION "\([^"]*\)"/\1/p' "$root/runtime/version.h")"
[ -n "$version" ] || { echo "binary-archive: runtime version is missing" >&2; exit 1; }
output="$1"
mkdir -p "$output"
output="$(cd "$output" && pwd -P)"

cd "$root"
openssl_prefix="${OPENSSL_PREFIX:-$(brew --prefix openssl@3 2>/dev/null || true)}"
[ -f "$openssl_prefix/lib/libssl.a" ] &&
[ -f "$openssl_prefix/lib/libcrypto.a" ] || {
  echo "binary-archive: static OpenSSL 3 archives are required" >&2
  exit 1
}
make release-small \
  RELEASE_SMALL_MAX_BYTES=8388608 \
  OPENSSL_PREFIX="$openssl_prefix" \
  OPENSSL_LIBS="$openssl_prefix/lib/libssl.a $openssl_prefix/lib/libcrypto.a" \
  CURL_CFLAGS='' CURL_LIBS='-lcurl'
if otool -L bin/fclaw | grep -Eq '/(opt/homebrew|usr/local)/'; then
  otool -L bin/fclaw >&2
  echo "binary-archive: macOS binary retains a package-manager rpath" >&2
  exit 1
fi

./tests/run_in_isolated_copy.sh ./bin/fclaw run -a --text hello >/dev/null

temporary="$(mktemp -d "${TMPDIR:-/tmp}/floofclaw-binary.XXXXXX")"
trap 'rm -rf "$temporary"' EXIT
name="floofclaw-v${version}-${target}"
stage="$temporary/$name"
mkdir -p "$stage/bin"
cp bin/fclaw "$stage/bin/fclaw"
cp LICENSE "$stage/LICENSE"
printf '%s\n' \
  "FloofClaw v$version ($target)" \
  "Install bin/fclaw on PATH, then run: fclaw setup -h" \
  "Documentation: https://github.com/FloofLogic/Floofclaw" \
  > "$stage/README.txt"
find "$stage" -exec touch -t 200001010000 {} +

archive="$output/$name.tar.gz"
COPYFILE_DISABLE=1 tar -cf "$temporary/$name.tar" -C "$temporary" "$name"
gzip -n -9 < "$temporary/$name.tar" > "$archive"
hash="$(shasum -a 256 "$archive" | awk '{print $1}')"
printf '%s  %s\n' "$hash" "$(basename "$archive")" > "$archive.sha256"
printf 'binary-archive: PASS target=%s archive=%s sha256=%s\n' \
  "$target" "$archive" "$hash"
