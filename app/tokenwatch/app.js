/* Tokenwatch renders app-owned calculations from raw tokenwatch.json records. */

const AUTO_MS = 8000;
const state = {
  data: null,
  pricing: null,
  selectedAgent: "",
  selectedCall: "summary",
  summaryRange: {agent: "", start: 0, end: 0, total: 0},
  requestFormat: "pretty",
  inspector: {title: "", note: "", raw: "", boundary: 0},
};
const $ = (id) => document.getElementById(id);
const utf8 = new TextEncoder();

function el(tag, cls, text) {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
}

function fmtInt(value) {
  return Number(value || 0).toLocaleString();
}

function fmtPercent(value) {
  return Number.isFinite(value) ? `${(value * 100).toFixed(1)}%` : "—";
}

function fmtUsd(value) {
  if (value === null || !Number.isFinite(value)) return "not priced";
  if (value === 0) return "$0.00";
  if (value < 0.0001) return `<$0.0001`;
  if (value < 0.01) return `$${value.toFixed(4)}`;
  return `$${value.toFixed(2)}`;
}

function fmtRate(value) {
  if (value === null || !Number.isFinite(Number(value))) return "Not priced";
  return `$${Number(value).toFixed(2)}`;
}

function fmtDate(value) {
  if (!/^\d{4}-\d{2}-\d{2}$/.test(value || "")) return "date not recorded";
  return new Date(`${value}T00:00:00Z`).toLocaleDateString(undefined,
    {year: "numeric", month: "short", day: "numeric", timeZone: "UTC"});
}

function pricingAssumptionsLink(text = "Pricing assumptions →") {
  const link = el("a", "source-link", text);
  link.href = "pricing.html";
  return link;
}

function priceFor(record) {
  return (state.pricing?.models || []).find((row) =>
    row.model === record.model && row.provider === record.provider) || null;
}

function recordCost(record) {
  const price = priceFor(record);
  if (!price || !Number.isFinite(price.input) ||
      !Number.isFinite(price.cached_input) || !Number.isFinite(price.output)) return null;
  const input = Number(record.input_tokens || 0);
  const output = Number(record.output_tokens || 0);
  const reported = record.provider_cached_input_tokens;
  const cached = reported === null || reported === undefined
    ? 0 : Math.min(input, Number(reported || 0));
  const createdReported = record.provider_cache_creation_input_tokens;
  const created = createdReported === null || createdReported === undefined
    ? 0 : Math.min(input - cached, Number(createdReported || 0));
  const createdRate = Number.isFinite(price.cache_creation_input)
    ? price.cache_creation_input : price.input;
  return ((input - cached - created) * price.input +
          cached * price.cached_input + created * createdRate +
          output * price.output) / 1_000_000;
}

function recordsFor(agentId) {
  return (state.data.usage_records || []).filter((record) => record.agent === agentId);
}

function callsFor(agentId) {
  return state.data.cacheview_records?.[agentId] || [];
}

function callKey(call) {
  return `${call.run_id}/${call.call_seq}`;
}

function commonPrefixBytes(strings) {
  if (!strings.length) return 0;
  const bodies = strings.map((value) => utf8.encode(value));
  let end = Math.min(...bodies.map((value) => value.length));
  for (let index = 0; index < end; index += 1) {
    const byte = bodies[0][index];
    if (bodies.some((value) => value[index] !== byte)) return index;
  }
  return end;
}

function observedCache(calls, selected = calls[0]) {
  if (calls.length < 2 || !selected) return null;
  const prefix = commonPrefixBytes(calls.map((call) => call.raw_request || ""));
  const bytes = utf8.encode(selected.raw_request || "").length;
  return {
    prefix,
    percent: bytes ? prefix / bytes : 0,
    tokens: bytes ? Math.round(Number(selected.input_tokens || 0) * prefix / bytes) : 0,
  };
}

function providerCacheStats(records) {
  const reported = records.filter((record) =>
    record.provider_cached_input_tokens !== null &&
    record.provider_cached_input_tokens !== undefined &&
    Number(record.input_tokens) > 0);
  const tokens = reported.reduce((sum, record) =>
    sum + Math.min(Number(record.input_tokens), Number(record.provider_cached_input_tokens || 0)), 0);
  const input = reported.reduce((sum, record) => sum + Number(record.input_tokens), 0);
  return {
    count: reported.length,
    tokens,
    input,
    percent: input ? tokens / input : null,
  };
}

