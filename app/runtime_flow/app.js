const state = {
  data: null,
  selectedLoopId: null,
  selectedPhaseId: null,
  selectedRunId: "",
};

const el = {
  snapshot: document.getElementById("snapshot-panel"),
  warnings: document.getElementById("warnings"),
  loopList: document.getElementById("loop-list"),
  title: document.getElementById("active-loop-title"),
  desc: document.getElementById("active-loop-desc"),
  badges: document.getElementById("active-loop-badges"),
  runSelect: document.getElementById("run-select"),
  runSummary: document.getElementById("run-summary"),
  runAnalysis: document.getElementById("run-analysis"),
  runPasses: document.getElementById("run-passes"),
  phaseFlow: document.getElementById("phase-flow"),
  detail: document.getElementById("detail-view"),
  models: document.getElementById("models"),
  actions: document.getElementById("actions"),
  rules: document.getElementById("rules"),
};

let regenerateBusy = false;

function esc(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

/* Tiny markdown renderer for message texts (bold, code, links,
 * bullet lists, paragraphs). Escapes first, so model/worker output
 * can never inject HTML. */
function mdInline(escaped) {
  return escaped
    .replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>")
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/\[([^\]]+)\]\((https?:[^)\s]+)\)/g,
             '<a href="$2" target="_blank" rel="noopener">$1</a>');
}

function mdToHtml(text) {
  const out = [];
  let list = false;
  for (const raw of esc(text).split("\n")) {
    const line = raw.trim();
    const item = line.match(/^[-*]\s+(.*)/);
    if (item) {
      if (!list) { out.push("<ul>"); list = true; }
      out.push(`<li>${mdInline(item[1])}</li>`);
      continue;
    }
    if (list) { out.push("</ul>"); list = false; }
    if (line) out.push(`<p>${mdInline(line)}</p>`);
  }
  if (list) out.push("</ul>");
  return out.join("");
}

function badge(text, kind = "") {
  return `<span class="badge ${esc(kind)}">${esc(text)}</span>`;
}

function orderedValue(value) {
  if (Array.isArray(value)) return value.map(orderedValue);
  if (!value || typeof value !== "object") return value;
  const entries = Object.entries(value);
  const preferred = [
    "conversation_summary",
    "summary",
    "memory_tail",
    "recent",
  ];
  const preferredEntries = [];
  preferred.forEach((key) => {
    const match = entries.find(([entryKey]) => entryKey === key);
    if (match) preferredEntries.push([match[0], orderedValue(match[1])]);
  });
  const remainingEntries = entries
    .filter(([key]) => !preferred.includes(key))
    .map(([key, entryValue]) => [key, orderedValue(entryValue)]);
  return Object.fromEntries(preferredEntries.concat(remainingEntries));
}

function pretty(value) {
  return JSON.stringify(orderedValue(value), null, 2);
}

function currentLoop() {
  return state.data.loops.find((loop) => loop.id === state.selectedLoopId) || state.data.loops[0];
}

function syntheticPhase(id) {
  if (id === "user_message") {
    return {
      step_id: "user_message",
      synthetic: true,
      step_type: "input",
      target: "user",
      resolved_kind: "input",
      executor: "input",
      input_shape: "raw external user/channel text",
      output_shape: "normalized user_message event for the next phase",
      emits: ["user_message"],
      side_effects: ["Appends user_message to the run event log before configured loop phases run."],
      state_effects: ["Reducer creates the input task that becomes processor input for later phases."],
    };
  }
  if (id === "visible_output") {
    return {
      step_id: "visible_output",
      synthetic: true,
      step_type: "output",
      target: "message",
      resolved_kind: "output",
      executor: "action",
      input_shape: "message action_succeeded",
      output_shape: "user-visible delivered text",
      emits: ["latest_message"],
      side_effects: ["Represents the delivered message text; not a loop profile step."],
      state_effects: [],
    };
  }
  return null;
}

function selectedPhase() {
  const loop = currentLoop();
  return syntheticPhase(state.selectedPhaseId)
    || loop?.steps.find((step) => step.step_id === state.selectedPhaseId)
    || loop?.steps[0]
    || null;
}


