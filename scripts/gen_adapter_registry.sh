#!/usr/bin/env bash
# Generate the fc_adapters[] registry for a chosen set of adapter names.
# Two sets exist: the build-time selection that bin/fclaw links, and every
# adapter, which the test binaries link so `make test` parity never depends
# on what the operator selected.
#
# Prints "changed" when the registry content actually moved. The caller uses
# that to invalidate the linked binary: a rebuild that swaps the adapter set
# often lands in the same filesystem second as the previous link, and make
# would then consider the binary up to date and quietly keep the old
# channels in it.
set -euo pipefail
out="$1"; shift
mkdir -p "$(dirname "$out")"
{
  printf '/* Generated from the adapters/ directory listing. Do not edit. */\n'
  printf '#include "../runtime/gateway/adapter.h"\n\n'
  for name in "$@"; do printf 'extern const FcAdapter fc_adapter_%s;\n' "$name"; done
  printf '\nconst FcAdapter *const fc_adapters[] = {\n'
  for name in "$@"; do printf '  &fc_adapter_%s,\n' "$name"; done
  [ "$#" -gt 0 ] || printf '  (const FcAdapter *)0,\n'
  printf '};\nconst size_t fc_adapter_count = %d;\n' "$#"
} > "$out.tmp"
if cmp -s "$out.tmp" "$out"; then
  rm -f "$out.tmp"
else
  mv -f "$out.tmp" "$out"
  printf 'changed\n'
fi
