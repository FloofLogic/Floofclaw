#!/usr/bin/env python3
"""Generate the static Runtime Flow inspection artifact.

This is intentionally app-local. It reads repo configuration and source files,
then writes one JSON view model consumed by app/runtime_flow/.
"""
import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path


ROOT_KEYS = {
    "generated_at",
    "repo_root",
    "default_floop",
    "loops",
    "agents",
    "actions",
    "model_profiles",
    "providers",
    "runtime_rules",
    "runs",
    "warnings",
}


def read_text(path: Path, default="") -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return default


def read_json(path: Path, required=True):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        if required:
            raise SystemExit(f"missing required file: {path}")
        return None
    except json.JSONDecodeError as exc:
        raise SystemExit(f"invalid JSON in {path}: {exc}") from exc


def rel(repo_root: Path, path: Path | None) -> str:
    if not path:
        return ""
    try:
        return str(path.relative_to(repo_root))
    except ValueError:
        return str(path)


def first_nonempty_lines(text: str, limit=28) -> str:
    lines = []
    for raw in text.splitlines():
        line = raw.rstrip()
        if line or lines:
            lines.append(line)
        if len(lines) >= limit:
            break
    return "\n".join(lines).strip()


def load_profiles(repo_root: Path):
    data = read_json(repo_root / "config" / "model_profiles.json") or {}
    profiles = []
    for item in data.get("profiles", []):
        if not isinstance(item, dict) or not item.get("id"):
            raise SystemExit("malformed config/model_profiles.json: each profile needs id")
        profiles.append({
            "id": item.get("id", ""),
            "provider": item.get("provider", ""),
            "model": item.get("model", ""),
            "protocol": item.get("protocol", item.get("provider", "")),
            "effort": item.get("effort", ""),
            "raw": item,
        })
    return profiles


def load_providers(repo_root: Path, warnings):
    path = repo_root / "config" / "llm_registry.json"
    data = read_json(path, required=False)
    if data is None:
        warnings.append("config/llm_registry.json not found; provider resolution is partial")
        return []
    providers = []
    for item in data.get("providers", []):
        if not isinstance(item, dict) or not item.get("id"):
            warnings.append("skipped malformed provider entry without id")
            continue
        providers.append({
            "id": item.get("id", ""),
            "kind": item.get("kind", item.get("api_format", "")),
            "url": item.get("url", item.get("base_url", "")),
            "auth_endpoint": item.get("auth_endpoint", ""),
            "raw": item,
        })
    return providers


def allowed_actions_from_prompt(prompt: str):
    actions = []
    in_allowed = False
    for raw in prompt.splitlines():
        line = raw.strip()
        low = line.lower()
        if low.startswith("allowed action"):
            in_allowed = True
            continue
        if in_allowed and (not line or low.startswith(("forbidden", "request id", "step behavior", "behavior", "examples", "the runtime"))):
            if line:
                in_allowed = False
            continue
        if in_allowed:
            m = re.match(r"-\s+`([^`]+)`", line)
            if m:
                actions.append(m.group(1).split()[0])
    return actions


def infer_emits_from_prompt(prompt: str, agent_id: str):
    emits = []
    for call in ["task.update", "message.send", "write_file", "read_file", "bash", "web_fetch"]:
        if call in prompt:
            emits.append(f"call:{call}")
    allowed_actions = allowed_actions_from_prompt(prompt)
    for action in allowed_actions:
        emits.append(f"call:{action}")
    if not emits and agent_id in {"state"}:
        emits.extend(["memory_patch", "conversation_patch", "task_patch", "affair_patch"])
    return sorted(dict.fromkeys(emits))


def infer_emits_from_script(script: str):
    emits = []
    for call in ["task.update", "message.send", "write_file", "read_file", "bash", "web_fetch"]:
        if re.search(r'\bname\s*:\s*["\']' + re.escape(call) + r'["\']', script) or \
           re.search(r'["\']name["\']\s*:\s*["\']' + re.escape(call) + r'["\']', script):
            emits.append(f"call:{call}")
    for event_type in ["action_request", "memory_patch", "tasks_patch", "task_patch", "affair_patch", "conversation_patch"]:
        if event_type in script:
            emits.append(event_type)
    for action in ["message", "write_file", "read_file", "memory_recall", "memory_write", "bash", "web_fetch"]:
        if f'"{action}"' in script or f"'{action}'" in script:
            emits.append(f"action_request:{action}")
    return sorted(dict.fromkeys(emits))


def side_effects_for_agent(agent):
    executor = agent.get("executor", "script")
    emits = agent.get("emits", [])
    effects = []
    state_effects = []
    if executor == "llm":
        effects.append("Calls configured LLM provider and stores raw provider artifacts.")
    elif executor == "script":
        effects.append("Runs agent-local run.sh and captures stdout/stderr under agent_outputs.")
    if any(item.startswith("call:") for item in emits) or any(item.startswith("action_request") for item in emits):
        effects.append("Requests work indirectly through calls normalized into runtime events.")
    if any(item.endswith("_patch") or item == "tasks_patch" for item in emits):
        state_effects.append("Emits patch events that the reducer applies to materialized state.")
    if not state_effects:
        state_effects.append("Does not mutate state directly; reducer handles any emitted events.")
    return effects, state_effects


