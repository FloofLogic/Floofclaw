#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/fclaw-web-read.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/actions/common/web_read/_pluck/zig-out/bin"
cp "$repo_root/actions/common/web_read/run.sh" "$scratch/actions/common/web_read/run.sh"

request='{"args":{"op":"start","url":"https://example.test/article"}}'
if output="$(printf '%s\n' "$request" | bash "$scratch/actions/common/web_read/run.sh")"; then
  echo "web-read-test: missing pluck unexpectedly succeeded" >&2
  exit 1
fi
printf '%s\n' "$output" | grep -Fq '"code": "PLUCK_MISSING"' || {
  echo "web-read-test: missing helper did not return PLUCK_MISSING: $output" >&2
  exit 1
}
printf '%s\n' "$output" | grep -Fq 'bash actions/common/web_read/build.sh' || {
  echo "web-read-test: setup error did not name the documented build command" >&2
  exit 1
}

cat > "$scratch/actions/common/web_read/_pluck/zig-out/bin/pluck" <<'PLUCK'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' '{"title":"Synthetic article","url":"https://example.test/article","excerpt":"fixture","text_plain":"synthetic readable body","length":23}'
PLUCK
chmod +x "$scratch/actions/common/web_read/_pluck/zig-out/bin/pluck"

output="$(printf '%s\n' "$request" | bash "$scratch/actions/common/web_read/run.sh")"
printf '%s\n' "$output" | grep -Fq '"status": "finished"' || {
  echo "web-read-test: built helper path did not finish: $output" >&2
  exit 1
}
printf '%s\n' "$output" | grep -Fq 'synthetic readable body' || {
  echo "web-read-test: helper output was not returned: $output" >&2
  exit 1
}

echo "web-read-test: PASS"