function selectedRun() {
  if (!state.selectedRunId) return null;
  return state.data.runs.find((run) => run.id === state.selectedRunId) || null;
}

function requestIdsForBuiltinPhase(run, step) {
  const loop = currentLoop();
  if (!run || !step || !loop) return new Set();
  const index = loop.steps.findIndex((item) => item.step_id === step.step_id);
  if (index < 0) return new Set();
  let producer = null;
  for (let i = index - 1; i >= 0; i -= 1) {
    if (loop.steps[i].resolved_kind === "agent" || loop.steps[i].resolved_kind === "action") {
      producer = loop.steps[i];
      break;
    }
  }
  if (!producer) return new Set();
  return new Set((run.events || [])
    .filter((event) => event.type === "action_request" && event.source === producer.target)
    .map((event) => event.payload?.request_id)
    .filter(Boolean));
}

function phaseEvents(run, step) {
  if (!run || !step) return [];
  if (step.step_id === "user_message") {
    return (run.events || []).filter((event) => event.type === "user_message");
  }
  if (step.step_id === "visible_output") {
    return (run.events || []).filter((event) => event.type === "action_succeeded" && event.payload?.action === "message");
  }
  if (step.resolved_kind === "builtin" && step.target === "action_runner") {
    const ids = requestIdsForBuiltinPhase(run, step);
    return (run.events || []).filter((event) => event.source === "action_runner" && ids.has(event.payload?.request_id));
  }
  let events = run.events_by_phase[step.step_id] || [];
  if (!events.length && step.resolved_kind === "agent") {
    events = run.events_by_phase[step.target] || [];
  }
  return events;
}

function phaseProviderCalls(run, step) {
  if (!run || !step) return [];
  return (run.provider_calls || []).filter((call) => call.agent === step.target);
}

function phaseAgentOutput(run, step) {
  if (!run || !step) return null;
  return (run.agent_outputs || []).find((item) => item.phase === step.step_id)
    || (run.agent_outputs || []).find((item) => item.phase === step.target)
    || null;
}

function phaseActionRequestEvents(run, step) {
  if (!run || !step) return [];
  if (step.resolved_kind === "builtin" && step.target === "action_runner") {
    const ids = requestIdsForBuiltinPhase(run, step);
    return (run.events || []).filter((event) => event.type === "action_request" && ids.has(event.payload?.request_id));
  }
  return phaseEvents(run, step).filter((event) => event.type === "action_request");
}

function phaseActionCalls(run, step) {
  if (!run || !step) return [];
  const events = phaseEvents(run, step);
  const requestIds = new Set(events.map((event) => event.payload?.request_id).filter(Boolean));
  return (run.action_calls || []).filter((call) => requestIds.has(call.request_id));
}

function runPhaseStats(step) {
  const run = selectedRun();
  if (!run) return null;
  const events = phaseEvents(run, step);
  const providers = phaseProviderCalls(run, step);
  const actions = phaseActionCalls(run, step);
  const agentOutput = phaseAgentOutput(run, step);
  const actionRequests = phaseActionRequestEvents(run, step);
  return { events, providers, actions, agentOutput, actionRequests };
}

function bestLoopForRun(run) {
  if (!run) return currentLoop();
  if (run.profile_name) {
    const exact = state.data.loops.find((loop) => loop.name === run.profile_name || loop.id === run.profile_name);
    if (exact) return exact;
  }
  let best = currentLoop();
  let bestScore = -1;
  const trace = new Set(run.trace_phases || []);
  state.data.loops.forEach((loop) => {
    const ids = new Set(loop.steps.map((step) => step.step_id));
    let score = 0;
    trace.forEach((phase) => { if (ids.has(phase)) score += 1; });
    if (score > bestScore) {
      best = loop;
      bestScore = score;
    }
  });
  return best;
}