def agent_input_shape(cfg):
    if cfg.get("conversational_payload_only"):
        return '{"summary":null,"recent":[...],"request":{"kind":"reply","text":"..."}|null}'
    listen = cfg.get("listen") if isinstance(cfg.get("listen"), list) else []
    parts = []
    if "memory" in listen:
        parts.append('"memory":{"memory_tail":[],"conversation_summary":null}')
    if "tasks" in listen:
        parts.append('"tasks":{"active":[]}')
    return "{" + ",".join(parts) + "}" if parts else "{}"


def agent_output_shape(cfg):
    if cfg.get("conversational_payload_only"):
        return '{"message":"..."|null}'
    return '{"calls":[]}'


def load_agents(repo_root: Path, profiles_by_id, providers_by_id, warnings):
    agents = []
    floops_root = repo_root / "floops"
    for floop_dir in sorted(p for p in floops_root.iterdir() if p.is_dir()):
        agents_root = floop_dir / "agents"
        if not agents_root.exists():
            warnings.append(f"floop {floop_dir.name} has no agents directory")
            continue
        for agent_dir in sorted(p for p in agents_root.iterdir() if p.is_dir()):
            agent_json_path = agent_dir / "agent.json"
            if not agent_json_path.exists():
                warnings.append(f"agent {floop_dir.name}/{agent_dir.name} has no agent.json")
                continue
            cfg = read_json(agent_json_path)
            agent_id = cfg.get("id", agent_dir.name)
            executor = cfg.get("executor", "script")
            model_ref = ((cfg.get("model") or {}).get("ref") or "") if isinstance(cfg.get("model"), dict) else ""
            profile = profiles_by_id.get(model_ref) if model_ref else None
            provider = providers_by_id.get(profile.get("provider")) if profile else None
            prompt_path = agent_dir / "prompt.md"
            run_path = agent_dir / "run.sh"
            prompt = read_text(prompt_path) if prompt_path.exists() else ""
            script = read_text(run_path) if run_path.exists() else ""
            emits = infer_emits_from_prompt(prompt, agent_id) if prompt else infer_emits_from_script(script)
            agent = {
                "id": agent_id,
                "key": f"{floop_dir.name}:{agent_id}",
                "floop": floop_dir.name,
                "directory": rel(repo_root, agent_dir),
                "executor": executor,
                "model": {
                    "ref": model_ref,
                    "profile": profile,
                    "provider": provider,
                },
                "prompt": prompt,
                "prompt_path": rel(repo_root, prompt_path) if prompt_path.exists() else "",
                "agent_json": cfg,
                "agent_json_path": rel(repo_root, agent_json_path),
                "run_script_path": rel(repo_root, run_path) if run_path.exists() else "",
                "run_script_excerpt": first_nonempty_lines(script),
                "input_shape": agent_input_shape(cfg),
                "output_shape": agent_output_shape(cfg),
                "emits": emits,
                "source_refs": [x for x in [rel(repo_root, agent_json_path), rel(repo_root, prompt_path) if prompt_path.exists() else "", rel(repo_root, run_path) if run_path.exists() else ""] if x],
            }
            if executor == "llm" and not profile:
                warnings.append(f"agent {floop_dir.name}/{agent_id} references missing model profile '{model_ref}'")
            agent["side_effects"], agent["state_effects"] = side_effects_for_agent(agent)
            agents.append(agent)
    return agents


def agents_by_floop(agents):
    out = {}
    for agent in agents:
        out.setdefault(agent.get("floop", ""), {})[agent.get("id", "")] = agent
    return out


def load_action_areas(repo_root: Path, warnings):
    """Mirror the runtime scan: every directory under actions/ is an area.

    There is no registration list. Names starting with '.' or '_' are skipped
    the same way the runtime skips them, so a directory parked as _name stays
    visible in the tree and out of the diagram.
    """
    actions_root = repo_root / "actions"
    if not actions_root.is_dir():
        warnings.append("actions/ not found; action areas are unavailable")
        return []
    areas = [
        {"id": child.name, "path": child.name, "enabled": True}
        for child in sorted(actions_root.iterdir())
        if child.is_dir() and not child.name.startswith((".", "_"))
    ]
    if not areas:
        warnings.append("actions/ holds no loadable areas")
    return areas


