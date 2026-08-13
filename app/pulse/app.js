/* Pulse — render pulse.json. Static-only: the page re-reads the
 * artifact; scripts/up.sh keeps it fresh with a generator loop. */

const AUTO_MS = 8000;
const FLOW_URL = `http://${location.hostname}:8002/`;

const $ = (id) => document.getElementById(id);

function fmtDelta(ms, nowMs) {
  if (!ms) return "";
  const d = ms - nowMs;
  const abs = Math.abs(d);
  const units = [[86400000, "d"], [3600000, "h"], [60000, "m"], [1000, "s"]];
  for (const [size, label] of units) {
    if (abs >= size) {
      const v = Math.round(abs / size);
      return d >= 0 ? `in ${v}${label}` : `${v}${label} ago`;
    }
  }
  return d >= 0 ? "now" : "just now";
}

function el(tag, cls, text) {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
}

function pill(text, cls) {
  return el("span", `pill ${cls || ""}`, text);
}

function renderWarnings(data) {
  const box = $("warnings");
  box.replaceChildren();
  for (const w of data.warnings || []) box.appendChild(el("div", "warning", w));
}

function renderHero(data) {
  $("title").textContent = `${data.bot_name} · Pulse`;
  document.title = `${data.bot_name} Pulse`;
  const g = data.gateway || {};
  const chip = $("gateway-chip");
  chip.className = `chip ${g.running ? "up" : "down"}`;
  chip.textContent = g.running
    ? `gateway up · pid ${g.pid} · floop ${g.loop}`
    : "gateway down";
  $("generated-chip").textContent = `generated ${data.generated_at}`;
}

function affairCard(a, data, past) {
  const card = el("div", `card ${a.dormant ? "dormant" : ""} ${past ? "past" : ""}`);
  const row = el("div", "row");
  row.appendChild(el("code", "", a.affair_id));
  const pills = el("span", "");
  pills.appendChild(pill(a.status, a.status));
  if (a.dormant) pills.appendChild(pill("dormant", "dormant"));
  else if (a.status === "active" && a.next_review_at_ms)
    pills.appendChild(pill(`review ${fmtDelta(a.next_review_at_ms, data.now_ms)}`, "active"));
  row.appendChild(pills);
  card.appendChild(row);
  card.appendChild(el("p", "main", a.manage));
  if (!past) for (const n of a.notes || []) card.appendChild(el("div", "note", n));
  return card;
}

function renderAffairs(data) {
  const box = $("affairs");
  box.replaceChildren();
  const affairs = data.affairs || [];
  const active = affairs.filter(a => a.status === "active");
  const past = affairs.filter(a => a.status !== "active").slice(0, 8);
  $("affairs-count").textContent = `${active.length} active`;
  if (!active.length) box.appendChild(el("div", "empty", "No standing concerns."));
  for (const a of active) box.appendChild(affairCard(a, data, false));
  if (past.length) {
    box.appendChild(el("div", "subhead", "Past affairs — history, not active"));
    for (const a of past) box.appendChild(affairCard(a, data, true));
  }
}

function renderWork(data) {
  const box = $("work");
  box.replaceChildren();
  const opsByTask = {};
  for (const o of data.operations || []) opsByTask[o.task_id] = o;
  const work = (data.tasks || []).filter(t => t.kind === "work");
  const runningOps = (data.operations || []).filter(o => o.status === "running");
  $("work-count").textContent =
    `${work.length} task${work.length === 1 ? "" : "s"} · ${runningOps.length} running`;
  if (!work.length && !runningOps.length)
    box.appendChild(el("div", "empty", `No background work. ${data.tasks_archived || 0} archived.`));
  for (const t of work) {
    const op = opsByTask[t.task_id] || (t.external || {});
    const running = (op.status || "") === "running";
    const card = el("div", `card ${running ? "running" : ""}`);
    const row = el("div", "row");
    row.appendChild(el("code", "", t.task_id));
    const pills = el("span", "");
    pills.appendChild(pill(`${t.status} · rev ${t.work_rev}`, t.status));
    if (op.status) pills.appendChild(pill(`worker ${op.status}`, op.status));
    row.appendChild(pills);
    card.appendChild(row);
    card.appendChild(el("p", "main", t.work));
    if (t.done_when) card.appendChild(el("p", "sub", `done when: ${t.done_when}`));
    if (op.handle) {
      const nextPoll = op.next_poll_at_ms
        ? ` · next poll ${fmtDelta(op.next_poll_at_ms, data.now_ms)}` : "";
      card.appendChild(el("p", "sub", `attempt ${op.handle} (rev ${op.work_rev})${nextPoll}`));
    }
    if ((t.external || {}).result_excerpt)
      card.appendChild(el("p", "sub", `result: ${t.external.result_excerpt}`));
    box.appendChild(card);
  }
}

