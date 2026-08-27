#include "runtime.h"
#include "support/cli_output.h"
#include "llm/llm.h"
#include "llm/pricing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USAGE_LOG_PATH "workspace/logs/llm_usage.jsonl"
#define USAGE_MAX_GROUPS 64

typedef struct {
  char agent[RT_SMALL];
  char profile[RT_SMALL];
  char provider[RT_SMALL];
  /* Model is part of the grouping key. A profile can change models within
   * one current log segment without attributing earlier calls to the new one. */
  char model[RT_SMALL];
  long long calls;
  long long input_chars;
  long long output_chars;
  long long input_tokens;
  long long output_tokens;
  long long total_tokens;
  long long provider_cached_input_tokens;
  long long provider_cache_reported_calls;
  long long duration_ms;
  double priced_estimated_usd;
  long long priced_calls;
  long long unpriced_calls;
} UsageRow;

static UsageRow *find_or_add(UsageRow *rows, int *n, const char *agent,
                             const char *profile, const char *provider,
                             const char *model) {
  int i;
  for (i = 0; i < *n; ++i) {
    if (strcmp(rows[i].agent, agent) == 0 &&
        strcmp(rows[i].profile, profile) == 0 &&
        strcmp(rows[i].provider, provider) == 0 &&
        strcmp(rows[i].model, model) == 0)
      return &rows[i];
  }
  if (*n >= USAGE_MAX_GROUPS) return NULL;
  memset(&rows[*n], 0, sizeof(rows[0]));
  snprintf(rows[*n].agent, sizeof(rows[*n].agent), "%s", agent);
  snprintf(rows[*n].profile, sizeof(rows[*n].profile), "%s", profile);
  snprintf(rows[*n].provider, sizeof(rows[*n].provider), "%s", provider);
  snprintf(rows[*n].model, sizeof(rows[*n].model), "%s", model);
  return &rows[(*n)++];
}

typedef struct {
  UsageRow rows[USAGE_MAX_GROUPS];
  int count;
  int truncated;
  long long total_calls;
  long long total_input_chars;
  long long total_output_chars;
  long long total_input_tokens;
  long long total_output_tokens;
  long long total_tokens;
  long long total_provider_cached_input_tokens;
  long long total_provider_cache_reported_calls;
  long long total_duration_ms;
  double priced_estimated_usd;
  long long priced_calls;
  long long unpriced_calls;
} UsageAggregate;

/* One complete current-segment aggregation shared by the table, compact
 * model projection, and v1 client JSON renderers. Returns 1 when the segment
 * does not exist yet, 0 on success, and -1 on invalid arguments. */
static int usage_aggregate(const char *want_agent, const char *want_profile,
                           UsageAggregate *out) {
  FILE *ledger;
  char line[RT_LARGE];
  LlmPricingCatalog pricing;
  int pricing_available;
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  pricing_available = llm_pricing_catalog_load(&pricing) == 0;
  ledger = fopen(USAGE_LOG_PATH, "rb");
  if (!ledger) return 1;
  while (fgets(line, sizeof(line), ledger)) {
    size_t len = strlen(line);
    LlmUsageRecord record;
    UsageRow *row;
    double call_usd = 0.0;
    int price_rc;
    if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
      int c;
      while ((c = fgetc(ledger)) != EOF && c != '\n') {}
      continue;
    }
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (llm_usage_record_parse(line, &record) != 0) continue;
    if (want_agent && strcmp(record.agent_id, want_agent) != 0) continue;
    if (want_profile && strcmp(record.profile_id, want_profile) != 0)
      continue;
    out->total_calls++;
    out->total_input_chars += record.input_chars;
    out->total_output_chars += record.output_chars;
    out->total_input_tokens += record.input_tokens;
    out->total_output_tokens += record.output_tokens;
    out->total_tokens += record.total_tokens;
    if (record.provider_cached_input_tokens_reported) {
      out->total_provider_cached_input_tokens +=
          record.provider_cached_input_tokens;
      out->total_provider_cache_reported_calls++;
    }
    out->total_duration_ms += record.duration_ms;
    price_rc = pricing_available
                   ? llm_pricing_usage_usd(
                         &pricing, record.provider_id, record.model,
                         record.input_tokens, record.output_tokens,
                         record.provider_cached_input_tokens,
                         record.provider_cached_input_tokens_reported,
                         &call_usd)
                   : 1;
    if (price_rc == 0) {
      out->priced_estimated_usd += call_usd;
      out->priced_calls++;
    } else {
      out->unpriced_calls++;
    }
    row = find_or_add(out->rows, &out->count, record.agent_id,
                      record.profile_id, record.provider_id, record.model);
    if (!row) {
      out->truncated = 1;
      continue;
    }
    row->calls++;
    row->input_chars += record.input_chars;
    row->output_chars += record.output_chars;
    row->input_tokens += record.input_tokens;
    row->output_tokens += record.output_tokens;
    row->total_tokens += record.total_tokens;
    if (record.provider_cached_input_tokens_reported) {
      row->provider_cached_input_tokens +=
          record.provider_cached_input_tokens;
      row->provider_cache_reported_calls++;
    }
    row->duration_ms += record.duration_ms;
    if (price_rc == 0) {
      row->priced_estimated_usd += call_usd;
      row->priced_calls++;
    } else {
      row->unpriced_calls++;
    }
  }
  fclose(ledger);
  return 0;
}