function summarize(records, calls) {
  const provider = providerCacheStats(records);
  const knownCosts = records.map(recordCost).filter((value) => value !== null);
  return {
    records,
    calls,
    callsCount: records.length,
    input: records.reduce((sum, record) => sum + Number(record.input_tokens || 0), 0),
    output: records.reduce((sum, record) => sum + Number(record.output_tokens || 0), 0),
    reportedTokens: provider.tokens,
    reportedInput: provider.input,
    reportedPercent: provider.percent,
    reportedCount: provider.count,
    observed: observedCache(calls),
    cost: knownCosts.length === records.length ? knownCosts.reduce((a, b) => a + b, 0) : null,
    knownCost: knownCosts.reduce((a, b) => a + b, 0),
    unknownPriceCalls: records.length - knownCosts.length,
  };
}

function aggregate(agent) {
  return summarize(recordsFor(agent.id), callsFor(agent.id));
}

function ensureSummaryRange(agentId, total) {
  const range = state.summaryRange;
  if (range.agent !== agentId) {
    state.summaryRange = {agent: agentId, start: 0, end: total, total};
    return state.summaryRange;
  }
  const followedEnd = range.end === range.total;
  range.total = total;
  range.start = Math.min(Math.max(0, range.start), Math.max(0, total - 1));
  range.end = followedEnd ? total : Math.min(Math.max(range.start + 1, range.end), total);
  if (!total) range.start = range.end = 0;
  return range;
}

function rangedSummary(agent, summary) {
  const range = ensureSummaryRange(agent.id, summary.calls.length);
  const calls = summary.calls.slice(range.start, range.end);
  const fullRange = range.start === 0 && range.end === summary.calls.length;
  const records = fullRange ? summary.records
    : calls.map((call) => summary.records.find((record) => callKey(record) === callKey(call)))
      .filter(Boolean);
  return {...summarize(records, calls), range};
}

function metric(label, value, note = "", kind = "") {
  const card = el("div", `metric ${kind}`);
  card.append(el("span", "metric-label", label), el("strong", "", value));
  if (note) card.appendChild(el("small", "", note));
  return card;
}

function providerMetric(summary) {
  if (summary.reportedCount) {
    return metric("Provider cached input", fmtPercent(summary.reportedPercent),
      `${fmtInt(summary.reportedTokens)} tokens · ${summary.reportedCount}/${summary.callsCount} calls included a count`, "provider");
  }
  return metric("Provider cached input", "not reported",
    summary.callsCount ? "provider responses included no cache count" : "no calls in this segment", "provider");
}

function observedMetric(summary) {
  return metric("FloofClaw stable region",
    summary.observed ? fmtPercent(summary.observed.percent) : "not enough data",
    summary.observed ? `≈${fmtInt(summary.observed.tokens)} input tokens` : "needs 2 recorded requests",
    "observed");
}

function renderHero() {
  const scope = state.data.scope || {};
  $("scope-chip").textContent = `floop ${scope.floop || "unknown"} · current segment`;
  $("generated-chip").textContent = `generated ${new Date(state.data.generated_at).toLocaleString()}`;
  $("warnings").replaceChildren(...(state.data.warnings || []).map((text) => el("div", "warning", text)));
}

function renderTotals(summaries) {
  const known = summaries.reduce((sum, item) => sum + item.knownCost, 0);
  const unknown = summaries.reduce((sum, item) => sum + item.unknownPriceCalls, 0);
  const calls = summaries.reduce((sum, item) => sum + item.callsCount, 0);
  const cacheUnreported = calls - summaries.reduce((sum, item) => sum + item.reportedCount, 0);
  $("total-spend").textContent = fmtUsd(known);
  const notes = [unknown
    ? `Partial estimate · ${unknown} call${unknown === 1 ? " has" : "s have"} no pricing assumption.`
    : "Estimated from Tokenwatch's stored per-million-token assumptions."];
  if (cacheUnreported) notes.push(`${cacheUnreported} unreported cache call${cacheUnreported === 1 ? " is" : "s are"} priced as uncached.`);
  const spendNote = $("spend-note");
  spendNote.replaceChildren(document.createTextNode(`${notes.join(" ")} `),
    pricingAssumptionsLink());
  const input = summaries.reduce((sum, item) => sum + item.input, 0);
  const output = summaries.reduce((sum, item) => sum + item.output, 0);
  const reported = summaries.reduce((sum, item) => sum + item.reportedTokens, 0);
  const reportedInput = summaries.reduce((sum, item) => sum + item.reportedInput, 0);
  const reportedCalls = summaries.reduce((sum, item) => sum + item.reportedCount, 0);
  $("totals").replaceChildren(
    metric("Calls", fmtInt(calls)),
    metric("Input", fmtInt(input), "tokens"),
    metric("Output", fmtInt(output), "tokens"),
    reportedCalls
      ? metric("Provider cached input", fmtPercent(reportedInput ? reported / reportedInput : null),
          `${fmtInt(reported)} cached tokens · ${reportedCalls}/${calls} calls included a cache count`, "provider")
      : metric("Provider cached input", "not reported",
          calls ? "provider responses included no cache count" : "no calls in this segment", "provider"),
  );
}

