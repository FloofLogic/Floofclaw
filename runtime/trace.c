#include "runtime.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>

static char g_pending_narration[RT_LARGE * 2 + 128];

static int append_narration_line(const char *line) {
  if (!line || !*line) return 0;
  if (fs_mkdir_p("workspace/logs") != 0) return -1;
  return fs_append_text("workspace/logs/narration.jsonl", line);
}

int rt_narrate_flush_pending(void) {
  if (!g_pending_narration[0]) return 0;
  if (append_narration_line(g_pending_narration) != 0) return -1;
  g_pending_narration[0] = '\0';
  return 0;
}

static int append_trace_line(RtContext *ctx, const char *line) {
  char path[PATH_MAX];
  if (!ctx || !line) return -1;
  rt_trace_path(ctx, path, sizeof(path));
  return fs_append_text(path, line);
}

int rt_write_trace(RtContext *ctx, const char *phase, const char *detail) {
  char escaped_phase[RT_MED];
  char escaped_detail[RT_LARGE];
  char line[RT_LARGE + 256];
  if (!ctx || !phase) return -1;
  if (rt_json_escape(phase, escaped_phase, sizeof(escaped_phase)) != 0) return -1;
  if (rt_json_escape(detail ? detail : "", escaped_detail, sizeof(escaped_detail)) != 0) return -1;
  snprintf(line, sizeof(line), "{\"phase\":\"%s\",\"detail\":\"%s\"}\n",
           escaped_phase, escaped_detail);
  return append_trace_line(ctx, line);
}

int rt_write_loop_decision_trace(RtContext *ctx, const char *decision,
                                 const char *reason, unsigned int pass) {
  char escaped_decision[RT_SMALL];
  char escaped_reason[RT_SMALL];
  char line[RT_LARGE];
  if (!ctx || !decision || !*decision || !reason || !*reason) return -1;
  if (rt_json_escape(decision, escaped_decision, sizeof(escaped_decision)) != 0) return -1;
  if (rt_json_escape(reason, escaped_reason, sizeof(escaped_reason)) != 0) return -1;
  snprintf(line, sizeof(line),
           "{\"type\":\"loop_decision\",\"pass\":%u,\"decision\":\"%s\",\"reason\":\"%s\"}\n",
           pass, escaped_decision, escaped_reason);
  return append_trace_line(ctx, line);
}

int rt_write_phase_skipped_trace(RtContext *ctx, unsigned int pass,
                                 const char *phase, const char *reason) {
  char escaped_phase[RT_SMALL];
  char escaped_reason[RT_SMALL];
  char line[RT_LARGE];
  if (!ctx || !phase || !*phase || !reason || !*reason) return -1;
  if (rt_json_escape(phase, escaped_phase, sizeof(escaped_phase)) != 0) return -1;
  if (rt_json_escape(reason, escaped_reason, sizeof(escaped_reason)) != 0) return -1;
  snprintf(line, sizeof(line),
           "{\"type\":\"phase_skipped\",\"pass\":%u,\"phase\":\"%s\",\"reason\":\"%s\"}\n",
           pass, escaped_phase, escaped_reason);
  return append_trace_line(ctx, line);
}

/* Narration: one-line human-readable notes about what the runtime is
 * doing, appended to workspace/logs/narration.jsonl. Channel adapters
 * may mirror these into a configured ops channel so a person can watch
 * the bot run its shop. Diagnostics-adjacent: narration is never
 * behavioral truth and losing it changes nothing. */
int rt_narrate(const char *fmt, ...) {
  char text[RT_LARGE];
  char escaped[RT_LARGE * 2];
  char ts[64];
  char line[RT_LARGE * 2 + 128];
  va_list ap;
  time_t now = time(NULL);
  struct tm tmv;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  if (rt_json_escape(text, escaped, sizeof(escaped)) != 0) return -1;
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  snprintf(line, sizeof(line), "{\"ts\":\"%s\",\"text\":\"%s\"}\n", ts, escaped);
  /* Keep the first unsaved operator diagnostic until storage returns. Later
   * narration is best-effort while that bounded slot is occupied. */
  if (g_pending_narration[0] && rt_narrate_flush_pending() != 0) return -1;
  if (append_narration_line(line) == 0) return 0;
  if (!g_pending_narration[0])
    snprintf(g_pending_narration, sizeof(g_pending_narration), "%s", line);
  return -1;
}
