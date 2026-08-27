/* Run-state projector. Pure: takes events, mutates RtContext fields
 * (latest_action, latest_result_text, latest_message, user_text). No
 * file I/O on the hot path — events are applied at append time via
 * rt_apply_event_to_context, called from rt_append_event.
 *
 * Durable mutations (memory record types and task events) are routed by
 * rt_append_event when the event type matches. The run-state projector
 * here does not touch them.
 *
 * rt_write_state_snapshot writes the per-run state.json from the
 * current ctx fields. Called at step boundaries by the scheduler /
 * sync CLI loop. No event-log re-read.
 *
 * rt_reduce_event_log_text is the cold path used by `fclaw replay` to
 * compute a final state from a frozen event log text. */

#include "runtime.h"
#include "support/heap_guard.h"

#include <stdlib.h>
#include <string.h>

static void note_action_terminal(RtContext *ctx, const JsonRef *payload, int succeeded) {
  char action[RT_SMALL] = "";
  char text[RT_LARGE] = "";
  char raw[RT_LARGE] = "";
  JsonRef result, error;
  int have_result;
  if (json_ref_object_get_string(payload, "action", action, sizeof(action)) != 0) return;
  snprintf(ctx->latest_action, sizeof(ctx->latest_action), "%s", action);
  have_result = json_ref_object_get_object(payload, "result", &result) == 0;
  if (have_result &&
      (json_ref_object_get_string(&result, "message", text, sizeof(text)) == 0 ||
       json_ref_object_get_string(&result, "content", text, sizeof(text)) == 0 ||
       json_ref_object_get_string(&result, "value", text, sizeof(text)) == 0 ||
       json_ref_object_get_string(&result, "text", text, sizeof(text)) == 0 ||
       json_ref_object_get_string(&result, "path", text, sizeof(text)) == 0)) {
    snprintf(ctx->latest_result_text, sizeof(ctx->latest_result_text), "%s", text);
  } else if (have_result && json_ref_value_copy(&result, raw, sizeof(raw)) == 0 &&
             json_compact(raw, text, sizeof(text)) == 0) {
    snprintf(ctx->latest_result_text, sizeof(ctx->latest_result_text), "%s", text);
  } else if (!succeeded &&
             json_ref_object_get_object(payload, "error", &error) == 0 &&
             json_ref_object_get_string(&error, "message", text, sizeof(text)) == 0) {
    snprintf(ctx->latest_result_text, sizeof(ctx->latest_result_text), "%s", text);
  } else if (!succeeded &&
             json_ref_object_get_string(payload, "message", text, sizeof(text)) == 0) {
    snprintf(ctx->latest_result_text, sizeof(ctx->latest_result_text), "%s", text);
  } else {
    snprintf(ctx->latest_result_text, sizeof(ctx->latest_result_text), "%s",
             succeeded ? "succeeded" : "failed");
  }
  if (succeeded && have_result && strcmp(action, "message") == 0 &&
      json_ref_object_get_string(&result, "message", text, sizeof(text)) == 0) {
    snprintf(ctx->latest_message, sizeof(ctx->latest_message), "%s", text);
  }
}

void rt_context_clear_media_ref(RtContext *ctx) {
  if (!ctx) return;
  fc_xfree(ctx->media_ref_json);
  ctx->media_ref_json = NULL;
}

static int media_ref_matches(const char *compact, const JsonRef *value) {
  size_t len;
  size_t span;
  if (!compact || !value || !value->start || !value->end ||
      value->end < value->start)
    return 0;
  len = strlen(compact);
  span = (size_t)(value->end - value->start);
  return len == span && memcmp(compact, value->start, span) == 0;
}

static void project_user_media_ref(RtContext *ctx, const JsonRef *payload) {
  JsonRef media;
  char *compact;
  if (json_ref_object_get_object(payload, "media", &media) != 0) {
    rt_context_clear_media_ref(ctx);
    return;
  }
  /* Ports emits the already-compact object held by ctx, so the ordinary live
   * append and cold-recovery replay take this no-allocation equality path. */
  if (media_ref_matches(ctx->media_ref_json, &media)) return;
  compact = json_ref_value_compact_dup(&media);
  if (!compact) {
    /* Never retain a stale prior message's media if projection cannot adopt
     * the current event. The intake path reports allocation failure before
     * append, so this is only defensive replay behavior. */
    rt_context_clear_media_ref(ctx);
    return;
  }
  rt_context_clear_media_ref(ctx);
  ctx->media_ref_json = compact;
}