function renderRuns(data) {
  const box = $("runs");
  box.replaceChildren();
  const runs = data.runs || [];
  const failed = runs.filter(r => r.outcome === "run_failed").length;
  $("runs-count").textContent = `${runs.length} recent${failed ? ` · ${failed} failed` : ""}`;
  if (!runs.length) box.appendChild(el("div", "empty", "No runs yet."));
  for (const r of runs) {
    const card = el("div", `card ${r.outcome === "run_failed" ? "failed" : ""}`);
    const row = el("div", "row");
    const link = el("a", "run-link");
    link.href = `${FLOW_URL}?run=${encodeURIComponent(r.run_id)}`;
    link.target = "_blank";
    link.title = "Open in Runtime Flow analyzer";
    link.appendChild(el("code", "", r.run_id));
    row.appendChild(link);
    const pills = el("span", "");
    pills.appendChild(pill(r.origin_kind, ""));
    pills.appendChild(pill(r.outcome.replace("run_", ""), r.outcome));
    row.appendChild(pills);
    card.appendChild(row);
    const bits = [];
    if (r.actions && r.actions.length) bits.push(r.actions.join(", "));
    if (r.reason) bits.push(`reason: ${r.reason}`);
    if (r.start_ms) bits.push(fmtDelta(r.start_ms, data.now_ms));
    if (bits.length) card.appendChild(el("p", "sub", bits.join(" · ")));
    box.appendChild(card);
  }
}

function renderMessages(data) {
  const box = $("messages");
  box.replaceChildren();
  const msgs = data.messages || [];
  const proactive = msgs.filter(m => m.proactive).length;
  $("messages-count").textContent =
    `${msgs.length} recent${proactive ? ` · ${proactive} proactive` : ""}`;
  if (!msgs.length) box.appendChild(el("div", "empty", "No messages yet."));
  for (const m of msgs) {
    const div = el("div",
      `msg ${m.dir} ${m.proactive ? "proactive" : ""} ${m.failure ? "failure" : ""}`);
    div.appendChild(el("span", "when", m.ts_ms ? fmtDelta(m.ts_ms, data.now_ms) : ""));
    let who = m.dir === "out" ? data.bot_name : (m.who || "user");
    if (m.kind === "affair_review") who = "⏰ review";
    if (m.kind === "operation_result") who = "⚙ worker";
    if (m.proactive) who = `★ ${data.bot_name}`;
    div.appendChild(el("span", "who", who));
    div.appendChild(el("span", "text", m.text));
    box.appendChild(div);
  }
  box.scrollTop = box.scrollHeight;
}

function render(data) {
  renderHero(data);
  renderWarnings(data);
  renderAffairs(data);
  renderWork(data);
  renderRuns(data);
  renderMessages(data);
}

/* Regeneration happens outside the page: scripts/up.sh runs the
 * generator on a loop. The page only ever re-reads the artifact. */
async function refresh() {
  try {
    const res = await fetch(`pulse.json?t=${Date.now()}`, { cache: "no-store" });
    render(await res.json());
  } catch (err) {
    $("warnings").replaceChildren(
      el("div", "warning",
         "pulse.json not found — run: python3 app/pulse/generate.py --repo-root ."));
  }
}

$("refresh").addEventListener("click", refresh);
setInterval(() => { if ($("auto").checked) refresh(); }, AUTO_MS);
refresh();