def load_actions(repo_root: Path, warnings):
    actions_root = repo_root / "actions"
    actions = []
    for area in load_action_areas(repo_root, warnings):
        area_root = actions_root / area["path"]
        if not area_root.exists():
            warnings.append(f"enabled action area {area['id']} missing at actions/{area['path']}")
            continue
        for manifest_path in sorted(area_root.rglob("action.json")):
            if any(part.startswith(".") for part in manifest_path.relative_to(area_root).parts):
                continue
            action_dir = manifest_path.parent
            run_path = action_dir / "run.sh"
            manifest = read_json(manifest_path, required=False) or {"id": action_dir.name}
            script = read_text(run_path)
            exec_cfg = manifest.get("exec") if isinstance(manifest, dict) else None
            is_runtime_action = isinstance(exec_cfg, list) and exec_cfg and exec_cfg[0] == "runtime"
            if not run_path.exists() and not is_runtime_action:
                warnings.append(f"action {manifest.get('id', action_dir.name)} has no run.sh")
            actions.append({
                "id": manifest.get("id", action_dir.name),
                "area": area["id"],
                "outside_world": bool(manifest.get("outside_world", False)),
                "manifest": manifest,
                "manifest_path": rel(repo_root, manifest_path) if manifest_path.exists() else "",
                "run_script_path": rel(repo_root, run_path) if run_path.exists() else "",
                "run_script_excerpt": first_nonempty_lines(script, limit=18),
                "result_shape": "{ events, result, error }",
                "source_refs": [x for x in [rel(repo_root, manifest_path) if manifest_path.exists() else "", rel(repo_root, run_path) if run_path.exists() else ""] if x],
            })
    return actions


def excerpt_for_anchor(repo_root: Path, relative_path: str, anchor: str, warnings, context=4):
    path = repo_root / relative_path
    text = read_text(path)
    if not text:
        warnings.append(f"missing runtime source for rule: {relative_path}")
        return None
    lines = text.splitlines()
    for idx, line in enumerate(lines):
        if anchor in line:
            start = max(0, idx - context)
            end = min(len(lines), idx + context + 1)
            return {
                "path": relative_path,
                "anchor": anchor,
                "line_start": start + 1,
                "line_end": end,
                "excerpt": "\n".join(lines[start:end]),
            }
    warnings.append(f"missing runtime rule anchor '{anchor}' in {relative_path}")
    return None


def runtime_rules(repo_root: Path, warnings):
    specs = [
        ("processor_input", "Processor input shape", "Processor input is rendered from agent.json flags: listen blocks, planner context, conversational payload, or selected-task payload.", "runtime/agent_model.c", "int rt_build_processor_input"),
        ("llm_agent_prompt", "LLM agent prompt assembly", "LLM agents render prompt.md with compact JSON and generated JSON tool docs when requested by that agent mode.", "runtime/agent_exec.c", "Reply with one JSON object"),
        ("agent_output_validation", "Agent output validation", "Agent output contracts vary by agent flags: planner plan, speaker message, selected-task tool, or generic calls[]. Invalid output appends no partial runtime events.", "runtime/agent_exec.c", "agent output did not contain a calls array"),
        ("action_runner_pending", "Action runner pending rule", "action_runner executes the next not-yet-started pending action_request; action_rejected, action_succeeded, and action_failed are terminal.", "runtime/action_runner.c", "if (e->started) continue"),
        ("message_delivery", "Message is normal action delivery", "The latest visible assistant text comes from the message action_succeeded delivery.", "runtime/state.c", "strcmp(action, \"message\")"),
        ("responses_decode", "Responses text decode", "OpenAI Responses output text is extracted and normalized into runtime event JSON.", "runtime/llm/http.c", "extract_openai_responses_text"),
    ]
    out = []
    for rid, title, summary, path, anchor in specs:
        ref = excerpt_for_anchor(repo_root, path, anchor, warnings)
        if not ref:
            continue
        out.append({"id": rid, "title": title, "summary": summary, "source_ref": ref})
    return out


def builtin_phase(step_id, target):
    if target == "action_runner":
        return {
            "step_id": step_id,
            "step_type": "builtin",
            "target": target,
            "resolved_kind": "builtin",
            "executor": "builtin",
            "model": None,
            "provider": None,
            "prompt": "",
            "prompt_path": "",
            "agent_json": {},
            "agent_json_path": "",
            "run_script_path": "",
            "run_script_excerpt": "",
            "input_shape": "current event_log + pending action_request events",
            "output_shape": {"events": ["action_started", "action_rejected", "action_succeeded", "action_failed"]},
            "emits": ["action_started", "action_rejected", "action_succeeded", "action_failed"],
            "side_effects": ["Executes pending action requests through registered runtime functions or actions/<area>/<id>/run.sh.", "Captures subprocess action inputs, outputs, and stderr under action_calls/."],
            "state_effects": ["Reducer observes action terminal events and updates latest_action/latest_result/latest_message.", "Task-bound terminal results project typed artifacts with task_updated events."],
            "source_refs": ["runtime/action_runner.c"],
        }
    return {
        "step_id": step_id,
        "step_type": "builtin",
        "target": target,
        "resolved_kind": "builtin",
        "executor": "builtin",
        "model": None,
        "provider": None,
        "prompt": "",
        "prompt_path": "",
        "agent_json": {},
        "agent_json_path": "",
        "run_script_path": "",
        "run_script_excerpt": "",
        "input_shape": "runtime state + recent events",
        "output_shape": {},
        "emits": [],
        "side_effects": ["Unknown builtin in inspection data."],
        "state_effects": ["No inferred state effect."],
        "source_refs": [],
    }