function agentCard(agent, summary) {
  const button = el("button", `agent-card ${state.selectedAgent === agent.id ? "active" : ""}`);
  button.type = "button";
  button.dataset.agent = agent.id;
  const head = el("div", "agent-head");
  const name = el("div");
  name.append(el("strong", "", agent.id),
    el("small", "", `${agent.provider || "provider?"} · ${agent.model || agent.profile || "model?"}`));
  head.append(name, el("span", "agent-cost",
    summary.cost === null && summary.callsCount ? "not priced" : fmtUsd(summary.cost || 0)));
  button.appendChild(head);
  const stats = el("div", "agent-stats");
  stats.append(
    metric("Calls", fmtInt(summary.callsCount)),
    metric("Input / output", `${fmtInt(summary.input)} / ${fmtInt(summary.output)}`),
    providerMetric(summary),
    observedMetric(summary),
  );
  button.appendChild(stats);
  button.addEventListener("click", () => {
    state.selectedAgent = agent.id;
    state.selectedCall = "summary";
    renderAll();
  });
  return button;
}

function renderAgents(summaries) {
  $("agent-count").textContent = `${summaries.length} LLM agent${summaries.length === 1 ? "" : "s"}`;
  const list = $("agents");
  list.replaceChildren();
  if (!summaries.length) list.appendChild(el("div", "empty", "No LLM agents are configured in this floop."));
  summaries.forEach((summary) => list.appendChild(agentCard(summary.agent, summary)));
}

function emptyChart(target, text) {
  $(target).replaceChildren(el("div", "chart-empty", text));
}

function horizontalBar(kind, value, max, cached = null) {
  const track = el("div", `h-bar-track ${kind}`);
  const fill = el("span", `h-bar-fill ${kind}`);
  fill.style.width = `${value > 0 ? Math.max(1, value / max * 100) : 0}%`;
  track.appendChild(fill);
  if (cached !== null) {
    const overlay = el("span", "h-bar-fill cached");
    overlay.style.width = `${cached > 0 ? Math.max(1, cached / max * 100) : 0}%`;
    track.appendChild(overlay);
  }
  return track;
}

function renderTokenChart(records) {
  if (!records.length) return emptyChart("token-chart", "No calls in this segment.");
  const max = Math.max(1, ...records.map((record) =>
    Math.max(Number(record.input_tokens || 0), Number(record.output_tokens || 0))));
  const chart = el("div", "h-chart");
  chart.setAttribute("role", "img");
  chart.setAttribute("aria-label", "Input, output, and provider-cached tokens by call");
  records.forEach((record, index) => {
    const input = Number(record.input_tokens || 0);
    const output = Number(record.output_tokens || 0);
    const reported = record.provider_cached_input_tokens;
    const cached = reported === null || reported === undefined
      ? null : Math.min(input, Number(reported || 0));
    const row = el("div", "h-chart-row");
    row.appendChild(el("strong", "h-call-number", String(index + 1)));
    const body = el("div", "h-chart-body");
    body.append(horizontalBar("input", input, max, cached), horizontalBar("output", output, max));
    const values = el("small", "chart-values");
    values.append(
      el("span", "input-value", `red ${fmtInt(input)} input`),
      el("span", "output-value", `purple ${fmtInt(output)} output`),
      el("span", "cache-value", cached === null ? "blue not reported" : `blue ${fmtInt(cached)} cached`),
    );
    body.appendChild(values);
    row.appendChild(body);
    chart.appendChild(row);
  });
  chart.appendChild(el("small", "chart-footnote",
    "Red = input · purple = output · blue overlay = provider-reported cached input"));
  $("token-chart").replaceChildren(chart);
}

