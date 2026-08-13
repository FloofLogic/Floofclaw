#!/usr/bin/env bash
set -euo pipefail

out=""
cwd="$PWD"

while [ "$#" -gt 0 ]; do
  case "$1" in
    -o)
      out="$2"
      shift 2
      ;;
    -C)
      cwd="$2"
      shift 2
      ;;
    -)
      shift
      ;;
    *)
      shift
      ;;
  esac
done

prompt="$(cat)"
mkdir -p "$(dirname "$out")"

if [[ "${FCLAW_VERIFY_ACTION_CLI:-0}" == "1" ]]; then
  command -v fclaw >/dev/null
  fclaw action list -a | jq -e '.[] | select(.name == "read_file")' >/dev/null
fi

if grep -qi 'silent\.html' <<<"$prompt"; then
  # Deliberate silent-quiesce reproducer: claim success without writing the
  # named deliverable. The runtime should treat this as failure.
  printf 'Codex finished.\n' > "$out"
elif grep -qi 'frogger\.html' <<<"$prompt"; then
  cat > "$cwd/frogger.html" <<'HTML'
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Frogger</title>
  <style>
    body { margin: 0; background: #102018; color: #eef7df; font-family: Georgia, serif; }
    canvas { display: block; margin: 2rem auto; background: #1f4d3b; border: 6px solid #d9c46a; }
  </style>
</head>
<body>
  <canvas id="game" width="420" height="520"></canvas>
  <script>
    const canvas = document.getElementById('game');
    const ctx = canvas.getContext('2d');
    const frog = { x: 200, y: 480, s: 28 };
    const cars = [{ x: 20, y: 360, w: 80, v: 2 }, { x: 260, y: 300, w: 90, v: -3 }];
    addEventListener('keydown', e => {
      if (e.key === 'ArrowUp') frog.y -= 32;
      if (e.key === 'ArrowDown') frog.y += 32;
      if (e.key === 'ArrowLeft') frog.x -= 32;
      if (e.key === 'ArrowRight') frog.x += 32;
    });
    function frame() {
      ctx.clearRect(0, 0, 420, 520);
      ctx.fillStyle = '#245bca'; ctx.fillRect(0, 40, 420, 140);
      ctx.fillStyle = '#333'; ctx.fillRect(0, 260, 420, 160);
      ctx.fillStyle = '#f6d365';
      for (const car of cars) {
        car.x += car.v;
        if (car.x > 430) car.x = -car.w;
        if (car.x < -car.w) car.x = 430;
        ctx.fillRect(car.x, car.y, car.w, 30);
      }
      ctx.fillStyle = '#7cff6b'; ctx.fillRect(frog.x, frog.y, frog.s, frog.s);
      ctx.fillStyle = '#eef7df'; ctx.fillText('Reach the river bank. Arrow keys move.', 90, 24);
      requestAnimationFrame(frame);
    }
    frame();
  </script>
</body>
</html>
HTML
  printf 'Codex finished and wrote frogger.html.\n' > "$out"
else
  printf 'Codex finished.\n' > "$out"
fi

printf '{"event":"done","output_file":"%s"}\n' "$out"