function phaseSummary(step) {
  if (!step) return "";
  if (step.synthetic && step.step_id === "user_message") {
    return "Synthetic boundary: raw inbound text becomes the normalized user_message event consumed by the configured loop.";
  }
  if (step.synthetic && step.step_id === "visible_output") {
    return "Synthetic boundary: the message action_succeeded delivery becomes visible user-facing output.";
  }
  if (step.resolved_kind === "agent") {
    if (step.executor === "llm") return `LLM agent ${step.target} emits ${step.emits.join(", ") || "calls"}.`;
    return `Script agent ${step.target} emits ${step.emits.join(", ") || "calls"}.`;
  }
  if (step.resolved_kind === "builtin" && step.target === "action_runner") {
    return "Executes pending action_request events and appends action_started plus terminal action events.";
  }
  if (step.resolved_kind === "action") return `Runs action ${step.target}.`;
  return step.resolved_kind;
}

function modelLabel(step) {
  const profile = step?.model?.profile;
  const provider = step?.model?.provider;
  if (!profile) return "no model";
  const bits = [profile.model || profile.id, profile.provider].filter(Boolean);
  if (provider?.kind) bits.push(provider.kind);
  return bits.join(" · ");
}

function renderSnapshot() {
  const d = state.data;
  const items = [
    ["Default floop", d.default_floop || "unset"],
    ["Loops", d.loops.length],
    ["Agents", d.agents.length],
    ["Actions", d.actions.length],
    ["Models", d.model_profiles.length],
    ["Runs", (d.runs || []).length],
    ["Generated", new Date(d.generated_at).toLocaleString()],
  ];
  const buttonLabel = regenerateBusy ? "Re-generating..." : "Re-generate Flows";
  const buttonDisabled = regenerateBusy ? "disabled" : "";
  el.snapshot.innerHTML = `
    ${items.map(([k, v]) => `<div class="kv"><span>${esc(k)}</span><strong>${esc(v)}</strong></div>`).join("")}
    <button id="regenerate-flows" class="snapshot-action" type="button" ${buttonDisabled}>${esc(buttonLabel)}</button>
  `;
  const button = document.getElementById("regenerate-flows");
  if (button) {
    button.addEventListener("click", async () => {
      if (regenerateBusy) return;
      regenerateBusy = true;
      renderSnapshot();
      try {
        /* Regeneration happens outside the page (scripts/up.sh loop or a
         * manual generate.py run); this button just re-reads the artifact. */
        await load();
      } catch (err) {
        window.alert(`Runtime Flow reload failed: ${err.message}`);
      } finally {
        regenerateBusy = false;
        renderSnapshot();
      }
    });
  }
}

function renderWarnings() {
  const warnings = (state.data.warnings || []).concat(bootWarning ? [bootWarning] : []);
  el.warnings.innerHTML = warnings.map((w) => `<div class="warning-card">${esc(w)}</div>`).join("");
}

function renderLoops() {
  el.loopList.innerHTML = state.data.loops.map((loop) => {
    const active = loop.id === state.selectedLoopId;
    const defaultBadge = loop.is_default ? badge("default", "default") : "";
    return `
      <button class="loop-button ${active ? "active" : ""}" data-loop="${esc(loop.id)}">
        <div class="card-badges">${defaultBadge}${badge(`${loop.steps.length} phases`)}</div>
        <h3>${esc(loop.name)}</h3>
        <p>${esc(loop.description || loop.path)}</p>
      </button>
    `;
  }).join("");
  el.loopList.querySelectorAll("[data-loop]").forEach((btn) => {
    btn.addEventListener("click", () => {
      state.selectedLoopId = btn.dataset.loop;
      state.selectedPhaseId = currentLoop()?.steps[0]?.step_id || null;
      renderAll();
    });
  });
}

function renderRoundTrip(step) {
  if (step.executor !== "llm") return "";
  const profile = step.model?.profile || {};
  const provider = step.model?.provider || {};
  return `
    <div class="roundtrip">
      <div class="rt-node"><strong>${esc(step.target)} input</strong><span>prompt.md + processor task JSON</span></div>
      <div class="rt-arrow">req</div>
      <div class="rt-node model"><strong>${esc(profile.model || step.model?.ref || "model")}</strong><span>${esc(provider.id || profile.provider || "provider unresolved")}</span><span>${esc(provider.kind || "")}</span></div>
      <div class="rt-arrow">res</div>
      <div class="rt-node"><strong>calls</strong><span>model JSON call envelope</span></div>
    </div>
  `;
}