def action_phase(step_id, target, actions_by_id):
    action = actions_by_id.get(target, {})
    return {
        "step_id": step_id,
        "step_type": "action",
        "target": target,
        "resolved_kind": "action",
        "executor": "action",
        "model": None,
        "provider": None,
        "prompt": "",
        "prompt_path": "",
        "agent_json": {},
        "agent_json_path": "",
        "run_script_path": action.get("run_script_path", ""),
        "run_script_excerpt": action.get("run_script_excerpt", ""),
        "input_shape": "action request JSON from stdin",
            "output_shape": {"shape": "{ events, result, error }"},
        "emits": ["action_succeeded", "action_failed"],
        "side_effects": ["Runs action adapter directly for the configured step."],
        "state_effects": ["Reducer observes any emitted action terminal event.", "Task-bound terminal results project typed task artifacts."],
        "source_refs": action.get("source_refs", []),
    }


def agent_phase(step, agent, warnings):
    target = step.get("agent", "")
    if not agent:
        warnings.append(f"loop step {step.get('id')} references missing agent '{target}'")
        return {
            "step_id": step.get("id", ""),
            "step_type": step.get("type", ""),
            "target": target,
            "resolved_kind": "missing_agent",
            "executor": "missing",
            "model": None,
            "provider": None,
            "prompt": "",
            "prompt_path": "",
            "agent_json": {},
            "agent_json_path": "",
            "run_script_path": "",
            "run_script_excerpt": "",
            "input_shape": "unresolved",
            "output_shape": {},
            "emits": [],
            "side_effects": ["Missing agent; loop cannot execute this phase."],
            "state_effects": [],
            "source_refs": [],
        }
    return {
        "step_id": step.get("id", ""),
        "step_type": step.get("type", ""),
        "target": agent["id"],
        "resolved_kind": "agent",
        "executor": agent["executor"],
        "model": agent["model"],
        "provider": agent["model"].get("provider"),
        "prompt": agent["prompt"],
        "prompt_path": agent["prompt_path"],
        "agent_json": agent["agent_json"],
        "agent_json_path": agent["agent_json_path"],
        "run_script_path": agent["run_script_path"],
        "run_script_excerpt": agent["run_script_excerpt"],
        "input_shape": agent["input_shape"],
        "output_shape": agent["output_shape"],
        "emits": agent["emits"],
        "side_effects": agent["side_effects"],
        "state_effects": agent["state_effects"],
        "source_refs": agent["source_refs"],
    }


def load_loops(repo_root: Path, default_floop: str, agents_by_floop_id, actions_by_id, warnings):
    loops = []
    for floop_dir in sorted(p for p in (repo_root / "floops").iterdir() if p.is_dir()):
        path = floop_dir / "loop.json"
        if not path.exists():
            warnings.append(f"floop {floop_dir.name} has no loop.json")
            continue
        data = read_json(path)
        floop_id = floop_dir.name
        name = data.get("name") or floop_id
        floop_agents = agents_by_floop_id.get(floop_id, {})
        steps = []
        raw_steps = data.get("steps", [])
        if not isinstance(raw_steps, list):
            raise SystemExit(f"loop {path} has non-array steps")
        for raw in raw_steps:
            step_type = raw.get("type", "")
            step_id = raw.get("id", "")
            if step_type == "agent":
                steps.append(agent_phase(raw, floop_agents.get(raw.get("agent", "")), warnings))
            elif step_type == "builtin":
                steps.append(builtin_phase(step_id, raw.get("builtin", "")))
            elif step_type == "action":
                steps.append(action_phase(step_id, raw.get("action", ""), actions_by_id))
            else:
                warnings.append(f"loop {name} has unknown step type '{step_type}'")
        edges = []
        for idx in range(len(steps) - 1):
            edges.append({"from": steps[idx]["step_id"], "to": steps[idx + 1]["step_id"], "label": "next"})
        loops.append({
            "id": floop_id,
            "name": name,
            "description": data.get("description", ""),
            "path": rel(repo_root, path),
            "is_default": floop_id == default_floop or name == default_floop,
            "steps": steps,
            "edges": edges,
            "raw": data,
        })
    return loops




def read_jsonl(path: Path, warnings, limit=None):
    rows = []
    text = read_text(path)
    if not text:
        return rows
    for idx, raw in enumerate(text.splitlines(), start=1):
        if limit and len(rows) >= limit:
            break
        raw = raw.strip()
        if not raw:
            continue
        try:
            rows.append(json.loads(raw))
        except json.JSONDecodeError as exc:
            warnings.append(f"invalid JSONL in {rel(path.parents[2] if len(path.parents) > 2 else Path('.'), path)} line {idx}: {exc}")
    return rows


def compact_payload(payload, max_chars=1200):
    text = json.dumps(payload, ensure_ascii=False, sort_keys=True)
    if len(text) <= max_chars:
        return payload
    return {"_truncated": True, "text": text[:max_chars] + "..."}


def canonical_json(value):
    return json.dumps(value if value is not None else {}, sort_keys=True, separators=(",", ":"))


def event_pos_by_id(events):
    return {ev.get("event_id", ""): idx for idx, ev in enumerate(events)}