function renderCostChart(records) {
  if (!records.length) return emptyChart("cost-chart", "No calls in this segment.");
  const costs = records.map(recordCost);
  if (costs.every((cost) => cost === null)) return emptyChart("cost-chart", "No matching pricing assumptions.");
  const max = Math.max(0.000001, ...costs.filter((cost) => cost !== null));
  const chart = el("div", "h-chart cost-chart");
  chart.setAttribute("role", "img");
  chart.setAttribute("aria-label", "Estimated cost by call");
  costs.forEach((cost, index) => {
    const row = el("div", "h-chart-row cost-row");
    row.appendChild(el("strong", "cost-call-label", `${index + 1} — ${fmtUsd(cost)}`));
    const body = el("div", "h-chart-body");
    if (cost === null) {
      body.appendChild(el("div", "h-bar-track cost unknown"));
    } else {
      body.appendChild(horizontalBar("cost", cost, max));
    }
    row.appendChild(body);
    chart.appendChild(row);
  });
  chart.appendChild(el("small", "chart-footnote",
    "Estimated USD · calls without provider cache evidence are priced as uncached"));
  $("cost-chart").replaceChildren(chart);
}

function selectedCall(calls) {
  return calls.find((call) => `${call.run_id}/${call.call_seq}` === state.selectedCall) || calls[0] || null;
}

function sourceMapForRaw(text) {
  const map = new Array(text.length);
  let byte = 0;
  for (let index = 0; index < text.length;) {
    const codePoint = text.codePointAt(index);
    const units = codePoint > 0xffff ? 2 : 1;
    const end = byte + utf8.encode(text.slice(index, index + units)).length;
    for (let unit = 0; unit < units; unit += 1) map[index + unit] = {start: byte, end};
    byte = end;
    index += units;
  }
  return map;
}

function mappedRange(map, start, end) {
  if (!map.length) return {start: 0, end: 0};
  const first = map[Math.min(start, map.length - 1)];
  const last = map[Math.min(Math.max(start, end - 1), map.length - 1)];
  return {start: first.start, end: last.end};
}

function appendChunk(out, text, range, layout = false) {
  if (text) out.push({text, start: range.start, end: range.end, layout});
}

function appendLayout(out, indent, map, anchor) {
  const range = mappedRange(map, anchor, anchor + 1);
  appendChunk(out, `\n${"  ".repeat(Math.max(0, indent))}`, range, true);
}

function trimLayout(out) {
  while (out.length && out[out.length - 1].layout) out.pop();
}

function stringEnd(text, start) {
  let escaped = false;
  for (let index = start + 1; index < text.length; index += 1) {
    if (escaped) escaped = false;
    else if (text[index] === "\\") escaped = true;
    else if (text[index] === "\"") return index;
  }
  return text.length - 1;
}

function decodeStringToken(text, start, end, map) {
  let decoded = "";
  const decodedMap = [];
  for (let index = start + 1; index < end;) {
    let consumed = 1;
    let value = text[index];
    if (text[index] === "\\" && index + 1 < end) {
      consumed = text[index + 1] === "u" ? 6 : 2;
      if (consumed === 6 && index + 12 <= end && text.slice(index + 6, index + 8) === "\\u") {
        const high = Number.parseInt(text.slice(index + 2, index + 6), 16);
        const low = Number.parseInt(text.slice(index + 8, index + 12), 16);
        if (high >= 0xd800 && high <= 0xdbff && low >= 0xdc00 && low <= 0xdfff) consumed = 12;
      }
      try { value = JSON.parse(`"${text.slice(index, index + consumed)}"`); }
      catch (_) { value = text.slice(index, index + consumed); }
    }
    const range = mappedRange(map, index, Math.min(end, index + consumed));
    decoded += value;
    for (let unit = 0; unit < value.length; unit += 1) decodedMap.push(range);
    index += consumed;
  }
  return {text: decoded, map: decodedMap};
}

function nestedJson(decoded) {
  let start = 0, end = decoded.text.length;
  while (start < end && /\s/.test(decoded.text[start])) start += 1;
  while (end > start && /\s/.test(decoded.text[end - 1])) end -= 1;
  const text = decoded.text.slice(start, end);
  if (!text || !"[{".includes(text[0])) return null;
  try { JSON.parse(text); }
  catch (_) { return null; }
  return {text, map: decoded.map.slice(start, end)};
}

function appendDecoded(out, decoded) {
  for (let index = 0; index < decoded.text.length; index += 1) {
    appendChunk(out, decoded.text[index], decoded.map[index]);
  }
}