function renderScriptTrip(step) {
  if (step.executor !== "script") return "";
  return `
    <div class="roundtrip">
      <div class="rt-node"><strong>${esc(step.target)} input</strong><span>processor task JSON stdin</span></div>
      <div class="rt-arrow">run</div>
      <div class="rt-node model"><strong>run.sh</strong><span>${esc(step.run_script_path || "missing script")}</span></div>
      <div class="rt-arrow">out</div>
      <div class="rt-node"><strong>calls</strong><span>stdout JSON call envelope</span></div>
    </div>
  `;
}

function ioBox(label, value) {
  return `<div class="io-box"><strong>${esc(label)}</strong><span>${esc(value || "n/a")}</span></div>`;
}

function phaseCard(step, extraClass = "") {
  const active = step.step_id === state.selectedPhaseId;
  const kind = step.executor || step.resolved_kind;
  const stats = runPhaseStats(step);
  const runBadges = stats
    ? `${badge(`${stats.events.length} run events`)}${stats.providers.length ? badge(`${stats.providers.length} llm`, "llm") : ""}${stats.actions.length ? badge(`${stats.actions.length} actions`, "action") : ""}`
    : "";
  const syntheticBadge = step.synthetic ? badge("synthetic", "synthetic") : "";
  return `
    <article class="phase-card ${extraClass} ${active ? "active" : ""} ${stats && stats.events.length ? "has-run" : ""}" data-phase="${esc(step.step_id)}">
      <div class="card-badges">
        ${badge(step.step_type, step.step_type === "builtin" ? "builtin" : "")}
        ${syntheticBadge}
        ${badge(kind, kind)}
        ${step.model?.ref ? badge(`model:${step.model.ref}`, "llm") : ""}
        ${runBadges}
      </div>
      <h3>${esc(step.step_id)}</h3>
      <p>${esc(phaseSummary(step))}</p>
      <div class="io">
        ${ioBox("input", step.input_shape)}
        ${ioBox("output", step.output_shape || (Array.isArray(step.emits) && step.emits.length ? step.emits.join(", ") : "calls envelope"))}
      </div>
      ${renderRoundTrip(step)}
      ${renderScriptTrip(step)}
    </article>
  `;
}

function renderRunSelector() {
  const runs = state.data.runs || [];
  if (!runs.length) {
    el.runSelect.innerHTML = '<option value="">No runs found</option>';
    el.runSelect.disabled = true;
    el.runSummary.innerHTML = '';
    el.runAnalysis.innerHTML = '';
    el.runPasses.innerHTML = '';
    return;
  }
  el.runSelect.disabled = false;
  el.runSelect.innerHTML = ['<option value="">No run overlay</option>']
    .concat(runs.map((run) => {
      const profile = run.profile_name ? `${run.profile_name} · ` : "";
      const label = `${run.id} · ${profile}${run.status} · ${run.user_text || '(no user text)'}`;
      return `<option value="${esc(run.id)}" ${run.id === state.selectedRunId ? 'selected' : ''}>${esc(label)}</option>`;
    }))
    .join('');
  el.runSelect.onchange = () => {
    state.selectedRunId = el.runSelect.value;
    const url = new URL(window.location);
    if (state.selectedRunId) url.searchParams.set("run", state.selectedRunId);
    else url.searchParams.delete("run");
    window.history.replaceState(null, "", url);
    const run = selectedRun();
    const matchedLoop = bestLoopForRun(run);
    if (matchedLoop) {
      state.selectedLoopId = matchedLoop.id;
      state.selectedPhaseId = matchedLoop.steps[0]?.step_id || null;
    }
    renderAll();
  };
  const run = selectedRun();
  if (!run) {
    el.runSummary.innerHTML = '<div class="empty-run">Select a run to overlay actual events, provider calls, and action calls on the configured loop.</div>';
    el.runAnalysis.innerHTML = '';
    el.runPasses.innerHTML = '';
    return;
  }
  el.runSummary.innerHTML = `
    <div class="run-card">
      <div class="card-badges">${run.profile_name ? badge(run.profile_name, "default") : ""}${badge(run.status)}${badge(`${run.event_count} events`)}${badge(`${run.provider_call_count} llm`)}${badge(`${run.action_call_count} actions`)}</div>
      <strong>${esc(run.id)}</strong>
      <p>${esc(run.user_text || 'No user text')}</p>
      ${run.final_message ? `<div class="run-final">${mdToHtml(run.final_message)}</div>` : ''}
    </div>
  `;
  el.runAnalysis.innerHTML = renderRunAnalysis(run);
  el.runPasses.innerHTML = renderRunPasses(run);
}