def analyze_run_events(events, provider_calls):
    findings = []
    requests_by_id = {}
    terminals_by_id = {}
    terminals_by_sig = {}
    message_texts = {}
    pos = event_pos_by_id(events)

    for ev in events:
        payload = ev.get("payload", {}) or {}
        etype = ev.get("type", "")
        if ev.get("type") == "action_request":
            rid = payload.get("request_id", "")
            if rid:
                requests_by_id[rid] = ev
        if ev.get("type") in {"action_succeeded", "action_failed", "action_rejected"}:
            rid = payload.get("request_id", "")
            if rid:
                terminals_by_id[rid] = ev
            action = payload.get("action", "")
            req = requests_by_id.get(rid, {})
            req_payload = req.get("payload", {}) or {}
            args = req_payload.get("args", {})
            if action:
                terminals_by_sig.setdefault((action, canonical_json(args)), []).append(ev)
            if action == "message" and ev.get("type") == "action_succeeded":
                text = (((payload.get("delivery") or {}).get("text")) or
                        ((payload.get("result") or {}).get("message")) or "")
                if text:
                    message_texts.setdefault(text, []).append(ev)

    for ev in events:
        if ev.get("type") != "action_rejected":
            continue
        payload = ev.get("payload", {}) or {}
        if payload.get("reason") != "duplicate_equivalent_action_call":
            continue
        rid = payload.get("request_id", "")
        req = requests_by_id.get(rid, {})
        req_payload = req.get("payload", {}) or {}
        action = payload.get("action", req_payload.get("action", ""))
        args = req_payload.get("args", {})
        prior = None
        for term in terminals_by_sig.get((action, canonical_json(args)), []):
            if term.get("event_id") == ev.get("event_id"):
                continue
            if pos.get(term.get("event_id", ""), 10**9) < pos.get(ev.get("event_id", ""), -1):
                prior = term
                break
        findings.append({
            "severity": "warning",
            "kind": "duplicate_equivalent_action_call",
            "title": "Repeated tool call rejected",
            "summary": f"{action or 'action'} was requested again with equivalent args after it already had a terminal outcome.",
            "impact": "The runtime rejected a duplicate effect instead of executing it twice.",
            "recommendation": "Inspect the prior terminal and the agent request that repeated the same action arguments.",
            "evidence": {
                "request_event_id": req.get("event_id", ""),
                "rejected_event_id": ev.get("event_id", ""),
                "prior_terminal_event_id": prior.get("event_id", "") if prior else "",
                "action": action,
                "args": args,
            },
        })

    for text, deliveries in message_texts.items():
        if len(deliveries) < 2:
            continue
        findings.append({
            "severity": "warning",
            "kind": "repeated_message_delivery",
            "title": "Same user-visible message delivered more than once",
            "summary": "The message path delivered identical text multiple times in one run.",
            "impact": "This is usually an agent or retry issue, not a transport issue.",
            "recommendation": "Inspect the pass timeline and action signatures for repeated message calls.",
            "evidence": {
                "message": text,
                "event_ids": [ev.get("event_id", "") for ev in deliveries],
            },
        })

    if findings:
        return findings
    return []


def artifact_text(path: Path, repo_root: Path, max_chars=24000):
    if not path.exists():
        return None
    text = read_text(path)
    truncated = len(text) > max_chars
    return {
        "path": rel(repo_root, path),
        "text": text[:max_chars] + ("\n... [truncated]" if truncated else ""),
        "truncated": truncated,
    }


def split_agent_output_stem(stem: str):
    m = re.match(r"^(\d{3})_(.+)$", stem)
    if m:
        return m.group(1), m.group(2)
    return "", stem


def run_sort_key(path: Path):
    m = re.search(r"run_(\d+)$", path.name)
    return int(m.group(1)) if m else -1


def load_provider_calls(run_dir: Path, repo_root: Path, warnings):
    calls_dir = run_dir / "provider_calls"
    calls = []
    if not calls_dir.exists():
        return calls
    for meta_path in sorted(calls_dir.glob("*_meta.json")):
        seq = meta_path.name.split("_")[0]
        meta = read_json(meta_path, required=False) or {}
        raw_request_path = calls_dir / f"{seq}_raw_request.json"
        raw_response_path = calls_dir / f"{seq}_raw_response.json"
        request_path = calls_dir / f"{seq}_request.json"
        response_path = calls_dir / f"{seq}_response.json"
        calls.append({
            "seq": seq,
            "agent": meta.get("agent", ""),
            "profile": meta.get("profile", ""),
            "provider": meta.get("provider", ""),
            "provider_kind": meta.get("provider_kind", ""),
            "model": meta.get("model", ""),
            "ok": meta.get("ok", None),
            "duration_ms": meta.get("duration_ms", None),
            "input_tokens": meta.get("input_tokens", None),
            "output_tokens": meta.get("output_tokens", None),
            "total_tokens": meta.get("total_tokens", None),
            "raw_request_path": rel(repo_root, raw_request_path) if raw_request_path.exists() else "",
            "raw_response_path": rel(repo_root, raw_response_path) if raw_response_path.exists() else "",
            "request_path": rel(repo_root, request_path) if request_path.exists() else "",
            "response_path": rel(repo_root, response_path) if response_path.exists() else "",
            "raw_request": artifact_text(raw_request_path, repo_root),
            "raw_response": artifact_text(raw_response_path, repo_root),
            "request": artifact_text(request_path, repo_root),
            "response": artifact_text(response_path, repo_root),
        })
    return calls


