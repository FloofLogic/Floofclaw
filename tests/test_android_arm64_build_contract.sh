#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/fclaw-android-contract.XXXXXX")
cleanup() {
  rm -rf -- "$tmp_dir"
}
trap cleanup EXIT INT TERM

tool_bin="$tmp_dir/ndk/toolchains/llvm/prebuilt/test-host/bin"
mkdir -p "$tool_bin" \
  "$tmp_dir/deps/curl/include/curl" "$tmp_dir/deps/curl/lib" \
  "$tmp_dir/deps/openssl/include/openssl" "$tmp_dir/deps/openssl/lib"
printf 'Pkg.Revision = 27.0.0\n' >"$tmp_dir/ndk/source.properties"
printf '/* fixture */\n' >"$tmp_dir/deps/curl/include/curl/curl.h"
printf '/* fixture */\n' >"$tmp_dir/deps/openssl/include/openssl/ssl.h"
: >"$tmp_dir/deps/curl/lib/libcurl.a"
: >"$tmp_dir/deps/openssl/lib/libssl.a"
: >"$tmp_dir/deps/openssl/lib/libcrypto.a"

cat >"$tool_bin/aarch64-linux-android28-clang" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$FCLAW_FAKE_CLANG_LOG"
output=
previous=
for argument in "$@"; do
  if [[ "$previous" == -o ]]; then
    output=$argument
    break
  fi
  previous=$argument
done
[[ -n "$output" ]]
printf 'fake arm64 output\n' >"$output"
chmod 0755 "$output"
EOF
cat >"$tool_bin/llvm-strip" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exit 0
EOF
cat >"$tool_bin/llvm-readelf" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "$1" in
  -hW)
    printf '  Class:                             ELF64\n'
    printf '  Machine:                           AArch64\n'
    ;;
  -lW)
    printf '      [Requesting program interpreter: /system/bin/linker64]\n'
    ;;
  *) exit 2 ;;
esac
EOF
chmod 0755 "$tool_bin/aarch64-linux-android28-clang" \
  "$tool_bin/llvm-strip" "$tool_bin/llvm-readelf"

export FCLAW_FAKE_CLANG_LOG="$tmp_dir/clang.log"
ANDROID_NDK_ROOT="$tmp_dir/ndk" \
FCLAW_ANDROID_DEPS_ROOT="$tmp_dir/deps" \
FCLAW_ANDROID_OUTPUT="$tmp_dir/out/fclaw" \
  "$root/scripts/build_android_arm64.sh" >"$tmp_dir/build.out"

[[ -x "$tmp_dir/out/fclaw" ]]
grep -Fq 'runtime/main.c' "$tmp_dir/clang.log"
grep -Fq -- '-DFCLAW_HAVE_OPENSSL' "$tmp_dir/clang.log"
grep -Fq -- '-DFCLAW_HAVE_LIBCURL' "$tmp_dir/clang.log"
grep -Fq -- '-Wl,-z,max-page-size=16384' "$tmp_dir/clang.log"
grep -Fq "built $tmp_dir/out/fclaw (API 28)" "$tmp_dir/build.out"

if ANDROID_NDK_ROOT="$tmp_dir/ndk" \
   FCLAW_ANDROID_DEPS_ROOT="$tmp_dir/deps" \
   FCLAW_ANDROID_OUTPUT="$tmp_dir/out/libfclaw.so" \
     "$root/scripts/build_android_arm64.sh" \
       >"$tmp_dir/bad.out" 2>"$tmp_dir/bad.err"; then
  printf 'android ARM64 builder accepted a non-fclaw output name\n' >&2
  exit 1
fi
grep -Fq "must name the normal binary 'fclaw'" "$tmp_dir/bad.err"

printf 'android ARM64 build contract: PASS\n'