function renderRunAnalysis(run) {
  const findings = run?.analysis || [];
  if (!findings.length) return '<div class="analysis-card ok">No analyzer findings for this run.</div>';
  return findings.map((finding) => {
    const evidence = finding.evidence || {};
    const evidenceRows = Object.entries(evidence)
      .map(([key, value]) => `<div class="analysis-kv"><span>${esc(key)}</span><strong>${esc(typeof value === "object" ? pretty(value) : value)}</strong></div>`)
      .join("");
    return `
      <article class="analysis-card ${esc(finding.severity || "info")}">
        <div class="card-badges">${badge(finding.severity || "info")}${badge(finding.kind || "finding")}</div>
        <h3>${esc(finding.title || "Analyzer finding")}</h3>
        <p>${esc(finding.summary || "")}</p>
        <p>${esc(finding.impact || "")}</p>
        <p>${esc(finding.recommendation || "")}</p>
        <div class="analysis-evidence">${evidenceRows}</div>
      </article>
    `;
  }).join("");
}

function renderRunPasses(run) {
  const passes = run?.passes || [];
  if (!passes.length) return '<div class="analysis-card ok">No pass timeline generated for this run.</div>';
  return `
    <section class="pass-timeline">
      <div class="pass-timeline-head">
        <strong>Observed passes</strong>
        <span>${esc(passes.length)} pass${passes.length === 1 ? "" : "es"} · repeated phases stay separate</span>
      </div>
      ${passes.map((pass) => {
        const decisions = pass.decisions || [];
        const decisionBadges = decisions.length
          ? decisions.map((item) => badge(`${item.decision || "decision"}:${item.reason || ""}`, "default")).join("")
          : badge("no loop decision trace");
        const invocations = (pass.invocations || []).map((inv) => {
          const bits = [
            inv.provider_seq ? `llm ${inv.provider_seq}` : "",
            inv.skipped ? `skipped: ${inv.skip_reason || "unknown"}` : "",
            inv.accepted_events?.length ? `${inv.accepted_events.length} accepted` : "",
            inv.effects?.length ? `${inv.effects.length} effects` : "",
            inv.task_events?.length ? `${inv.task_events.length} task events` : "",
          ].filter(Boolean);
          return `
            <div class="pass-invocation ${inv.skipped ? "skipped" : ""}">
              <div>
                <strong>${esc(inv.agent || inv.phase)}</strong>
                <span>${esc(inv.phase || "")}</span>
              </div>
              <p>${esc(inv.output_summary || (inv.skipped ? "phase skipped" : "no captured output"))}</p>
              <div class="card-badges">${bits.map((item) => badge(item, inv.skipped ? "builtin" : "")).join("")}</div>
            </div>
          `;
        }).join("");
        return `
          <article class="pass-card">
            <div class="pass-card-head">
              <h3>Pass ${esc(pass.index)}</h3>
              <div class="card-badges">${decisionBadges}</div>
            </div>
            <div class="pass-invocations">${invocations}</div>
          </article>
        `;
      }).join("")}
    </section>
  `;
}

function renderFlow() {
  const loop = currentLoop();
  if (!loop) {
    el.phaseFlow.innerHTML = '<div class="empty-state">No loops found.</div>';
    return;
  }
  el.title.textContent = loop.name;
  el.desc.textContent = loop.description || loop.path;
  el.badges.innerHTML = `${loop.is_default ? badge("configured default", "default") : ""}${badge(loop.path)}${badge(`${loop.steps.length} phases`)}`;

  const userNode = syntheticPhase("user_message");
  const outNode = syntheticPhase("visible_output");
  const cards = [phaseCard(userNode, "user")]
    .concat(loop.steps.map((step) => phaseCard(step)))
    .concat([phaseCard(outNode, "output")]);
  el.phaseFlow.innerHTML = `<div class="phase-lane">${cards.join('<div class="arrow">next</div>')}</div>`;
  el.phaseFlow.querySelectorAll("[data-phase]").forEach((node) => {
    node.addEventListener("click", () => {
      state.selectedPhaseId = node.dataset.phase;
      renderFlow();
      renderDetail();
    });
  });
}