def load_agent_outputs(run_dir: Path, repo_root: Path):
    out_dir = run_dir / "agent_outputs"
    outputs = []
    if not out_dir.exists():
        return outputs
    for input_path in sorted(out_dir.glob("*.input.json")):
        stem = input_path.name.removesuffix(".input.json")
        seq, phase = split_agent_output_stem(stem)
        output_path = out_dir / f"{stem}.json"
        stderr_path = out_dir / f"{stem}.stderr"
        llm_response_path = out_dir / f"{stem}.llm_response.json"
        outputs.append({
            "seq": seq,
            "phase": phase,
            "input_path": rel(repo_root, input_path),
            "output_path": rel(repo_root, output_path) if output_path.exists() else "",
            "stderr_path": rel(repo_root, stderr_path) if stderr_path.exists() else "",
            "llm_response_path": rel(repo_root, llm_response_path) if llm_response_path.exists() else "",
            "input": artifact_text(input_path, repo_root),
            "output": artifact_text(output_path, repo_root),
            "stderr": artifact_text(stderr_path, repo_root),
            "llm_response": artifact_text(llm_response_path, repo_root),
        })
    return outputs


def short_text(text: str, max_chars=180) -> str:
    text = re.sub(r"\s+", " ", text or "").strip()
    if len(text) <= max_chars:
        return text
    return text[:max_chars] + "..."


def output_summary_from_artifact(artifact):
    if not artifact or not artifact.get("text"):
        return ""
    try:
        obj = json.loads(artifact.get("text") or "{}")
    except json.JSONDecodeError:
        return short_text(artifact.get("text", ""))
    if not isinstance(obj, dict):
        return short_text(json.dumps(obj, ensure_ascii=False))
    if "plan" in obj:
        plan = obj.get("plan")
        return "plan: no-op" if plan is None else "plan: " + short_text((plan or {}).get("work", ""))
    if "message" in obj:
        msg = obj.get("message")
        return "message: no-op" if msg is None else "message: " + short_text(msg)
    if "tool" in obj:
        tool = obj.get("tool")
        if tool is None:
            return "tool: no-op"
        args = obj.get("args") if isinstance(obj.get("args"), dict) else obj
        detail = (args.get("path") or args.get("url") or args.get("cmd")) if isinstance(args, dict) else ""
        return f"tool: {tool}" + (f" {short_text(detail, 90)}" if detail else "")
    if "calls" in obj:
        calls = obj.get("calls") if isinstance(obj.get("calls"), list) else []
        if not calls:
            return "calls: no-op"
        names = [str(call.get("name", "")) for call in calls if isinstance(call, dict)]
        return "calls: " + ", ".join(name for name in names if name)
    return short_text(json.dumps(obj, ensure_ascii=False))


def agent_output_by_phase_occurrence(agent_outputs):
    grouped = {}
    for item in sorted(agent_outputs, key=lambda x: (x.get("seq") or "999", x.get("phase", ""))):
        grouped.setdefault(item.get("phase", ""), []).append(item)
    return grouped


def provider_by_agent_occurrence(provider_calls):
    grouped = {}
    for item in sorted(provider_calls, key=lambda x: x.get("seq") or "999"):
        grouped.setdefault(item.get("agent", ""), []).append(item)
    return grouped


def loop_phase_targets(repo_root: Path, profile_name: str):
    path = repo_root / "floops" / profile_name / "loop.json"
    if not path.exists():
        # Legacy test fixtures may still provide loops/<name>.json.
        path = repo_root / "loops" / f"{profile_name}.json"
    data = read_json(path, required=False) or {}
    targets = {}
    for step in data.get("steps", []):
        phase = step.get("id", "")
        if not phase:
            continue
        targets[phase] = step.get("agent") or step.get("builtin") or step.get("action") or phase
    return targets


def normalized_events_from_output(agent_output):
    if not agent_output:
        return []
    artifact = agent_output.get("llm_response")
    if not artifact or not artifact.get("text"):
        return []
    try:
        root = json.loads(artifact.get("text") or "{}")
    except json.JSONDecodeError:
        return []
    events = (((root.get("runtime_meta") or {}).get("normalized_runtime_events")) or [])
    return events if isinstance(events, list) else []


def terminal_events_by_request(events):
    out = {}
    for ev in events:
        if ev.get("type") not in {"action_succeeded", "action_failed", "action_rejected"}:
            continue
        rid = ((ev.get("payload") or {}).get("request_id")) or ""
        if rid:
            out[rid] = ev
    return out


def action_calls_by_request(action_calls):
    return {call.get("request_id", ""): call for call in action_calls if call.get("request_id")}


def task_events_by_id(events):
    out = {}
    for ev in events:
        if ev.get("type") not in {"task_created", "task_updated", "task_completed", "task_failed"}:
            continue
        task_id = ((ev.get("payload") or {}).get("task_id")) or ""
        if task_id:
            out.setdefault(task_id, []).append(ev)
    return out


