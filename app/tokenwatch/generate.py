#!/usr/bin/env python3
"""Generate Tokenwatch's static artifact from existing FloofClaw files."""

import argparse
import datetime as dt
import json
import os
import re
from pathlib import Path


def read_json(path, default=None):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        return default


def nonnegative_int(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def current_usage_record(record):
    strings = ("ts", "floop", "run_id", "agent", "profile", "provider", "model")
    counters = ("call_seq", "input_chars", "input_media_count",
                "input_media_bytes", "output_chars", "input_tokens",
                "output_tokens", "total_tokens", "duration_ms")
    if not isinstance(record, dict):
        return False
    if any(not isinstance(record.get(key), str) or not record[key]
           for key in strings):
        return False
    if any(not nonnegative_int(record.get(key)) for key in counters):
        return False
    if record["call_seq"] < 1:
        return False
    cached = record.get("provider_cached_input_tokens", object())
    if not (cached is None or nonnegative_int(cached)):
        return False
    created = record.get("provider_cache_creation_input_tokens", None)
    return created is None or nonnegative_int(created)


def current_cache_meta(meta):
    strings = ("floop", "agent", "profile", "provider", "model", "url")
    if not isinstance(meta, dict):
        return False
    if any(not isinstance(meta.get(key), str) or not meta[key]
           for key in strings):
        return False
    if not nonnegative_int(meta.get("input_tokens")):
        return False
    if not nonnegative_int(meta.get("input_media_count")):
        return False
    cached = meta.get("provider_cached_input_tokens", object())
    return cached is None or nonnegative_int(cached)


def active_floop(root, config, warnings):
    loop_path = root / ".fclaw" / "run" / "gateway.loop"
    pid_path = root / ".fclaw" / "run" / "gateway.pid"
    try:
        pid = int(pid_path.read_text(encoding="utf-8").strip())
        os.kill(pid, 0)
        selected = loop_path.read_text(encoding="utf-8").strip()
        if selected:
            return selected, "gateway"
    except (FileNotFoundError, PermissionError, ProcessLookupError, ValueError):
        pass
    selected = config.get("default_floop", "")
    if not selected:
        warnings.append("No live gateway floop or default_floop is available.")
        return "", "unavailable"
    return selected, "config_default"


def current_agents(root, floop, warnings):
    loop = read_json(root / "floops" / floop / "loop.json", {}) or {}
    profiles_doc = read_json(root / "config" / "model_profiles.json", {}) or {}
    profiles = {item.get("id"): item for item in profiles_doc.get("profiles", [])}
    agents = []
    seen = set()
    for step in loop.get("steps", []):
        if step.get("type") != "agent":
            continue
        agent_id = step.get("agent", "")
        if not agent_id or agent_id in seen:
            continue
        cfg_path = root / "floops" / floop / "agents" / agent_id / "agent.json"
        cfg = read_json(cfg_path, {}) or {}
        if cfg.get("executor", "script") != "llm":
            continue
        seen.add(agent_id)
        profile_id = ((cfg.get("model") or {}).get("ref", "")
                      if isinstance(cfg.get("model"), dict) else "")
        profile = profiles.get(profile_id, {})
        agents.append({
            "id": agent_id,
            "profile": profile_id,
            "provider": profile.get("provider", ""),
            "model": profile.get("model", ""),
        })
    if not loop:
        warnings.append(f"Could not read floops/{floop}/loop.json.")
    return agents


def usage_records(root, floop, warnings):
    path = root / "workspace" / "logs" / "llm_usage.jsonl"
    records = []
    try:
        with path.open(encoding="utf-8") as stream:
            for number, line in enumerate(stream, 1):
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    warnings.append(f"Skipped malformed llm_usage.jsonl line {number}.")
                    continue
                if not current_usage_record(record) or record["floop"] != floop:
                    continue
                records.append(record)
    except FileNotFoundError:
        pass
    return records


def run_order(run_id):
    match = re.match(r"run_(\d+)", run_id)
    return int(match.group(1)) if match else -1


def cache_records(root, floop, agents, recent, warnings):
    by_agent = {agent["id"]: [] for agent in agents}
    runs_root = root / "workspace" / "runs"
    if not runs_root.exists():
        return by_agent
    for run_dir in runs_root.iterdir():
        if not run_dir.is_dir() or not run_dir.name.startswith("run_"):
            continue
        calls_dir = run_dir / "provider_calls"
        if not calls_dir.exists():
            continue
        for meta_path in calls_dir.glob("*_meta.json"):
            match = re.match(r"(\d+)_meta\.json$", meta_path.name)
            if not match:
                continue
            seq = int(match.group(1))
            meta = read_json(meta_path)
            if not isinstance(meta, dict):
                warnings.append(f"Skipped malformed {meta_path.relative_to(root)}.")
                continue
            if not current_cache_meta(meta) or meta["floop"] != floop:
                continue
            if meta["input_media_count"] > 0:
                continue
            agent_id = meta["agent"]
            if agent_id not in by_agent:
                continue
            raw_path = calls_dir / f"{seq:03d}_raw_request.json"
            try:
                raw_request = raw_path.read_text(encoding="utf-8")
            except FileNotFoundError:
                continue
            by_agent[agent_id].append({
                "run_id": run_dir.name,
                "call_seq": seq,
                "profile": meta["profile"],
                "provider": meta["provider"],
                "model": meta["model"],
                "url": meta["url"],
                "bytes": len(raw_request.encode("utf-8")),
                "input_tokens": meta["input_tokens"],
                "provider_cached_input_tokens":
                    meta["provider_cached_input_tokens"],
                "raw_request": raw_request,
            })
    for agent_id, records in by_agent.items():
        records.sort(key=lambda item: (run_order(item["run_id"]),
                                       item["call_seq"]), reverse=True)
        by_agent[agent_id] = records[:recent]
    return by_agent


def generate(root, recent):
    warnings = []
    config = read_json(root / "config" / "floofclaw_config.json", {}) or {}
    floop, source = active_floop(root, config, warnings)
    agents = current_agents(root, floop, warnings) if floop else []
    records = usage_records(root, floop, warnings) if floop else []
    cache = cache_records(root, floop, agents, recent, warnings) if floop else {}
    return {
        "schema_version": 1,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "scope": {
            "kind": "current_log_segment",
            "path": "workspace/logs/llm_usage.jsonl",
            "floop": floop,
            "floop_source": source,
            "rotated_history_included": False,
            "recent_cache_calls_per_agent": recent,
        },
        "agents": agents,
        "usage_records": records,
        "cacheview_records": cache,
        "warnings": warnings,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--out", default="app/tokenwatch/tokenwatch.json")
    parser.add_argument("--recent", type=int, default=10)
    parser.add_argument("--fixture")
    args = parser.parse_args()
    if args.recent < 2 or args.recent > 10:
        raise SystemExit("--recent must be from 2 through 10")
    root = Path(args.repo_root).resolve()
    if args.fixture:
        data = read_json((root / args.fixture).resolve())
        if not isinstance(data, dict):
            raise SystemExit(f"invalid fixture: {args.fixture}")
    else:
        data = generate(root, args.recent)
    out_path = (root / args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