function renderDetailBlock(title, body) {
  return `<section class="detail-block"><h3>${esc(title)}</h3>${body}</section>`;
}

function renderArtifactBlock(title, artifact, empty = "No artifact captured for this boundary.") {
  if (!artifact || artifact.text === undefined || artifact.text === null) {
    return renderDetailBlock(title, `<p class="muted">${esc(empty)}</p>`);
  }
  const meta = artifact.path
    ? `<div class="artifact-path">${esc(artifact.path)}${artifact.truncated ? " · truncated" : ""}</div>`
    : "";
  return renderDetailBlock(title, `${meta}<pre>${esc(artifact.text)}</pre>`);
}

function renderArtifactJsonBlock(title, artifact, empty = "No artifact captured for this boundary.") {
  if (!artifact || artifact.text === undefined || artifact.text === null) {
    return renderDetailBlock(title, `<p class="muted">${esc(empty)}</p>`);
  }
  let text = artifact.text;
  if (!artifact.truncated) {
    try {
      text = pretty(JSON.parse(artifact.text));
    } catch (_) {
      text = artifact.text;
    }
  }
  const meta = artifact.path
    ? `<div class="artifact-path">${esc(artifact.path)}${artifact.truncated ? " · truncated" : ""}</div>`
    : "";
  return renderDetailBlock(title, `${meta}<pre>${esc(text)}</pre>`);
}

function renderJsonBlock(title, value, empty = "No run data captured for this boundary.") {
  const isEmptyArray = Array.isArray(value) && value.length === 0;
  const isEmptyObject = value && typeof value === "object" && !Array.isArray(value) && !Object.keys(value).length;
  if (value === null || value === undefined || isEmptyArray || isEmptyObject) {
    return renderDetailBlock(title, `<p class="muted">${esc(empty)}</p>`);
  }
  return renderDetailBlock(title, `<pre>${esc(pretty(value))}</pre>`);
}

function renderTextBlock(title, value, empty = "No text captured for this boundary.") {
  if (value === null || value === undefined || value === "") {
    return renderDetailBlock(title, `<p class="muted">${esc(empty)}</p>`);
  }
  return renderDetailBlock(title, `<pre>${esc(value)}</pre>`);
}

function renderList(items) {
  if (!items || !items.length) return '<p class="muted">None inferred.</p>';
  return `<ul>${items.map((item) => `<li>${esc(item)}</li>`).join("")}</ul>`;
}

function renderProviderCallBlocks(call, index) {
  const label = `LLM call ${call.seq || index + 1}`;
  const meta = {
    agent: call.agent,
    provider: call.provider,
    model: call.model,
    ok: call.ok,
    duration_ms: call.duration_ms,
    input_tokens: call.input_tokens,
    output_tokens: call.output_tokens,
    total_tokens: call.total_tokens,
  };
  return [
    renderJsonBlock(`${label}: provider metadata`, meta),
    renderArtifactBlock(`${label}: raw request to server`, call.raw_request),
    renderArtifactBlock(`${label}: raw response from server`, call.raw_response),
    call.request ? renderArtifactBlock(`${label}: normalized request`, call.request) : "",
    call.response ? renderArtifactJsonBlock(`${label}: normalized response`, call.response) : "",
  ].join("");
}

function renderActionCallBlocks(call) {
  const title = `${call.action || "action"} ${call.request_id || ""}`.trim();
  const stderr = call.stderr?.text ? renderArtifactBlock(`Action stderr: ${title}`, call.stderr) : "";
  return [
    renderJsonBlock(`Action summary: ${title}`, {
      request_id: call.request_id,
      action: call.action,
      ok: call.ok,
      args: call.args,
      result: call.result,
      error: call.error,
    }),
    renderArtifactJsonBlock(`Action call input: ${title}`, call.input),
    renderArtifactJsonBlock(`Action call output: ${title}`, call.output),
    stderr,
  ].join("");
}