function prettyProjection(text, map = sourceMapForRaw(text), baseIndent = 0, depth = 0) {
  const out = [];
  let indent = baseIndent;
  for (let index = 0; index < text.length;) {
    const char = text[index];
    if (/\s/.test(char)) { index += 1; continue; }
    const range = mappedRange(map, index, index + 1);
    if (char === "\"") {
      const end = stringEnd(text, index);
      const decoded = decodeStringToken(text, index, end, map);
      let next = end + 1;
      while (next < text.length && /\s/.test(text[next])) next += 1;
      const isKey = text[next] === ":";
      if (isKey) {
        appendChunk(out, JSON.stringify(decoded.text), mappedRange(map, index, end + 1));
      } else {
        const nested = depth < 4 ? nestedJson(decoded) : null;
        if (nested) {
          appendLayout(out, indent, map, index);
          out.push(...prettyProjection(nested.text, nested.map, indent, depth + 1));
        } else if (decoded.text.includes("\n") || decoded.text.length > 80) {
          appendLayout(out, indent, map, index);
          appendChunk(out, "```text\n", mappedRange(map, index, index + 1));
          appendDecoded(out, decoded);
          appendChunk(out, "\n```", mappedRange(map, end, end + 1));
        } else {
          appendChunk(out, "\"", mappedRange(map, index, index + 1));
          appendDecoded(out, decoded);
          appendChunk(out, "\"", mappedRange(map, end, end + 1));
        }
      }
      index = end + 1;
      continue;
    }
    if (char === "{" || char === "[") {
      appendChunk(out, char, range);
      indent += 1;
      let next = index + 1;
      while (next < text.length && /\s/.test(text[next])) next += 1;
      if (!((char === "{" && text[next] === "}") || (char === "[" && text[next] === "]")))
        appendLayout(out, indent, map, index);
      index += 1;
      continue;
    }
    if (char === "}" || char === "]") {
      trimLayout(out);
      indent = Math.max(baseIndent, indent - 1);
      appendLayout(out, indent, map, index);
      appendChunk(out, char, range);
      index += 1;
      continue;
    }
    if (char === ",") {
      appendChunk(out, char, range);
      appendLayout(out, indent, map, index);
      index += 1;
      continue;
    }
    if (char === ":") {
      appendChunk(out, ": ", range);
      index += 1;
      continue;
    }
    let end = index + 1;
    while (end < text.length && !/[\s,\]}]/.test(text[end])) end += 1;
    appendChunk(out, text.slice(index, end), mappedRange(map, index, end));
    index = end;
  }
  trimLayout(out);
  return out;
}

function rawProjection(text) {
  const map = sourceMapForRaw(text);
  const out = [];
  for (let index = 0; index < text.length; index += 1)
    appendChunk(out, text[index], map[index]);
  return out;
}

function renderProjection(target, chunks, boundary) {
  let cached = "", after = "";
  chunks.forEach((chunk) => {
    if (chunk.end <= boundary) cached += chunk.text;
    else after += chunk.text;
  });
  const before = el("span", "request-cached", cached);
  const marker = el("span", "cache-boundary-marker", `CACHE ENDS HERE · RAW BYTE ${fmtInt(boundary)}`);
  const remainder = el("span", "request-after-cache", after);
  target.replaceChildren(before, marker, remainder);
}

function renderInspector() {
  if (!$("request-content")) return;
  $("request-content-title").textContent = state.inspector.title;
  $("request-content-note").textContent = state.inspector.note;
  const chunks = state.requestFormat === "raw"
    ? rawProjection(state.inspector.raw) : prettyProjection(state.inspector.raw);
  renderProjection($("request-content"), chunks, state.inspector.boundary);
  document.querySelectorAll("[data-request-format]").forEach((button) => {
    button.classList.toggle("active", button.dataset.requestFormat === state.requestFormat);
    button.setAttribute("aria-pressed", String(button.dataset.requestFormat === state.requestFormat));
  });
}

function setInspector(title, note, raw, boundary) {
  state.inspector = {title, note, raw, boundary};
  renderInspector();
}

function addBoundary(band, kind, percent, label) {
  const line = el("div", `boundary ${kind}-boundary`);
  line.style.left = `${Math.min(100, Math.max(0, percent * 100))}%`;
  line.classList.add(percent > 0.55 ? "label-left" : "label-right");
  line.appendChild(el("span", "", label));
  band.appendChild(line);
}