/* Apply a single event to RtContext run-state fields. Pure in-memory.
 * Called once per event by rt_append_event. Idempotent on the type
 * dimension: events whose type isn't a run-state projection are
 * silently ignored. */
void rt_apply_event_to_context(RtContext *ctx, const char *type, const char *payload_json) {
  JsonRef payload;
  if (!ctx || !type || !payload_json) return;
  if (json_ref_first_object(payload_json, &payload) != 0) return;
  if (strcmp(type, "user_message") == 0) {
    (void)json_ref_object_get_string(&payload, "text", ctx->user_text, sizeof(ctx->user_text));
    project_user_media_ref(ctx, &payload);
  } else if (strcmp(type, "action_succeeded") == 0) {
    note_action_terminal(ctx, &payload, 1);
  } else if (strcmp(type, "action_failed") == 0 || strcmp(type, "action_rejected") == 0) {
    note_action_terminal(ctx, &payload, 0);
  } else if (strcmp(type, "memory_recalled") == 0) {
    /* payload_json is already compact (rt_append_events_from_output
     * runs json_compact before append). Drop it straight into the
     * trusted runtime slot consumed by native memory handling. */
    snprintf(ctx->memory_json, sizeof(ctx->memory_json), "%s", payload_json);
  } else if (strcmp(type, "memory_compaction_requested") == 0) {
    ctx->memory_compaction_due = 1;
    snprintf(ctx->memory_compaction_json, sizeof(ctx->memory_compaction_json), "%s", payload_json);
  } else if (strcmp(type, "memory_compacted") == 0) {
    ctx->memory_compaction_due = 0;
    ctx->memory_compaction_json[0] = '\0';
  }
}

/* Write the per-run state.json from ctx fields. The "user" block is
 * always the empty default — durable user model lives in
 * workspace/memory/memory.jsonl. Processor inputs do not expose a
 * global memory root. */
int rt_write_state_snapshot(RtContext *ctx) {
  char eu[RT_LARGE], em[RT_LARGE], es[RT_SMALL], er[RT_LARGE];
  int truncated = 0;
  if (!ctx) return -1;
  truncated |= json_escape(ctx->user_text,          eu, sizeof(eu)) != 0;
  truncated |= json_escape(ctx->latest_message,     em, sizeof(em)) != 0;
  truncated |= json_escape(ctx->latest_action,       es, sizeof(es)) != 0;
  truncated |= json_escape(ctx->latest_result_text, er, sizeof(er)) != 0;
  if (truncated)
    fprintf(stderr,
            "state: run %s: oversized field truncated in state snapshot "
            "(projection only; event log is complete)\n",
            ctx->run_id);
  /* Byte-format matches rt_reduce_event_log_text so smoke_trace_replay's
   * cmp passes (trailing \n before the closing }, etc.). */
  snprintf(ctx->state_json, sizeof(ctx->state_json),
           "{\"user\":{\"facts\":{}}\n,\"run\":{\"user_text\":\"%s\",\"latest_action\":\"%s\","
           "\"latest_result_text\":\"%s\",\"latest_message\":\"%s\"}\n}\n",
           eu, es, er, em);
  {
    char path[PATH_MAX];
    if (rt_state_path(ctx, path, sizeof(path)) != 0) return -1;
    return fs_write_text_atomic(path, ctx->state_json);
  }
}

/* Walk one complete record out of `*cursor`. Oversized records advance as one
 * malformed line instead of being misread as several independent records. */
static int next_line(const char **cursor, char *line, size_t line_len,
                     int *out_oversized, int *out_final,
                     int *out_terminated) {
  const char *p = *cursor;
  const char *newline;
  size_t n;
  if (!p || !*p) return 0;
  newline = strchr(p, '\n');
  n = newline ? (size_t)(newline - p) : strlen(p);
  if (out_oversized) *out_oversized = n + 1U > line_len;
  if (n >= line_len) n = line_len - 1U;
  memcpy(line, p, n);
  line[n] = '\0';
  *cursor = newline ? newline + 1 : p + strlen(p);
  if (out_final) *out_final = **cursor == '\0';
  if (out_terminated) *out_terminated = newline != NULL;
  return 1;
}

/* Cold-path replay: compute a final state JSON from a frozen event-log
 * text. Used by `fclaw replay`. Independent of RtContext; never
 * touches durable memory. */
