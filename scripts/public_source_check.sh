#!/usr/bin/env bash
# Checks that are safe to ship and run in a public source checkout.
set -euo pipefail

root="${1:-.}"
root="$(cd "$root" && pwd -P)"
fail=0

complain() {
  echo "public-source-check: $*" >&2
  fail=1
}

for forbidden in \
  .gitmodules .github/workflows AGENTS.md TODO.md RELEASING.md RELEASES.md \
  release_manifest.txt floofclaw-release-todo.md dev_research workspace .fclaw; do
  [ ! -e "$root/$forbidden" ] || complain "internal path is present: $forbidden"
done

generated_hits="$(find "$root" -mindepth 1 \
  \( -path "$root/.git" -prune \) -o \
  \( -name .git -o -name .gitmodules -o -name .DS_Store -o -name .zig-cache -o -name zig-out \) \
  -print)"
if [ -n "$generated_hits" ]; then
  printf '%s\n' "$generated_hits" >&2
  complain "Git metadata or generated build state is present"
fi

for generated in \
  app/pulse/pulse.json \
  app/runtime_flow/runtime_flow.json \
  app/tokenwatch/tokenwatch.json \
  actions/common/web_read/_pluck/zig-out/bin/pluck; do
  [ ! -e "$root/$generated" ] || complain "generated or platform-specific file is present: $generated"
done

[ -f "$root/actions/common/web_read/_pluck/build.zig" ] || \
  complain "flattened public pluck source is missing"

text_files() {
  find "$root" \
    \( -path "$root/.git" -prune \) -o \
    \( -type f ! -path "$root/scripts/public_source_check.sh" -print0 \)
}

scan_for() {
  local label="$1" pattern="$2" hits
  hits="$(text_files | xargs -0 grep -InE -- "$pattern" 2>/dev/null || true)"
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    complain "$label"
  fi
}

scan_for "absolute user-home path found" '/(Users|home)/[[:alnum:]_.-]+/'
scan_for "private Git transport or nonstandard private port found" '(git@|ssh://|https?://[^[:space:])]+:8443)'

if [ -d "$root/tests/fuzz/corpus" ]; then
  fuzz_hits="$(find "$root/tests/fuzz/corpus" -type f -print0 | \
    xargs -0 grep -InE -- '(chat:discord:|discord-main|@[[:alnum:]._%+-]+\.(com|net|org))' 2>/dev/null || true)"
  if [ -n "$fuzz_hits" ]; then
    printf '%s\n' "$fuzz_hits" >&2
    complain "fuzz corpus contains production-shaped identity or contact data"
  fi
fi

# Check local Markdown targets. URL, mail, app, and in-document anchors are
# intentionally outside this filesystem check.
while IFS= read -r -d '' markdown; do
  while IFS= read -r token; do
    target="${token#](}"
    target="${target%)}"
    target="${target%%#*}"
    target="${target%%\?*}"
    target="${target#<}"
    target="${target%>}"
    [ -n "$target" ] || continue
    case "$target" in
      http://*|https://*|mailto:*|app://*|data:*|'#'*) continue ;;
      /*) complain "absolute Markdown link in ${markdown#$root/}: $target"; continue ;;
    esac
    if [ ! -e "$(dirname "$markdown")/$target" ]; then
      complain "dangling Markdown link in ${markdown#$root/}: $target"
    fi
  done < <(grep -Eo '\]\([^)]*\)' "$markdown" 2>/dev/null || true)
done < <(find "$root" \
  \( -path "$root/.git" -prune \) -o \
  \( -type f -name '*.md' -print0 \))

version="$(sed -n 's/^#define FCLAW_VERSION "\([^"]*\)"/\1/p' "$root/runtime/version.h" 2>/dev/null || true)"
case "$version" in
  [0-9]*.[0-9]*.[0-9]*) ;;
  *) complain "runtime/version.h must contain a plain X.Y.Z release version" ;;
esac
if [ -n "$version" ] && ! printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  complain "release version is not strict semantic X.Y.Z: $version"
fi

[ "$fail" -eq 0 ] || exit 1
echo "public-source-check: PASS"