function renderHeatmap(calls) {
  const box = $("heatmap");
  box.replaceChildren();
  const summaryMode = state.selectedCall === "summary";
  const selected = summaryMode ? calls[0] || null : selectedCall(calls);
  if (!selected) {
    box.appendChild(el("div", "chart-empty", "No raw provider requests for this agent."));
    $("heat-caption").textContent = "needs recorded requests";
    $("heat-legend").replaceChildren();
    setInspector("What is cached", "No raw requests are available.", "", 0);
    return;
  }
  const baseline = selected.raw_request || "";
  const baselineBytes = utf8.encode(baseline);
  const comparisons = summaryMode ? calls : calls.filter((call) => call !== selected);
  const comparisonBytes = comparisons.map((call) => utf8.encode(call.raw_request || ""));
  const canCompare = calls.length > 1;
  const bucketCount = Math.min(48, Math.max(1, baselineBytes.length));
  const band = el("div", "heat-band");
  for (let bucket = 0; bucket < bucketCount; bucket += 1) {
    const start = Math.floor(baselineBytes.length * bucket / bucketCount);
    const end = Math.floor(baselineBytes.length * (bucket + 1) / bucketCount);
    let matches = 0;
    const possible = Math.max(1, (end - start) * Math.max(1, comparisons.length));
    comparisonBytes.forEach((request) => {
      for (let index = start; index < end; index += 1) {
        if (request[index] === baselineBytes[index]) matches += 1;
      }
    });
    const ratio = canCompare ? matches / possible : null;
    const cell = el("span", `heat-cell ${ratio === null ? "heat-unknown" : ""}`);
    if (ratio !== null) cell.style.setProperty("--heat", String(ratio));
    cell.title = ratio === null
      ? `bytes ${start}–${end}: comparison unavailable`
      : `bytes ${start}–${end}: ${Math.round(ratio * 100)}% match`;
    band.appendChild(cell);
  }
  const observed = observedCache(calls, selected);
  if (observed) addBoundary(band, "observed", observed.percent,
    `FloofClaw ${fmtPercent(observed.percent)} · ≈${fmtInt(observed.tokens)} tok`);

  let providerPercent = null;
  let providerLabel = "provider cache not reported";
  if (summaryMode) {
    const provider = providerCacheStats(calls);
    providerPercent = provider.percent;
    if (provider.count) providerLabel = `provider ${fmtPercent(provider.percent)} · ${provider.count}/${calls.length} calls reported`;
  } else if (selected.provider_cached_input_tokens !== null &&
             selected.provider_cached_input_tokens !== undefined && Number(selected.input_tokens) > 0) {
    providerPercent = Math.min(1, Number(selected.provider_cached_input_tokens) / Number(selected.input_tokens));
    providerLabel = `provider ${fmtPercent(providerPercent)} · ${fmtInt(selected.provider_cached_input_tokens)} tok`;
  }
  if (providerPercent !== null) addBoundary(band, "provider", providerPercent, providerLabel);
  box.appendChild(band);

  if (summaryMode) {
    const provider = providerCacheStats(calls);
    const shares = [observed ? `FloofClaw stable ${fmtPercent(observed.percent)}` : "needs 2 requests"];
    shares.push(provider.count
      ? `provider cached ${fmtPercent(provider.percent)} on ${provider.count}/${calls.length} reporting calls`
      : "provider cache not reported by these calls");
    $("heat-caption").textContent = `Summary · ${calls.length} request${calls.length === 1 ? "" : "s"} · baseline ${selected.run_id}/${String(selected.call_seq).padStart(3, "0")} · ${fmtInt(baselineBytes.length)} bytes · ${shares.join(" · ")}`;
    const prefix = observed ? observed.prefix : 0;
    setInspector("What is cached",
      observed
        ? `Cached bytes are highlighted; everything after the exact shared boundary is dimmed · ${fmtInt(prefix)} raw bytes · ${fmtPercent(observed.percent)} of the baseline`
        : "At least 2 requests are required to identify a shared beginning.",
      baseline, prefix);
  } else {
    const shares = [];
    if (observed) shares.push(`FloofClaw stable ${fmtPercent(observed.percent)}`);
    shares.push(providerPercent === null
      ? "provider cache not reported for this call"
      : `provider cached ${fmtPercent(providerPercent)}`);
    $("heat-caption").textContent = `${selected.run_id}/${String(selected.call_seq).padStart(3, "0")} · ${fmtInt(baselineBytes.length)} bytes · compared with ${comparisons.length} · ${shares.join(" · ")}`;
    const prefix = observed ? observed.prefix : commonPrefixBytes(calls.map((call) => call.raw_request || ""));
    setInspector("Selected request",
      `Shared bytes 0–${fmtInt(prefix)} are highlighted; this request's divergent remainder is dimmed`, baseline, prefix);
  }
  $("heat-legend").replaceChildren(
    el("span", "", "teal = stable"),
    el("span", "", "orange = partial"),
    el("span", "", "red = divergent"),
    el("span", "provider-key", "blue line = provider cached share"),
    el("span", "observed-key", "green line = FloofClaw stable share"),
  );
}

