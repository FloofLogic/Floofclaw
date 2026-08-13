#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf 'android-arm64: %s\n' "$1" >&2
  exit 1
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$script_dir/.." && pwd)

api=${FCLAW_ANDROID_API:-28}
case "$api" in
  ''|*[!0-9]*) fail "FCLAW_ANDROID_API must be an integer" ;;
esac
if ((api < 28 || api > 99)); then
  fail "FCLAW_ANDROID_API must be between 28 and 99"
fi

ndk_root=${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}
[[ -n "$ndk_root" ]] ||
  fail "set ANDROID_NDK_ROOT to an installed Android NDK"
[[ -f "$ndk_root/source.properties" ]] ||
  fail "ANDROID_NDK_ROOT is not an Android NDK: $ndk_root"

deps_root=${FCLAW_ANDROID_DEPS_ROOT:-}
curl_root=${FCLAW_ANDROID_CURL_ROOT:-}
openssl_root=${FCLAW_ANDROID_OPENSSL_ROOT:-}
if [[ -n "$deps_root" ]]; then
  [[ -n "$curl_root" ]] || curl_root="$deps_root/curl"
  [[ -n "$openssl_root" ]] || openssl_root="$deps_root/openssl"
fi
[[ -n "$curl_root" ]] ||
  fail "set FCLAW_ANDROID_CURL_ROOT or FCLAW_ANDROID_DEPS_ROOT"
[[ -n "$openssl_root" ]] ||
  fail "set FCLAW_ANDROID_OPENSSL_ROOT or FCLAW_ANDROID_DEPS_ROOT"

for required in \
  "$curl_root/include/curl/curl.h" \
  "$curl_root/lib/libcurl.a" \
  "$openssl_root/include/openssl/ssl.h" \
  "$openssl_root/lib/libssl.a" \
  "$openssl_root/lib/libcrypto.a"; do
  [[ -f "$required" ]] || fail "required ARM64 dependency is missing: $required"
done

prebuilt_root="$ndk_root/toolchains/llvm/prebuilt"
toolchain_dir=
while IFS= read -r candidate; do
  if [[ -n "$toolchain_dir" ]]; then
    fail "Android NDK contains more than one host toolchain"
  fi
  toolchain_dir=$candidate
done < <(find "$prebuilt_root" -mindepth 1 -maxdepth 1 -type d -print)
[[ -n "$toolchain_dir" ]] || fail "Android NDK host toolchain is missing"

clang="$toolchain_dir/bin/aarch64-linux-android${api}-clang"
strip="$toolchain_dir/bin/llvm-strip"
readelf="$toolchain_dir/bin/llvm-readelf"
for tool in "$clang" "$strip" "$readelf"; do
  [[ -x "$tool" ]] || fail "required Android NDK tool is missing: $tool"
done

output=${FCLAW_ANDROID_OUTPUT:-"$root/dist/android-arm64/fclaw"}
[[ "$(basename -- "$output")" == "fclaw" ]] ||
  fail "FCLAW_ANDROID_OUTPUT must name the normal binary 'fclaw'"

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/fclaw-android-arm64.XXXXXX")
cleanup() {
  rm -rf -- "$tmp_dir"
}
trap cleanup EXIT INT TERM
mkdir -p "$tmp_dir/objects"

runtime_sources=$(make -s -C "$root" \
  OPENSSL_CFLAGS=x OPENSSL_LIBS=x CURL_CFLAGS=x CURL_LIBS=x \
  _print-runtime-src) || fail "could not evaluate RUNTIME_SRC"
[[ -n "$runtime_sources" ]] || fail "RUNTIME_SRC is empty"

build_revision=$(git -C "$root" rev-parse --short=12 HEAD 2>/dev/null ||
  printf 'unknown')
common_cflags=(
  -std=c11
  -Wall
  -Wextra
  -Wpedantic
  -O2
  -D_GNU_SOURCE
  -D_DEFAULT_SOURCE
  -D_POSIX_C_SOURCE=200809L
  "-DFCLAW_BUILD_REVISION=\"$build_revision\""
  -DFCLAW_HAVE_OPENSSL
  -DFCLAW_HAVE_LIBCURL
  -fPIE
  -fPIC
  -fstack-protector-strong
  -ffunction-sections
  -fdata-sections
  "-I$curl_root/include"
  "-I$openssl_root/include"
)

objects=()
source_count=0
while IFS= read -r source; do
  # runtime/ is the engine, adapters/ holds the channel adapter directories,
  # and build/ holds the generated adapter registry. Nothing else may compile
  # into the binary.
  [[ "$source" =~ ^(runtime|adapters|build)/[A-Za-z0-9_./-]+\.c$ ]] ||
    fail "invalid RUNTIME_SRC entry: $source"
  [[ -f "$root/$source" ]] || fail "RUNTIME_SRC file is missing: $source"
  ((source_count += 1))
  object=$(printf '%s/objects/%03d.o' "$tmp_dir" "$source_count")
  "$clang" "${common_cflags[@]}" -c "$root/$source" -o "$object"
  objects+=("$object")
done <<<"$runtime_sources"
((source_count > 0)) || fail "RUNTIME_SRC is empty"

unstripped="$tmp_dir/fclaw"
"$clang" -fPIE -pie \
  -Wl,-z,relro,-z,now \
  -Wl,-z,max-page-size=16384 \
  -Wl,--build-id=sha1 \
  -Wl,--gc-sections \
  -Wl,--no-undefined \
  "${objects[@]}" \
  -Wl,--start-group \
  "$curl_root/lib/libcurl.a" \
  "$openssl_root/lib/libssl.a" \
  "$openssl_root/lib/libcrypto.a" \
  -Wl,--end-group \
  -ldl \
  -o "$unstripped"
"$strip" --strip-all "$unstripped"

header=$($readelf -hW "$unstripped")
grep -Eq 'Class:[[:space:]]+ELF64' <<<"$header" ||
  fail "output is not ELF64"
grep -Eq 'Machine:[[:space:]]+AArch64' <<<"$header" ||
  fail "output is not AArch64"
program_headers=$($readelf -lW "$unstripped")
grep -Fq '/system/bin/linker64' <<<"$program_headers" ||
  fail "output does not use Android linker64"

mkdir -p "$(dirname -- "$output")"
install -m 0755 "$unstripped" "$output"
printf 'android-arm64: built %s (API %s)\n' "$output" "$api"