function renderRunBoundary(phase) {
  const run = selectedRun();
  const stats = runPhaseStats(phase);
  if (!run || !stats) return "";

  const blocks = [
    renderDetailBlock("Observed Run", `
      <div class="card-badges">${badge(run.id)}${badge(`${stats.events.length} events`)}${stats.providers.length ? badge(`${stats.providers.length} llm`, "llm") : ""}${stats.actions.length ? badge(`${stats.actions.length} actions`, "action") : ""}</div>
      <p>Actual artifacts for this selected phase. These are the concrete values that crossed the runtime boundary in this run.</p>
    `),
  ];

  if (phase.step_id === "user_message") {
    const first = stats.events[0] || null;
    const rawText = first?.payload?.text || "";
    const normalized = first ? {
      type: "user_message",
      source: first.source || "user",
      payload: {
        text: rawText,
      },
    } : null;
    blocks.push(
      renderTextBlock("Input: raw user text", rawText),
      renderJsonBlock("Output: normalized user_message for next phase", normalized),
    );
    return blocks.join("");
  }

  if (phase.step_id === "visible_output") {
    const deliveries = stats.events.map((event) => event.payload?.delivery || event.payload?.result || event.payload).filter(Boolean);
    blocks.push(
      renderJsonBlock("Input: message action_succeeded event", stats.events),
      renderJsonBlock("Output: delivered user-facing text", deliveries),
    );
    return blocks.join("");
  }

  if (phase.resolved_kind === "builtin" && phase.target === "action_runner") {
    blocks.push(renderJsonBlock("Input: pending action_request events", stats.actionRequests));
    if (stats.actions.length) {
      stats.actions.forEach((call) => blocks.push(renderActionCallBlocks(call)));
    } else {
      blocks.push(renderDetailBlock("Action calls", '<p class="muted">No matching action calls for this phase in the selected run.</p>'));
    }
    blocks.push(renderJsonBlock("Output: action_started / terminal action events", stats.events));
    return blocks.join("");
  }

  if (phase.resolved_kind === "agent") {
    blocks.push(renderArtifactBlock("Call input: agent prompt + processor task JSON", stats.agentOutput?.input));
    if (stats.providers.length) {
      stats.providers.forEach((call, index) => blocks.push(renderProviderCallBlocks(call, index)));
    } else if (phase.executor === "llm") {
      blocks.push(renderDetailBlock("LLM calls", '<p class="muted">No provider call matched this agent in the selected run.</p>'));
    }
    blocks.push(
      renderArtifactJsonBlock("Call output: agent calls JSON", stats.agentOutput?.output),
      renderJsonBlock("Output: events appended by this phase", stats.events),
    );
    return blocks.join("");
  }

  blocks.push(renderJsonBlock("Output: matching run events", stats.events));
  return blocks.join("");
}

function renderDetail() {
  const phase = selectedPhase();
  if (!phase) {
    el.detail.innerHTML = '<div class="empty-state">Select a phase.</div>';
    return;
  }
  const sourceRefs = (phase.source_refs || []).map((ref) => typeof ref === "string" ? ref : ref.path || JSON.stringify(ref));
  const blocks = [
    renderRunBoundary(phase),
    renderDetailBlock("Configured Phase", `<p>${esc(phaseSummary(phase) || phase.input_shape)}</p><div class="card-badges">${badge(phase.resolved_kind)}${badge(phase.executor, phase.executor)}${phase.model?.ref ? badge(`model:${phase.model.ref}`, "llm") : ""}</div>`),
    renderDetailBlock("Configured Shape", `<pre>${esc(pretty({ input_shape: phase.input_shape, output_shape: phase.output_shape, emits: phase.emits }))}</pre>`),
  ];
  blocks.push(
    renderDetailBlock("Side Effects", renderList(phase.side_effects)),
    renderDetailBlock("State Effects", renderList(phase.state_effects)),
    renderDetailBlock("Prompt", phase.prompt ? `<pre>${esc(phase.prompt)}</pre>` : '<p class="muted">No prompt.md for this phase.</p>'),
    renderDetailBlock("Agent JSON", Object.keys(phase.agent_json || {}).length ? `<pre>${esc(pretty(phase.agent_json))}</pre>` : '<p class="muted">No agent.json for this phase.</p>'),
    renderDetailBlock("Run Script Excerpt", phase.run_script_excerpt ? `<pre>${esc(phase.run_script_excerpt)}</pre>` : '<p class="muted">No run.sh excerpt.</p>'),
    renderDetailBlock("Source Refs", renderList(sourceRefs)),
  );
  el.detail.innerHTML = blocks.join("");
}