function formatCall(call) {
  return call ? `${call.run_id}/${String(call.call_seq).padStart(3, "0")}` : "—";
}

function renderSummaryRange(agent, summary) {
  const panel = $("summary-range");
  panel.hidden = state.selectedCall !== "summary";
  const range = ensureSummaryRange(agent.id, summary.calls.length);
  const start = $("range-start");
  const end = $("range-end");
  const total = summary.calls.length;
  start.min = "0";
  start.max = String(Math.max(0, total - 1));
  start.value = String(range.start);
  start.disabled = total < 2;
  end.min = total ? "1" : "0";
  end.max = String(total);
  end.value = String(range.end);
  end.disabled = total < 2;
  $("range-start-value").textContent = String(range.start);
  $("range-end-value").textContent = String(range.end);
  $("range-value").textContent = `${range.start} → ${range.end}`;

  const calls = summary.calls.slice(range.start, range.end);
  const first = calls[0];
  const last = calls[calls.length - 1];
  $("range-runs").textContent = calls.length
    ? `${formatCall(first)} → ${formatCall(last)} · ${calls.length} of ${total} requests`
    : "No recorded requests";
  start.setAttribute("aria-valuetext", first ? `Beginning at ${formatCall(first)}` : "No requests");
  end.setAttribute("aria-valuetext", last ? `Ending before boundary ${range.end}, after ${formatCall(last)}` : "No requests");
}

function renderDetail(agent, summary) {
  $("detail-title").textContent = agent ? agent.id : "Agent drilldown";
  if (!agent) {
    $("detail-subtitle").textContent = "Select an agent.";
    $("detail-metrics").replaceChildren();
    $("summary-range").hidden = true;
    renderTokenChart([]); renderCostChart([]); renderHeatmap([]);
    return;
  }
  const selector = $("run-select");
  selector.replaceChildren();
  const range = ensureSummaryRange(agent.id, summary.calls.length);
  const rangeCount = Math.max(0, range.end - range.start);
  const fullRange = range.start === 0 && range.end === summary.calls.length;
  const summaryLabel = fullRange
    ? `Summary · ${summary.calls.length} request${summary.calls.length === 1 ? "" : "s"}`
    : `Summary · ${range.start}→${range.end} · ${rangeCount}/${summary.calls.length} requests`;
  const summaryOption = el("option", "", summaryLabel);
  summaryOption.value = "summary";
  summaryOption.selected = state.selectedCall === "summary";
  selector.appendChild(summaryOption);
  summary.calls.forEach((call) => {
    const option = el("option", "", `${call.run_id}/${String(call.call_seq).padStart(3, "0")}`);
    option.value = `${call.run_id}/${call.call_seq}`;
    option.selected = option.value === state.selectedCall;
    selector.appendChild(option);
  });
  if (state.selectedCall !== "summary" && !summary.calls.some((call) =>
    `${call.run_id}/${call.call_seq}` === state.selectedCall)) {
    state.selectedCall = "summary";
    selector.value = "summary";
  }
  renderSummaryRange(agent, summary);
  const detail = state.selectedCall === "summary" ? rangedSummary(agent, summary) : summary;
  const price = priceFor(detail.records[0] || summary.records[0] || agent);
  const detailSubtitle = $("detail-subtitle");
  detailSubtitle.replaceChildren(document.createTextNode(
    `${agent.provider || "provider not configured"} · ${agent.model || "model not configured"} · ${price ? "priced" : "no pricing assumption"} · `),
    pricingAssumptionsLink());
  $("detail-metrics").replaceChildren(
    metric("Calls", fmtInt(detail.callsCount)),
    metric("Input / output", `${fmtInt(detail.input)} / ${fmtInt(detail.output)}`),
    providerMetric(detail),
    observedMetric(detail),
    metric("Estimated spend", detail.cost === null && detail.callsCount ? "not priced" : fmtUsd(detail.cost || 0),
      price
        ? (detail.reportedCount < detail.callsCount ? "unreported cache priced uncached"
          : (state.selectedCall === "summary" && !fullRange ? "selected window" : "current segment"))
        : "provider/model pair is not listed"),
  );
  renderTokenChart(detail.records);
  renderCostChart(detail.records);
  renderHeatmap(state.selectedCall === "summary" ? detail.calls : summary.calls);
}

