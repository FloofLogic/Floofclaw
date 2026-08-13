#!/usr/bin/env python3
"""Write the static Pulse artifact from the runtime-owned C projection.

This grandfathered Python app is intentionally a thin file-writing wrapper.
Pulse semantics live in runtime/pulse_projection.c and are shared with the
authenticated local client API.
"""

import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--out", default="app/pulse/pulse.json")
    parser.add_argument("--messages", type=int, default=40)
    parser.add_argument("--runs", type=int, default=25)
    parser.add_argument("--fclaw")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    fclaw = Path(args.fclaw).resolve() if args.fclaw else root / "bin/fclaw"
    command = [
        str(fclaw),
        "internal",
        "pulse-json",
        "--root",
        str(root),
        "--messages",
        str(args.messages),
        "--runs",
        str(args.runs),
    ]
    completed = subprocess.run(
        command, check=True, capture_output=True, text=True
    )
    projection = json.loads(completed.stdout)
    out_path = root / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps(projection, indent=2) + "\n", encoding="utf-8"
    )
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