static int reduce_run_state_text(const char *event_text, char *out_state,
                                 size_t out_len, RtReplayReport *report) {
  char latest_message[RT_LARGE] = "";
  char latest_action[RT_SMALL] = "";
  char latest_result[RT_LARGE] = "";
  char user_text[RT_LARGE] = "";
  char line[RT_XL];
  const char *cursor = event_text ? event_text : "";
  size_t line_number = 0;
  while (*cursor) {
    JsonRef ev, payload;
    char type[RT_SMALL] = "";
    int oversized = 0;
    int final = 0;
    int terminated = 0;
    int malformed;
    if (!next_line(&cursor, line, sizeof(line), &oversized, &final,
                   &terminated))
      break;
    line_number++;
    malformed = !terminated || oversized ||
                json_ref_top_object(line, &ev) != 0 ||
                json_ref_object_get_string(&ev, "type", type,
                                           sizeof(type)) != 0 ||
                json_ref_object_get_object(&ev, "payload", &payload) != 0;
    if (malformed) {
      if (report) {
        report->malformed_lines++;
        if (!final) {
          report->malformed_nonfinal_lines++;
          if (report->first_malformed_nonfinal_line == 0)
            report->first_malformed_nonfinal_line = line_number;
        }
      }
      continue;
    }
    if (strcmp(type, "user_message") == 0) {
      (void)json_ref_object_get_string(&payload, "text", user_text, sizeof(user_text));
    } else if (strcmp(type, "action_succeeded") == 0 ||
               strcmp(type, "action_failed") == 0 ||
               strcmp(type, "action_rejected") == 0) {
      RtContext tmp;
      memset(&tmp, 0, sizeof(tmp));
      note_action_terminal(&tmp, &payload, strcmp(type, "action_succeeded") == 0);
      snprintf(latest_action, sizeof(latest_action), "%s", tmp.latest_action);
      snprintf(latest_result, sizeof(latest_result), "%s", tmp.latest_result_text);
      if (tmp.latest_message[0]) snprintf(latest_message, sizeof(latest_message), "%s", tmp.latest_message);
    }
  }
  {
    char eu[RT_LARGE], em[RT_LARGE], es[RT_SMALL], er[RT_LARGE];
    int truncated = 0;
    truncated |= json_escape(user_text, eu, sizeof(eu)) != 0;
    truncated |= json_escape(latest_message, em, sizeof(em)) != 0;
    truncated |= json_escape(latest_action, es, sizeof(es)) != 0;
    truncated |= json_escape(latest_result, er, sizeof(er)) != 0;
    if (truncated)
      fprintf(stderr, "replay: oversized field truncated in reduced state "
                      "(projection only; event log is complete)\n");
    return snprintf(out_state, out_len,
                    "{\"user_text\":\"%s\",\"latest_action\":\"%s\",\"latest_result_text\":\"%s\",\"latest_message\":\"%s\"}\n",
                    eu, es, er, em) >= (int)out_len ? -1 : 0;
  }
}

int rt_reduce_event_log_text_report(const char *event_text,
                                    char *out_state, size_t out_len,
                                    RtReplayReport *report) {
  char run_state[RT_LARGE * 2];
  if (report) memset(report, 0, sizeof(*report));
  if (reduce_run_state_text(event_text, run_state, sizeof(run_state), report) != 0)
    return -1;
  return snprintf(out_state, out_len, "{\"user\":{\"facts\":{}}\n,\"run\":%s}\n", run_state) >= (int)out_len ? -1 : 0;
}

int rt_reduce_event_log_text(const char *event_text, char *out_state,
                             size_t out_len) {
  RtReplayReport report;
  int rc = rt_reduce_event_log_text_report(event_text, out_state, out_len,
                                           &report);
  if (rc != 0) return rc;
  if (report.malformed_nonfinal_lines > 0) {
    fprintf(stderr,
            "WARNING: replay: malformed non-final line %zu; "
            "event log may be corrupt\n",
            report.first_malformed_nonfinal_line);
    (void)rt_narrate("WARNING: replay: %zu malformed non-final line(s); "
                     "event log may be corrupt",
                     report.malformed_nonfinal_lines);
  }
  if (report.malformed_lines > 0)
    (void)rt_narrate("replay: skipped %zu malformed line(s)",
                     report.malformed_lines);
  return 0;
}