static int usage_render_client_json(const UsageAggregate *usage,
                                    char *out, size_t out_len);

int rt_usage_main(int argc, char **argv) {
  const char *want_agent = NULL;
  const char *want_profile = NULL;
  UsageAggregate usage;
  int color = 0, agent_readable = 1;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw usage");
    if (mode_rc < 0) {
      fc_cli_print_json_error("usage", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return 1;
    }
    if (mode_rc > 0) {
      agent_readable = !mode.human;
      color = mode.human;
    } else if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) want_agent = argv[++i];
    else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) want_profile = argv[++i];
    else {
      fprintf(stderr,
              "usage:\n"
              "  fclaw usage [-a|-h] [--agent <id>] [--profile <id>]\n");
      if (agent_readable)
        fc_cli_print_json_error("usage", "invalid_arguments",
                                "invalid usage arguments");
      return 1;
    }
  }
  if (usage_aggregate(want_agent, want_profile, &usage) != 0) {
    fprintf(stderr, "usage: %s not found yet (no LLM calls have run)\n", USAGE_LOG_PATH);
    if (agent_readable)
      fc_cli_print_json_error("usage", "usage_unavailable",
                              "no LLM calls have run in the current log segment");
    return 1;
  }
  if (agent_readable) {
    char *json = (char *)malloc(RT_XL);
    int rc;
    if (!json) return 1;
    rc = usage_render_client_json(&usage, json, RT_XL);
    if (rc == 0) fputs(json, stdout);
    free(json);
    return rc == 0 ? 0 : 1;
  }
  printf("scope: current log segment only; rotated history not included\n");
  if (usage.count == 0) {
    printf("(no matching usage records)\n");
    return 0;
  }
  {
    /* Size name columns to their widest value. */
    int aw = (int)strlen("agent"), pw = (int)strlen("profile");
    int vw = (int)strlen("provider");
    int mw = (int)strlen("model");
    const char *hdr = color ? "\x1b[1;36m" : "";
    const char *dim = color ? "\x1b[2m" : "";
    const char *name = color ? "\x1b[32m" : "";
    const char *tot = color ? "\x1b[33m" : "";
    const char *bold = color ? "\x1b[1m" : "";
    const char *off = color ? "\x1b[0m" : "";
    for (int i = 0; i < usage.count; ++i) {
      if ((int)strlen(usage.rows[i].agent) > aw)
        aw = (int)strlen(usage.rows[i].agent);
      if ((int)strlen(usage.rows[i].profile) > pw)
        pw = (int)strlen(usage.rows[i].profile);
      if ((int)strlen(usage.rows[i].provider) > vw)
        vw = (int)strlen(usage.rows[i].provider);
      if ((int)strlen(usage.rows[i].model) > mw)
        mw = (int)strlen(usage.rows[i].model);
    }
    printf("%s%-*s %-*s %-*s %-*s %8s %10s %10s %10s %10s %12s %12s %10s %12s%s\n",
           hdr, aw, "agent", pw, "profile", vw, "provider", mw, "model",
           "calls", "in_tok", "out_tok", "cache_in",
           "tok_total", "in_chars", "out_chars", "ms_total", "est_usd", off);
    for (int i = 0; i < usage.count; ++i) {
      char cached[32];
      char usd[32];
      if (usage.rows[i].provider_cache_reported_calls > 0)
        snprintf(cached, sizeof(cached), "%lld",
                 usage.rows[i].provider_cached_input_tokens);
      else
        snprintf(cached, sizeof(cached), "-");
      if (usage.rows[i].unpriced_calls == 0)
        snprintf(usd, sizeof(usd), "%.8f",
                 usage.rows[i].priced_estimated_usd);
      else
        snprintf(usd, sizeof(usd), "unpriced");
      printf("%s%-*s%s %s%-*s%s %-*s %-*s %8lld %10lld %10lld %10s %s%10lld%s %12lld %12lld %10lld %12s\n",
             name, aw, usage.rows[i].agent, off, dim, pw,
             usage.rows[i].profile, off, vw, usage.rows[i].provider,
             mw, usage.rows[i].model,
             usage.rows[i].calls, usage.rows[i].input_tokens,
             usage.rows[i].output_tokens, cached, tot,
             usage.rows[i].total_tokens,
             off, usage.rows[i].input_chars, usage.rows[i].output_chars,
             usage.rows[i].duration_ms, usd);
    }
    {
      char cached[32];
      char usd[32];
      if (usage.total_provider_cache_reported_calls > 0)
        snprintf(cached, sizeof(cached), "%lld",
                 usage.total_provider_cached_input_tokens);
      else
        snprintf(cached, sizeof(cached), "-");
      if (usage.unpriced_calls == 0)
        snprintf(usd, sizeof(usd), "%.8f", usage.priced_estimated_usd);
      else
        snprintf(usd, sizeof(usd), "unpriced");
      printf("%s%-*s %-*s %-*s %-*s %8lld %10lld %10lld %10s %10lld %12lld %12lld %10lld %12s%s\n",
           bold, aw, "TOTAL", pw, "", vw, "", mw, "",
           usage.total_calls, usage.total_input_tokens,
           usage.total_output_tokens, cached, usage.total_tokens,
           usage.total_input_chars, usage.total_output_chars,
           usage.total_duration_ms, usd, off);
    }
  }
  return 0;
}