def build_run_passes(repo_root: Path, run_dir: Path, events, trace, provider_calls, agent_outputs, action_calls, profile_name: str):
    phase_targets = loop_phase_targets(repo_root, profile_name)
    output_by_phase = agent_output_by_phase_occurrence(agent_outputs)
    provider_by_agent = provider_by_agent_occurrence(provider_calls)
    terminals = terminal_events_by_request(events)
    action_by_request = action_calls_by_request(action_calls)
    task_events = task_events_by_id(events)
    skipped = {}
    decisions = {}
    for row in trace:
        if row.get("type") == "phase_skipped":
            skipped[(row.get("pass"), row.get("phase", ""))] = row.get("reason", "")
        elif row.get("type") == "loop_decision":
            decisions.setdefault(row.get("pass"), []).append(row)

    phase_rows = [row for row in trace if row.get("phase") and not row.get("type")]
    first_phase = phase_rows[0].get("phase") if phase_rows else ""
    phase_counts = {}
    agent_counts = {}
    passes = []
    current = None
    pass_index = 0

    for row in phase_rows:
        phase = row.get("phase", "")
        if phase == first_phase or current is None:
            pass_index += 1
            current = {"index": pass_index, "invocations": [], "decisions": []}
            passes.append(current)

        phase_counts[phase] = phase_counts.get(phase, 0) + 1
        phase_occ = phase_counts[phase]
        agent = phase_targets.get(phase, phase)
        agent_counts[agent] = agent_counts.get(agent, 0) + 1
        agent_occ = agent_counts[agent]
        agent_output = (output_by_phase.get(phase) or [None] * phase_occ)[phase_occ - 1] if len(output_by_phase.get(phase, [])) >= phase_occ else None
        provider = (provider_by_agent.get(agent) or [None] * agent_occ)[agent_occ - 1] if len(provider_by_agent.get(agent, [])) >= agent_occ else None
        accepted = normalized_events_from_output(agent_output)
        effects = []
        related_task_events = []
        related_task_ids = set()
        for ev in accepted:
            payload = ev.get("payload", {}) or {}
            task_id = payload.get("task_id", "")
            if task_id:
                related_task_ids.add(task_id)
            if ev.get("type") == "action_request":
                rid = payload.get("request_id", "")
                terminal = terminals.get(rid)
                action_call = action_by_request.get(rid)
                if terminal:
                    term_task = ((terminal.get("payload") or {}).get("task_id")) or ""
                    if term_task:
                        related_task_ids.add(term_task)
                effects.append({
                    "request": compact_payload(ev, max_chars=900),
                    "terminal": compact_payload(terminal, max_chars=900) if terminal else None,
                    "action_call": compact_payload(action_call, max_chars=900) if action_call else None,
                })
        for task_id in sorted(related_task_ids):
            related_task_events.extend(compact_payload(ev, max_chars=900) for ev in task_events.get(task_id, []))

        current["invocations"].append({
            "phase": phase,
            "agent": agent,
            "detail": row.get("detail", ""),
            "phase_occurrence": phase_occ,
            "agent_occurrence": agent_occ,
            "skipped": bool(skipped.get((pass_index, phase))),
            "skip_reason": skipped.get((pass_index, phase), ""),
            "provider_seq": provider.get("seq", "") if provider else "",
            "provider_call": provider,
            "agent_output": agent_output,
            "output_summary": output_summary_from_artifact((agent_output or {}).get("output")),
            "accepted_events": [compact_payload(ev, max_chars=900) for ev in accepted],
            "effects": effects,
            "task_events": related_task_events,
        })

    for item in passes:
        item["decisions"] = decisions.get(item["index"], [])
    return passes


def load_action_calls(run_dir: Path, repo_root: Path):
    calls_dir = run_dir / "action_calls"
    calls = []
    if not calls_dir.exists():
        return calls
    for input_path in sorted(calls_dir.glob("*.input.json")):
        name = input_path.name
        request_id = name.split("_", 1)[1].removesuffix(".input.json") if "_" in name else name.removesuffix(".input.json")
        output_path = input_path.with_name(input_path.name.replace(".input.json", ".output.json"))
        stderr_path = input_path.with_name(input_path.name.replace(".input.json", ".stderr"))
        input_json = read_json(input_path, required=False) or {}
        output_json = read_json(output_path, required=False) if output_path.exists() else None
        calls.append({
            "request_id": input_json.get("request_id", request_id),
            "action": input_json.get("action", ""),
            "ok": output_json.get("ok") if isinstance(output_json, dict) else None,
            "input_path": rel(repo_root, input_path),
            "output_path": rel(repo_root, output_path) if output_path.exists() else "",
            "stderr_path": rel(repo_root, stderr_path) if stderr_path.exists() else "",
            "input": artifact_text(input_path, repo_root),
            "output": artifact_text(output_path, repo_root),
            "stderr": artifact_text(stderr_path, repo_root),
            "args": compact_payload(input_json.get("args", {})),
            "result": compact_payload(output_json.get("result", {})) if isinstance(output_json, dict) else {},
            "error": output_json.get("error") if isinstance(output_json, dict) else None,
        })
    return calls


def phase_for_source(source: str, trace_phases):
    _ = trace_phases
    if source == "kernel":
        return "kernel"
    if source == "action_runner":
        return "action_runner"
    return source