function renderAll() {
  renderHero();
  const summaries = (state.data.agents || []).map((agent) => ({agent, ...aggregate(agent)}));
  if (!summaries.some((item) => item.agent.id === state.selectedAgent)) {
    state.selectedAgent = summaries[0]?.agent.id || "";
    state.selectedCall = "summary";
  }
  renderTotals(summaries);
  renderAgents(summaries);
  const selected = summaries.find((item) => item.agent.id === state.selectedAgent);
  renderDetail(selected?.agent, selected);
}

async function refreshDashboard() {
  try {
    const [dataResponse, priceResponse] = await Promise.all([
      fetch(`tokenwatch.json?t=${Date.now()}`, {cache: "no-store"}),
      fetch(`pricing.json?t=${Date.now()}`, {cache: "no-store"}),
    ]);
    if (!dataResponse.ok || !priceResponse.ok) throw new Error("artifact unavailable");
    state.data = await dataResponse.json();
    state.pricing = await priceResponse.json();
    renderAll();
  } catch (error) {
    $("warnings").replaceChildren(el("div", "warning",
      "Tokenwatch artifacts are unavailable — run: python3 app/tokenwatch/generate.py --repo-root ."));
  }
}

function renderPricingPage(pricing) {
  const rows = [...(pricing.models || [])].sort((a, b) =>
    `${a.provider}/${a.model}`.localeCompare(`${b.provider}/${b.model}`));
  $("pricing-count").textContent = `${rows.length} provider/model pair${rows.length === 1 ? "" : "s"}`;
  $("pricing-updated").textContent = `Last updated ${fmtDate(pricing.last_updated)}`;
  const table = $("pricing-rows");
  table.replaceChildren();
  rows.forEach((row) => {
    const item = el("article", "pricing-row");
    item.append(
      el("div", "pricing-identity", row.provider),
      el("div", "pricing-model", row.model),
      el("div", "pricing-rate", `${fmtRate(row.input)} input`),
      el("div", "pricing-rate", `${fmtRate(row.cached_input)} cached input`),
      el("div", "pricing-rate", `${fmtRate(row.output)} output`),
    );
    const source = el("div", "pricing-source");
    if (/^https:\/\//.test(row.source || "")) {
      const link = el("a", "source-link", "Last known pricing ↗");
      link.href = row.source;
      link.target = "_blank";
      link.rel = "noreferrer noopener";
      source.appendChild(link);
    } else {
      source.appendChild(el("span", "source-unavailable", "No external billing"));
    }
    source.appendChild(el("small", "", `${row.source_label} · checked ${row.checked_at}`));
    item.appendChild(source);
    table.appendChild(item);
  });
}

async function refreshPricingPage() {
  try {
    const response = await fetch(`pricing.json?t=${Date.now()}`, {cache: "no-store"});
    if (!response.ok) throw new Error("pricing unavailable");
    renderPricingPage(await response.json());
  } catch (_) {
    $("pricing-rows").replaceChildren(el("div", "warning", "Pricing assumptions are unavailable."));
  }
}

function setSummaryRange(edge, value) {
  if (!state.data || !state.selectedAgent) return;
  const total = callsFor(state.selectedAgent).length;
  const range = ensureSummaryRange(state.selectedAgent, total);
  if (!total) return;
  if (edge === "start") {
    range.start = Math.min(Math.max(0, Number(value)), total - 1);
    if (range.start >= range.end) range.end = range.start + 1;
  } else {
    range.end = Math.min(Math.max(1, Number(value)), total);
    if (range.end <= range.start) range.start = range.end - 1;
  }
  renderAll();
}

if ($("pricing-rows")) {
  refreshPricingPage();
} else {
  $("run-select").addEventListener("change", (event) => {
    state.selectedCall = event.target.value;
    renderAll();
  });
  $("range-start").addEventListener("input", (event) => setSummaryRange("start", event.target.value));
  $("range-end").addEventListener("input", (event) => setSummaryRange("end", event.target.value));
  document.querySelectorAll("[data-request-format]").forEach((button) => {
    button.addEventListener("click", () => {
      state.requestFormat = button.dataset.requestFormat;
      renderInspector();
    });
  });
  $("refresh").addEventListener("click", refreshDashboard);
  setInterval(() => { if ($("auto").checked) refreshDashboard(); }, AUTO_MS);
  refreshDashboard();
}