/* Compact usage projection for the "usage" listen block: LLM call and
 * token totals per profile plus worker-operation counts, small enough
 * for a model-facing input. Runtime-owned data, projected read-only. */
int rt_usage_projection_json(char *out, size_t out_len) {
  UsageAggregate usage;
  char total_usd[64];
  size_t pos = 0;
  int written;
  if (!out || out_len == 0) return -1;
  if (usage_aggregate(NULL, NULL, &usage) < 0) return -1;
  if (usage.unpriced_calls == 0)
    snprintf(total_usd, sizeof(total_usd), "%.8f",
             usage.priced_estimated_usd);
  else
    snprintf(total_usd, sizeof(total_usd), "null");
  written = snprintf(out, out_len,
                     "{\"llm\":{\"calls\":%lld,\"tokens_in\":%lld,\"tokens_out\":%lld,"
                     "\"provider_cached_tokens_in\":%lld,"
                     "\"estimated_usd\":%s,\"priced_estimated_usd\":%.8f,"
                     "\"priced_calls\":%lld,\"unpriced_calls\":%lld,"
                     "\"pricing_complete\":%s},"
                     "\"by_agent\":[",
                     usage.total_calls, usage.total_input_tokens,
                     usage.total_output_tokens,
                     usage.total_provider_cached_input_tokens, total_usd,
                     usage.priced_estimated_usd, usage.priced_calls,
                     usage.unpriced_calls,
                     usage.unpriced_calls == 0 ? "true" : "false");
  if (written < 0 || (size_t)written >= out_len) return -1;
  pos = (size_t)written;
  for (int i = 0; i < usage.count && i < 6; ++i) {
    char ea[RT_MED], ep[RT_MED], ev[RT_MED], em[RT_MED], row_usd[64];
    if (rt_json_escape(usage.rows[i].agent, ea, sizeof(ea)) != 0 ||
        rt_json_escape(usage.rows[i].profile, ep, sizeof(ep)) != 0 ||
        rt_json_escape(usage.rows[i].provider, ev, sizeof(ev)) != 0 ||
        rt_json_escape(usage.rows[i].model, em, sizeof(em)) != 0) return -1;
    if (usage.rows[i].unpriced_calls == 0)
      snprintf(row_usd, sizeof(row_usd), "%.8f",
               usage.rows[i].priced_estimated_usd);
    else
      snprintf(row_usd, sizeof(row_usd), "null");
    written = snprintf(out + pos, out_len - pos,
                       "%s{\"agent\":\"%s\",\"profile\":\"%s\",\"provider\":\"%s\","
                       "\"model\":\"%s\",\"calls\":%lld,\"tokens_in\":%lld,"
                       "\"tokens_out\":%lld,\"provider_cached_tokens_in\":%lld,"
                       "\"estimated_usd\":%s,\"priced_estimated_usd\":%.8f,"
                       "\"priced_calls\":%lld,\"unpriced_calls\":%lld}",
                       i ? "," : "", ea, ep, ev, em, usage.rows[i].calls,
                       usage.rows[i].input_tokens,
                       usage.rows[i].output_tokens,
                       usage.rows[i].provider_cached_input_tokens, row_usd,
                       usage.rows[i].priced_estimated_usd,
                       usage.rows[i].priced_calls,
                       usage.rows[i].unpriced_calls);
    if (written < 0 || (size_t)written >= out_len - pos) return -1;
    pos += (size_t)written;
  }
  {
    /* Worker operations from the durable store. */
    char handles_unused[1][RT_SMALL];
    size_t running = 0;
    (void)handles_unused;
    {
      char *ops_text = NULL;
      long long total_ops = 0, running_ops = 0;
      if (fs_read_text("workspace/memory/state/operations.json", &ops_text, FS_READ_TEXT_DEFAULT_CAP) == 0 && ops_text) {
        const char *p2 = ops_text;
        while ((p2 = strstr(p2, "\"handle\":")) != NULL) { total_ops++; p2++; }
        p2 = ops_text;
        while ((p2 = strstr(p2, "\"status\":\"running\"")) != NULL) { running_ops++; p2++; }
        free(ops_text);
      }
      written = snprintf(out + pos, out_len - pos,
                         "],\"workers\":{\"operations\":%lld,\"running\":%lld}}",
                         total_ops, running_ops);
    }
    (void)running;
  }
  return (written < 0 || (size_t)written >= out_len - pos) ? -1 : 0;
}