def summarize_run(run_dir: Path, repo_root: Path, warnings):
    event_log_path = run_dir / "event_log.jsonl"
    events = read_jsonl(event_log_path, warnings)
    trace = read_jsonl(run_dir / "trace.jsonl", warnings)
    state_json = read_json(run_dir / "state.json", required=False) or {}
    provider_calls = load_provider_calls(run_dir, repo_root, warnings)
    agent_outputs = load_agent_outputs(run_dir, repo_root)
    action_calls = load_action_calls(run_dir, repo_root)
    analysis = analyze_run_events(events, provider_calls)
    trace_phase_ids = [row.get("phase", "") for row in trace if row.get("phase") and not row.get("type")]
    user_text = ""
    final_message = ""
    profile_name = ""
    status = "unknown"
    started_at = ""
    ended_at = ""
    by_phase = {}
    for ev in events:
        etype = ev.get("type", "")
        payload = ev.get("payload", {}) or {}
        if not started_at:
            started_at = ev.get("ts", "")
        ended_at = ev.get("ts", ended_at)
        if etype == "user_message":
            user_text = payload.get("text", user_text)
        if etype in {"run_started", "run_done", "run_failed", "run_canceled"} and payload.get("profile_name"):
            profile_name = payload.get("profile_name", profile_name)
        if etype == "run_done":
            status = "done"
        elif etype == "run_failed":
            status = "failed"
        elif etype == "run_canceled":
            status = "canceled"
        if etype == "action_succeeded" and payload.get("action") == "message":
            result = payload.get("result", {}) or {}
            final_message = result.get("message", final_message)
        phase = phase_for_source(ev.get("source", ""), trace)
        by_phase.setdefault(phase, []).append({
            "event_id": ev.get("event_id", ""),
            "type": etype,
            "source": ev.get("source", ""),
            "ts": ev.get("ts", ""),
            "payload": compact_payload(payload, max_chars=900),
        })
    passes = build_run_passes(repo_root, run_dir, events, trace, provider_calls,
                              agent_outputs, action_calls, profile_name)
    return {
        "id": run_dir.name,
        "path": rel(repo_root, run_dir),
        "event_log_path": rel(repo_root, event_log_path),
        "trace_path": rel(repo_root, run_dir / "trace.jsonl") if (run_dir / "trace.jsonl").exists() else "",
        "state_path": rel(repo_root, run_dir / "state.json") if (run_dir / "state.json").exists() else "",
        "started_at": started_at,
        "ended_at": ended_at,
        "status": status,
        "profile_name": profile_name,
        "user_text": user_text,
        "final_message": final_message,
        "trace_phases": trace_phase_ids,
        "events": events,
        "events_by_phase": by_phase,
        "provider_calls": provider_calls,
        "agent_outputs": agent_outputs,
        "action_calls": action_calls,
        "passes": passes,
        "analysis": analysis,
        "state": compact_payload(state_json, max_chars=1600),
        "event_count": len(events),
        "provider_call_count": len(provider_calls),
        "action_call_count": len(action_calls),
    }


def load_runs(repo_root: Path, warnings, limit=30):
    runs_root = repo_root / "workspace" / "runs"
    if not runs_root.exists():
        return []
    run_dirs = sorted((p for p in runs_root.iterdir() if p.is_dir() and p.name.startswith("run_")), key=run_sort_key, reverse=True)
    runs = []
    for run_dir in run_dirs[:limit]:
        if (run_dir / "event_log.jsonl").exists():
            runs.append(summarize_run(run_dir, repo_root, warnings))
    return runs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--out", default="app/runtime_flow/runtime_flow.json")
    ap.add_argument("--no-runs", action="store_true", help="do not include workspace/runs overlays")
    args = ap.parse_args()

    repo_root = Path(args.repo_root).resolve()
    warnings = []
    config = read_json(repo_root / "config" / "floofclaw_config.json") or {}
    default_floop = config.get("default_floop") or "floofclaw"
    profiles = load_profiles(repo_root)
    providers = load_providers(repo_root, warnings)
    profiles_by_id = {item["id"]: item for item in profiles}
    providers_by_id = {item["id"]: item for item in providers}
    agents = load_agents(repo_root, profiles_by_id, providers_by_id, warnings)
    actions = load_actions(repo_root, warnings)
    agent_map = agents_by_floop(agents)
    actions_by_id = {item["id"]: item for item in actions}
    loops = load_loops(repo_root, default_floop, agent_map, actions_by_id, warnings)
    rules = runtime_rules(repo_root, warnings)
    runs = [] if args.no_runs else load_runs(repo_root, warnings)

    if not any(loop["is_default"] for loop in loops):
        warnings.append(f"default floop '{default_floop}' did not match any floop")

    payload = {
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "repo_root": ".",
        "default_floop": default_floop,
        "loops": loops,
        "agents": agents,
        "actions": actions,
        "model_profiles": profiles,
        "providers": providers,
        "runtime_rules": rules,
        "runs": runs,
        "warnings": warnings,
    }
    missing = ROOT_KEYS - set(payload)
    if missing:
        raise SystemExit(f"internal generator error: missing keys {missing}")
    out = Path(args.out)
    if not out.is_absolute():
        out = repo_root / out
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(payload, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