function renderModels() {
  const providersById = new Map(state.data.providers.map((p) => [p.id, p]));
  el.models.innerHTML = state.data.model_profiles.map((profile) => {
    const provider = providersById.get(profile.provider) || {};
    return `
      <article class="tiny-card">
        <div class="card-badges">${badge(profile.id, "llm")}${provider.kind ? badge(provider.kind) : ""}</div>
        <h3>${esc(profile.model || profile.id)}</h3>
        <p>${esc(profile.provider || "provider unset")}</p>
        <p>${esc(provider.url || "provider URL unavailable")}</p>
      </article>
    `;
  }).join("");
}

function renderActions() {
  el.actions.innerHTML = state.data.actions.map((action) => `
    <article class="tiny-card">
      <div class="card-badges">${badge(action.area || "registered", "default")}${badge(action.outside_world ? "outside" : "internal", "action")}</div>
      <h3>${esc(action.id)}</h3>
      <p>${esc(action.run_script_path || action.manifest_path || "no source")}</p>
    </article>
  `).join("");
}

function renderRules() {
  el.rules.innerHTML = state.data.runtime_rules.map((rule) => `
    <article class="rule-card">
      <h3>${esc(rule.title)}</h3>
      <p>${esc(rule.summary)}</p>
      <p><code>${esc(rule.source_ref.path)}:${esc(rule.source_ref.line_start)}</code></p>
    </article>
  `).join("");
}

function renderAll() {
  renderSnapshot();
  renderWarnings();
  renderLoops();
  renderRunSelector();
  renderFlow();
  renderDetail();
  renderModels();
  renderActions();
  renderRules();
}

async function load() {
  try {
    const res = await fetch("runtime_flow.json", { cache: "no-store" });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    state.data = await res.json();
    const defaultLoop = state.data.loops.find((loop) => loop.is_default) || state.data.loops[0];
    state.selectedLoopId = defaultLoop?.id || null;
    state.selectedPhaseId = defaultLoop?.steps?.[0]?.step_id || null;
    applyDeepLink();
    renderAll();
  } catch (err) {
    document.body.innerHTML = `<main class="page-shell"><section class="panel"><h1>Runtime Flow failed to load</h1><p>${esc(err.message)}</p><p>Run <code>python3 app/runtime_flow/generate.py --repo-root . --out app/runtime_flow/runtime_flow.json</code> and serve this directory over HTTP.</p></section></main>`;
  }
}

/* Deep link: ?run=run_144 selects that run overlay (and its loop) on
 * load, so other tools — Pulse's run cards — can point straight at an
 * analyzed run. Applied once; the dropdown owns the selection after. */
let deepLinkApplied = false;
let bootWarning = "";
function applyDeepLink() {
  if (deepLinkApplied) return;
  deepLinkApplied = true;
  const wanted = new URLSearchParams(window.location.search).get("run");
  if (!wanted) return;
  const run = (state.data.runs || []).find((r) => r.id === wanted);
  if (!run) {
    bootWarning = `Run ${wanted} is not in the loaded snapshot`
      + `${bootWarning ? ` (${bootWarning})` : ""}. `
      + `Hard-refresh (Cmd+Shift+R) or click Re-generate Flows.`;
    return;
  }
  state.selectedRunId = run.id;
  const matchedLoop = bestLoopForRun(run);
  if (matchedLoop) {
    state.selectedLoopId = matchedLoop.id;
    state.selectedPhaseId = matchedLoop.steps[0]?.step_id || null;
  }
}

async function boot() {
  /* A ?run= deep link may point at a run newer than the snapshot on
   * disk; scripts/up.sh's generator loop keeps the snapshot fresh, so
   * the page only ever re-reads the artifact. */
  await load();
}

boot();