static int usage_render_client_json(const UsageAggregate *usage,
                                    char *out, size_t out_len) {
  size_t pos = 0;
  int written;
  char total_usd[64];
  if (!usage || !out || out_len == 0) return -1;
  if (usage->unpriced_calls == 0)
    snprintf(total_usd, sizeof(total_usd), "%.8f",
             usage->priced_estimated_usd);
  else
    snprintf(total_usd, sizeof(total_usd), "null");
  written = snprintf(
      out, out_len,
      "{\"schema_version\":1,"
      "\"scope\":{\"kind\":\"current_log_segment\","
      "\"path\":\"%s\",\"rotated_history_included\":false},"
      "\"totals\":{\"calls\":%lld,\"input_tokens\":%lld,"
      "\"output_tokens\":%lld,\"total_tokens\":%lld,"
      "\"provider_cached_input_tokens\":%lld,"
      "\"provider_cache_reported_calls\":%lld,"
      "\"input_chars\":%lld,\"output_chars\":%lld,\"duration_ms\":%lld,"
      "\"estimated_usd\":%s,\"priced_estimated_usd\":%.8f,"
      "\"priced_calls\":%lld,\"unpriced_calls\":%lld,"
      "\"pricing_complete\":%s},"
      "\"rows\":[",
      USAGE_LOG_PATH, usage->total_calls, usage->total_input_tokens,
      usage->total_output_tokens, usage->total_tokens,
      usage->total_provider_cached_input_tokens,
      usage->total_provider_cache_reported_calls,
      usage->total_input_chars, usage->total_output_chars,
      usage->total_duration_ms, total_usd, usage->priced_estimated_usd,
      usage->priced_calls, usage->unpriced_calls,
      usage->unpriced_calls == 0 ? "true" : "false");
  if (written < 0 || (size_t)written >= out_len) return -1;
  pos = (size_t)written;
  for (int i = 0; i < usage->count; ++i) {
    char ea[RT_MED], ep[RT_MED], ev[RT_MED], em[RT_MED], cached[64], row_usd[64];
    if (json_escape(usage->rows[i].agent, ea, sizeof(ea)) != 0 ||
        json_escape(usage->rows[i].profile, ep, sizeof(ep)) != 0 ||
        json_escape(usage->rows[i].provider, ev, sizeof(ev)) != 0 ||
        json_escape(usage->rows[i].model, em, sizeof(em)) != 0)
      return -1;
    if (usage->rows[i].provider_cache_reported_calls > 0)
      snprintf(cached, sizeof(cached), "%lld",
               usage->rows[i].provider_cached_input_tokens);
    else
      snprintf(cached, sizeof(cached), "null");
    if (usage->rows[i].unpriced_calls == 0)
      snprintf(row_usd, sizeof(row_usd), "%.8f",
               usage->rows[i].priced_estimated_usd);
    else
      snprintf(row_usd, sizeof(row_usd), "null");
    written = snprintf(
        out + pos, out_len - pos,
        "%s{\"agent\":\"%s\",\"profile\":\"%s\",\"provider\":\"%s\","
        "\"model\":\"%s\","
        "\"calls\":%lld,\"input_tokens\":%lld,\"output_tokens\":%lld,"
        "\"total_tokens\":%lld,\"provider_cached_input_tokens\":%s,"
        "\"provider_cache_reported_calls\":%lld,"
        "\"input_chars\":%lld,\"output_chars\":%lld,"
        "\"duration_ms\":%lld,\"estimated_usd\":%s,"
        "\"priced_estimated_usd\":%.8f,\"priced_calls\":%lld,"
        "\"unpriced_calls\":%lld,\"pricing_complete\":%s}",
        i ? "," : "", ea, ep, ev, em, usage->rows[i].calls,
        usage->rows[i].input_tokens, usage->rows[i].output_tokens,
        usage->rows[i].total_tokens, cached,
        usage->rows[i].provider_cache_reported_calls,
        usage->rows[i].input_chars, usage->rows[i].output_chars,
        usage->rows[i].duration_ms, row_usd,
        usage->rows[i].priced_estimated_usd,
        usage->rows[i].priced_calls, usage->rows[i].unpriced_calls,
        usage->rows[i].unpriced_calls == 0 ? "true" : "false");
    if (written < 0 || (size_t)written >= out_len - pos) return -1;
    pos += (size_t)written;
  }
  written = snprintf(out + pos, out_len - pos,
                     "],\"truncated\":%s}\n",
                     usage->truncated ? "true" : "false");
  return written < 0 || (size_t)written >= out_len - pos ? -1 : 0;
}

int rt_usage_client_json(char *out, size_t out_len) {
  UsageAggregate usage;
  if (!out || out_len == 0) return -1;
  if (usage_aggregate(NULL, NULL, &usage) < 0) return -1;
  return usage_render_client_json(&usage, out, out_len);
}
