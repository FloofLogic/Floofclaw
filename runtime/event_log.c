#include "runtime.h"
#include "support/heap_guard.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define REJECTED_EVENTS_LOG "workspace/logs/rejected_events.jsonl"

/* Error emission may be reached from a reducer failure while an event is
 * already being appended. The runtime is single-threaded, so a module-static
 * detector is sufficient: a nested error must never recurse through the
 * event pipeline again. */
static int g_error_append_active;

static int requires_sync_append(const char *type) {
  return type &&
         (strcmp(type, "run_done") == 0 ||
          strcmp(type, "run_failed") == 0 ||
          strcmp(type, "run_canceled") == 0 ||
          strcmp(type, "work_completed") == 0 ||
          strcmp(type, "work_blocked") == 0 ||
          strcmp(type, "work_step_result") == 0 ||
          strcmp(type, "work_outcome") == 0 ||
          strcmp(type, "operation_result") == 0 ||
          strcmp(type, "task_working_memory_appended") == 0);
}

/* Run ids are owned by the runtime owner. Commands and adapters submit bus
 * envelopes; the active RtScheduler creates runs from those envelopes. The
 * scheduler seeds its in-memory counter once at owner startup and this module
 * still atomically claims the final run directory with mkdir. */

static int scan_next_run_number(const char *runs_dir) {
  DIR *d = opendir(runs_dir);
  int max_n = 0;
  int saved_errno;
  struct dirent *e;
  if (!d) return -1;
  errno = 0;
  while ((e = fs_readdir_checked(d)) != NULL) {
    int n = 0;
    if (sscanf(e->d_name, "run_%d", &n) == 1 && n > max_n) max_n = n;
  }
  saved_errno = errno;
  if (closedir(d) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    fprintf(stderr, "event_log: readdir failed for %s: %s\n",
            runs_dir, strerror(saved_errno));
    errno = saved_errno;
    return -1;
  }
  return max_n + 1;
}

static void iso_time(char *out, size_t out_len) {
  struct timeval tv;
  struct tm tmv;
  size_t n;
  if (gettimeofday(&tv, NULL) != 0) { tv.tv_sec = time(NULL); tv.tv_usec = 0; }
  gmtime_r(&tv.tv_sec, &tmv);
  n = strftime(out, out_len, "%Y-%m-%dT%H:%M:%S", &tmv);
  if (n == 0 || n + 5 >= out_len) { if (out_len) out[0] = '\0'; return; }
  snprintf(out + n, out_len - n, ".%03dZ", (int)(tv.tv_usec / 1000));
}

int rt_next_run_number_for_workspace(const char *workspace) {
  char runs_dir[PATH_MAX];
  const char *root = workspace ? workspace : "workspace";
  if (fs_mkdir_p(root) != 0) return -1;
  snprintf(runs_dir, sizeof(runs_dir), "%s/runs", root);
  if (fs_mkdir_p(runs_dir) != 0) return -1;
  return scan_next_run_number(runs_dir);
}

int rt_create_run_owned(RtContext *ctx, const char *workspace, int *next_run_number) {
  char runs_dir[PATH_MAX];
  char tmp[PATH_MAX];
  int n;
  const char *root = workspace ? workspace : "workspace";
  if (!ctx || !next_run_number) return -1;
  memset(ctx, 0, sizeof(*ctx));
  snprintf(ctx->workspace, sizeof(ctx->workspace), "%s", root);
  if (fs_mkdir_p(ctx->workspace) != 0) return -1;
  snprintf(runs_dir, sizeof(runs_dir), "%s/runs", ctx->workspace);
  if (fs_mkdir_p(runs_dir) != 0) return -1;

  n = *next_run_number > 0 ? *next_run_number : scan_next_run_number(runs_dir);
  if (n < 1) return -1;
  for (int attempts = 0; attempts < 1000000; ++attempts, ++n) {
    snprintf(ctx->run_id, sizeof(ctx->run_id), "run_%03d", n);
    snprintf(ctx->run_dir, sizeof(ctx->run_dir), "%s/%s", runs_dir, ctx->run_id);
    if (mkdir(ctx->run_dir, 0755) == 0) {
      *next_run_number = n + 1;
      snprintf(tmp, sizeof(tmp), "%s/agent_outputs", ctx->run_dir);
      if (fs_mkdir_p(tmp) != 0) return -1;
      snprintf(tmp, sizeof(tmp), "%s/action_calls", ctx->run_dir);
      if (fs_mkdir_p(tmp) != 0) return -1;
      ctx->next_event = 1;
      ctx->next_delivery = 1;
      return 0;
    }
    if (errno != EEXIST) return -1;
  }
  return -1;
}

int rt_next_event_id(const RtContext *ctx, char *out, size_t out_len) {
  int n;
  if (!ctx || !out || out_len == 0 || !ctx->run_id[0] ||
      ctx->next_event <= 0)
    return -1;
  n = snprintf(out, out_len, "evt_%s_%06d",
               ctx->run_id, ctx->next_event);
  return n < 0 || (size_t)n >= out_len ? -1 : 0;
}

static int append_event_unchecked(RtContext *ctx, const char *type,
                                  const char *source,
                                  const char *payload_json,
                                  int force_sync) {
  char ts[64];
  char log_path[PATH_MAX];
  char event_id[RT_SMALL];
  const char *body = payload_json ? payload_json : "{}";
  char *line = NULL;
  int rc;
  int work_reducer_failed = 0;
  int n;
  if (!ctx || !type || !source) return -1;
  if (rt_next_event_id(ctx, event_id, sizeof(event_id)) != 0) return -1;
  iso_time(ts, sizeof(ts));
  n = snprintf(NULL, 0,
               "{\"event_id\":\"%s\",\"ts\":\"%s\",\"type\":\"%s\",\"source\":\"%s\",\"run_id\":\"%s\",\"payload\":%s}\n",
               event_id, ts, type, source, ctx->run_id, body);
  if (n < 0) return -1;
  line = (char *)fc_xmalloc((size_t)n + 1U);
  if (!line) return -1;
  snprintf(line, (size_t)n + 1U,
           "{\"event_id\":\"%s\",\"ts\":\"%s\",\"type\":\"%s\",\"source\":\"%s\",\"run_id\":\"%s\",\"payload\":%s}\n",
           event_id, ts, type, source, ctx->run_id, body);
  ctx->next_event++;
  if (rt_event_log_path(ctx, log_path, sizeof(log_path)) != 0) return -1;
  rc = (force_sync || requires_sync_append(type))
           ? fs_append_text_sync(log_path, line)
           : fs_append_text(log_path, line);
  fc_xfree(line);
  if (rc != 0) return rc;

  /* Run-state projection: pure in-memory ctx mutation, every append. */
  rt_apply_event_to_context(ctx, type, body);

  /* Durable side effects owned by their respective subsystems. Skipped
   * on replay so we don't double-apply patches that were already
   * written during the original live run. */
  if (!ctx->replay_mode) {
    if (rt_memory_is_record_type(type)) {
      /* Flatten the envelope into one memory.jsonl line. The runtime
       * has already gated authority in rt_append_events_from_output;
       * a malformed payload (no text) is the only thing that can
       * still fail here, and we make that loud. */
      if (rt_memory_apply(ctx, type, body, ts, ctx->run_id) != 0) {
        char p[RT_LARGE];
        (void)rt_append_event_format(
            ctx, "error", "kernel", p, sizeof(p), 0,
            "{\"message\":\"memory record missing text\","
            "\"type\":\"%s\",\"payload\":%s}", type, body);
      }
    } else if (rt_memory_is_control_type(type)) {
      if (rt_memory_apply_control(type, body) != 0) {
        char p[RT_LARGE];
        (void)rt_append_event_format(
            ctx, "error", "kernel", p, sizeof(p), 0,
            "{\"message\":\"memory control event failed reducer\","
            "\"type\":\"%s\",\"payload\":%s}", type, body);
      }
    } else if (rt_task_is_event_type(type)) {
      if (rt_task_apply_event(type, body) != 0) {
        char p[RT_LARGE];
        (void)rt_append_event_format(
            ctx, "error", "kernel", p, sizeof(p), 0,
            "{\"message\":\"task event failed reducer\","
            "\"type\":\"%s\",\"payload\":%s}", type, body);
      }
    } else if (rt_affair_is_event_type(type)) {
      if (rt_affair_apply_event(type, body) != 0) {
        char p[RT_LARGE];
        (void)rt_append_event_format(
            ctx, "error", "kernel", p, sizeof(p), 0,
            "{\"message\":\"affair event failed reducer\","
            "\"type\":\"%s\",\"payload\":%s}", type, body);
      }
    } else if (rt_operation_is_event_type(type)) {
      if (rt_operation_apply_event(type, body) != 0) {
        char p[RT_LARGE];
        (void)rt_append_event_format(
            ctx, "error", "kernel", p, sizeof(p), 0,
            "{\"message\":\"operation event failed reducer\","
            "\"type\":\"%s\",\"payload\":%s}", type, body);
      }
    }
    if (rt_work_state_is_event_type(type) &&
        rt_work_state_apply_event(ctx, type, body, event_id) != 0) {
      char p[RT_LARGE];
      (void)rt_append_event_format(
          ctx, "error", "kernel", p, sizeof(p), 0,
          "{\"message\":\"work-step event failed reducer\","
          "\"type\":\"%s\",\"payload\":%s}", type, body);
      work_reducer_failed = 1;
    }
  }
  return work_reducer_failed ? -1 : 0;
}

int rt_append_event(RtContext *ctx, const char *type, const char *source,
                    const char *payload_json) {
  int rc;
  if (!type || strcmp(type, "error") != 0)
    return append_event_unchecked(ctx, type, source, payload_json, 0);
  if (g_error_append_active) {
    fprintf(stderr,
            "event_log: recursive error emission dropped (source=%s)\n",
            source ? source : "kernel");
    return -1;
  }
  g_error_append_active = 1;
  rc = append_event_unchecked(ctx, type, source, payload_json, 0);
  if (rc != 0) {
    fprintf(stderr,
            "event_log: error append failed (source=%s, rc=%d)\n",
            source ? source : "kernel", rc);
  }
  g_error_append_active = 0;
  return rc;
}

int rt_append_event_sync(RtContext *ctx, const char *type,
                         const char *source, const char *payload_json) {
  if (!type || strcmp(type, "error") == 0)
    return rt_append_event(ctx, type, source, payload_json);
  return append_event_unchecked(ctx, type, source, payload_json, 1);
}

int rt_emit_error(RtContext *ctx, const char *source, const char *fmt, ...) {
  char msg[RT_LARGE];
  char esc[RT_LARGE];
  char payload[RT_LARGE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (json_escape(msg, esc, sizeof(esc)) != 0) return -1;
  return rt_append_event_format(ctx, "error", source ? source : "kernel",
                                payload, sizeof(payload), 0,
                                "{\"message\":\"%s\"}", esc);
}

/* Error event carrying the run-relative artifact that holds its evidence
 * (e.g. an agent attempt's numbered stderr file). Lets `view -d` resolve
 * the exact artifact instead of guessing among attempts. */
int rt_emit_error_artifact(RtContext *ctx, const char *source,
                           const char *artifact_rel, const char *fmt, ...) {
  char msg[RT_LARGE];
  char esc[RT_LARGE];
  char eart[RT_MED * 2];
  char payload[RT_LARGE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (json_escape(msg, esc, sizeof(esc)) != 0) return -1;
  if (json_escape(artifact_rel ? artifact_rel : "", eart, sizeof(eart)) != 0)
    return -1;
  return rt_append_event_format(ctx, "error", source ? source : "kernel",
                                payload, sizeof(payload), 0,
                                "{\"message\":\"%s\",\"artifact\":\"%s\"}",
                                esc, eart);
}

/* The most recent rejection, kept so a bounded repair can tell the model
 * what was wrong instead of only showing it what it said. Single-threaded
 * reactor: the normalize -> reject -> repair sequence for one decision is
 * synchronous, so one slot is enough. */
static char last_rejected_detail[RT_LARGE];

const char *rt_last_rejected_agent_detail(void) { return last_rejected_detail; }
void rt_clear_rejected_agent_detail(void) { last_rejected_detail[0] = '\0'; }

void rt_log_rejected_agent_event(const char *source, const char *reason, const char *detail) {
  char ts[64], esource[RT_MED], ereason[RT_MED], edetail[RT_LARGE];
  char line[RT_XL];
  time_t now = time(NULL);
  snprintf(last_rejected_detail, sizeof(last_rejected_detail), "%s%s%s",
           reason ? reason : "", reason && detail && *detail ? ": " : "",
           detail ? detail : "");
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  if (json_escape(source ? source : "", esource, sizeof(esource)) != 0) esource[0] = '\0';
  if (json_escape(reason ? reason : "", ereason, sizeof(ereason)) != 0) ereason[0] = '\0';
  if (json_escape(detail ? detail : "", edetail, sizeof(edetail)) != 0) edetail[0] = '\0';
  snprintf(line, sizeof(line),
           "{\"ts\":\"%s\",\"source\":\"%s\",\"reason\":\"%s\",\"detail\":\"%s\"}\n",
           ts, esource, ereason, edetail);
  (void)fs_mkdir_p("workspace/logs");
  (void)fs_append_text(REJECTED_EVENTS_LOG, line);
}

static int safe_event_type(const char *type) {
  if (!type || !*type) return 0;
  for (const char *p = type; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '_')) return 0;
  return 1;
}

static int key_is_runtime_owned(const char *key) {
  return strcmp(key, "event_id") == 0 || strcmp(key, "ts") == 0 ||
         strcmp(key, "run_id") == 0 || strcmp(key, "request_id") == 0 ||
         strcmp(key, "job_id") == 0 || strcmp(key, "source") == 0 ||
         strcmp(key, "created_by") == 0;
}

static int event_has_only_intent_keys(const JsonRef *ev) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef value;
  while (json_ref_object_iter(ev, &cursor, key, sizeof(key), &value) == 1) {
    if (strcmp(key, "type") == 0 || strcmp(key, "payload") == 0) continue;
    return 0;
  }
  return 1;
}

static int payload_has_runtime_owned_key(const JsonRef *payload) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef value;
  if (!payload || payload->type != JSON_REF_OBJECT) return 0;
  while (json_ref_object_iter(payload, &cursor, key, sizeof(key), &value) == 1)
    if (key_is_runtime_owned(key)) return 1;
  return 0;
}

static int append_committed(char *out, size_t out_len, size_t *pos,
                            RtContext *ctx, const char *type, const char *source,
                            int event_seq, const char *payload) {
  char etype[RT_MED], esource[RT_MED], eid[RT_MED], erun[RT_MED];
  char item[RT_XL * 2];
  int n;
  if (!out || out_len == 0) return 0;
  if (json_escape(type, etype, sizeof(etype)) != 0 ||
      json_escape(source, esource, sizeof(esource)) != 0 ||
      json_escape(ctx->run_id, erun, sizeof(erun)) != 0) return -1;
  snprintf(eid, sizeof(eid), "evt_%s_%06d", ctx->run_id, event_seq);
  n = snprintf(item, sizeof(item),
               "%s{\"event_id\":\"%s\",\"type\":\"%s\",\"source\":\"%s\","
               "\"run_id\":\"%s\",\"payload\":%s}",
               *pos > strlen("{\"events\":[") ? "," : "",
               eid, etype, esource, erun, payload ? payload : "{}");
  if (n < 0 || (size_t)n >= sizeof(item)) return -1;
  return rt_append_text(out, out_len, pos, item);
}

static int action_task_reference_ok(const RtContext *ctx, const char *action,
                                    const char *task_id, long long work_rev) {
  char *task_json = NULL;
  const char *context_id = ctx ? ctx->context_id : "";
  int result;
  if (!task_id || !*task_id) return work_rev <= 0;
  /* action_cli is an operator/managed-worker transport with no model-owned
   * context projection. Its optional task id is inherited routing metadata;
   * existence is the narrow authority check for an ordinary (rev=0) call. */
  if (work_rev <= 0 && strncmp(context_id, "action:cli:", 11) == 0)
    return rt_task_reference_exists(task_id);
  if (work_rev > 0) {
    task_json = (char *)fc_xmalloc(RT_XL);
    if (!task_json) return 0;
    result = rt_task_work_binding_json(context_id, task_id, work_rev, 0,
                                       task_json, RT_XL) == 0;
    fc_xfree(task_json);
    return result;
  }
  if (strcmp(action ? action : "", "message") == 0)
    return rt_task_reference_in_context(context_id, task_id);
  if (strcmp(action ? action : "", "work") == 0)
    {
      task_json = (char *)fc_xmalloc(RT_XL);
      if (!task_json) return 0;
      result = rt_task_work_binding_json(context_id, task_id, 0, 1,
                                         task_json, RT_XL) == 0 ||
               rt_task_reference_is_ready_for_context(context_id, task_id);
      fc_xfree(task_json);
      return result;
    }
  return rt_task_reference_is_ready_for_context(context_id, task_id);
}

static int action_work_rev(const JsonRef *payload, long long *out) {
  JsonRef value;
  long long rev = 0;
  if (out) *out = 0;
  if (!payload || json_ref_object_get(payload, "work_rev", &value) != 0)
    return 0;
  if (json_ref_object_get_long(payload, "work_rev", &rev) != 0 || rev <= 0)
    return -1;
  if (out) *out = rev;
  return 1;
}

static int normalize_action_request(RtContext *ctx, const JsonRef *payload,
                                   int event_seq, char *out, size_t out_len) {
  char action[RT_SMALL], eaction[RT_MED], rid[RT_SMALL], erid[RT_MED];
  char task_id[RT_SMALL] = "", etask[RT_MED] = "";
  char work_clause[RT_LARGE] = "";
  long long work_rev = 0;
  JsonRef args_ref;
  int n;
  char *args_json = NULL;
  const char *effective_args = "{}";
  int has_work_rev;
  if (!payload || payload->type != JSON_REF_OBJECT) return -1;
  if (payload_has_runtime_owned_key(payload)) return -1;
  if (json_ref_object_get_string(payload, "action", action, sizeof(action)) != 0 ||
      action[0] == '\0') return -1;
  has_work_rev = action_work_rev(payload, &work_rev);
  if (has_work_rev < 0) return -1;
  {
    JsonRef task_ref;
    if (json_ref_object_get(payload, "task_id", &task_ref) == 0) {
      int ok;
      if (task_ref.type != JSON_REF_STRING ||
          json_ref_string_copy(&task_ref, task_id, sizeof(task_id)) != 0)
        return -1;
      ok = action_task_reference_ok(ctx, action, task_id, work_rev);
      if (!ok || json_escape(task_id, etask, sizeof(etask)) != 0) return -1;
    }
  }
  if (has_work_rev) {
    char trigger_event_id[RT_SMALL] = "";
    if (!task_id[0] ||
        !action_task_reference_ok(ctx, action, task_id, work_rev))
      return -1;
    if (json_ref_object_get_string(payload, "trigger_event_id",
                                   trigger_event_id,
                                   sizeof(trigger_event_id)) == 0 &&
        trigger_event_id[0]) {
      char selected_task[RT_SMALL], trusted_trigger[RT_SMALL];
      char ectx[RT_LARGE], etrigger[RT_MED];
      long long selected_rev = 0;
      if (rt_task_select_work_consequence(
              ctx, selected_task, sizeof(selected_task), &selected_rev,
              trusted_trigger, sizeof(trusted_trigger)) != 0 ||
          strcmp(selected_task, task_id) != 0 || selected_rev != work_rev ||
          strcmp(trusted_trigger, trigger_event_id) != 0 ||
          json_escape(ctx->context_id, ectx, sizeof(ectx)) != 0 ||
          json_escape(trigger_event_id, etrigger, sizeof(etrigger)) != 0)
        return -1;
      snprintf(work_clause, sizeof(work_clause),
               ",\"work_rev\":%lld,\"context_id\":\"%s\","
               "\"trigger_event_id\":\"%s\"",
               work_rev, ectx, etrigger);
    } else {
      snprintf(work_clause, sizeof(work_clause),
               ",\"work_rev\":%lld", work_rev);
    }
  }
  if (json_ref_object_get(payload, "args", &args_ref) == 0) {
    if (args_ref.type != JSON_REF_OBJECT) return -1;
    args_json = json_ref_value_compact_dup(&args_ref);
    if (!args_json) return -1;
    effective_args = args_json;
  }
  snprintf(rid, sizeof(rid), "actionreq_%s_%06d", ctx->run_id, event_seq);
  if (json_escape(rid, erid, sizeof(erid)) != 0 ||
      json_escape(action, eaction, sizeof(eaction)) != 0) {
    fc_xfree(args_json);
    return -1;
  }
  n = snprintf(out, out_len,
               "{\"request_id\":\"%s\",\"action\":\"%s\",\"args\":%s%s%s%s%s}",
               erid, eaction, effective_args,
               task_id[0] ? ",\"task_id\":\"" : "",
               task_id[0] ? etask : "",
               task_id[0] ? "\"" : "", work_clause);
  fc_xfree(args_json);
  return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

static int normalize_generic_payload(const JsonRef *ev, char *out, size_t out_len) {
  JsonRef payload;
  char raw[RT_LARGE];
  if (json_ref_object_get_object(ev, "payload", &payload) != 0) {
    snprintf(out, out_len, "{}");
    return 0;
  }
  if (payload_has_runtime_owned_key(&payload)) return -1;
  if (json_ref_value_copy(&payload, raw, sizeof(raw)) != 0 ||
      json_compact(raw, out, out_len) != 0) return -1;
  return 0;
}

/* A trigger-bearing, revision-bound action request is a semantic controller
 * decision. It must reach durable event truth before the caller mirrors it
 * into the in-memory dispatch queue. Mechanical operation polls and ordinary
 * action calls do not carry trigger_event_id and retain the normal append
 * path. */
static int action_request_is_semantic_selection(const char *payload_json) {
  JsonRef payload;
  char trigger_event_id[RT_SMALL] = "";
  long long work_rev = 0;
  return payload_json &&
         json_ref_top_object(payload_json, &payload) == 0 &&
         json_ref_object_get_long(&payload, "work_rev", &work_rev) == 0 &&
         work_rev > 0 &&
         json_ref_object_get_string(&payload, "trigger_event_id",
                                    trigger_event_id,
                                    sizeof(trigger_event_id)) == 0 &&
         trigger_event_id[0];
}

/* Agent-output validator. Agent events are intent only: an event object
 * may contain only `type` and `payload`. The runtime stamps source,
 * run_id, request_id, event_id, timestamps, and lifecycle outcomes. */
static int validate_agent_event(RtContext *ctx, const JsonRef *ev) {
  char type[RT_SMALL] = "";
  if (!ev || ev->type != JSON_REF_OBJECT) return -1;
  if (!event_has_only_intent_keys(ev)) return -1;
  if (json_ref_object_get_string(ev, "type", type, sizeof(type)) != 0) return -1;
  if (!safe_event_type(type)) return -1;
  if (strcmp(type, "action_started") == 0 || strcmp(type, "action_rejected") == 0 ||
      strcmp(type, "action_succeeded") == 0 || strcmp(type, "action_failed") == 0 ||
      strcmp(type, "action_result") == 0) return -1;
  /* Run lifecycle events come from the kernel, not from agents. */
  if (strcmp(type, "run_started") == 0 || strcmp(type, "run_done") == 0 ||
      strcmp(type, "run_failed") == 0 || strcmp(type, "run_canceled") == 0) return -1;
  if (strcmp(type, "tasks_patch") == 0) return -1;
  if (strcmp(type, "action_request") == 0) {
    JsonRef payload;
    char action[RT_SMALL];
    if (json_ref_object_get_object(ev, "payload", &payload) != 0) return -1;
    if (payload_has_runtime_owned_key(&payload)) return -1;
    if (json_ref_object_get_string(&payload, "action", action, sizeof(action)) != 0 ||
        action[0] == '\0') return -1;
    {
      JsonRef args;
      if (json_ref_object_get(&payload, "args", &args) == 0 && args.type != JSON_REF_OBJECT) return -1;
    }
    {
      JsonRef task_id;
      char id[RT_SMALL];
      long long work_rev = 0;
      int has_task_id = json_ref_object_get(&payload, "task_id", &task_id) == 0;
      int has_work_rev = action_work_rev(&payload, &work_rev);
      if (has_work_rev < 0) return -1;
      if (has_task_id) {
        if (task_id.type != JSON_REF_STRING ||
            json_ref_string_copy(&task_id, id, sizeof(id)) != 0) return -1;
        if (!action_task_reference_ok(ctx, action, id, work_rev)) return -1;
      }
      if (has_work_rev && !has_task_id)
        return -1;
    }
    } else if (rt_task_is_event_type(type)) {
      JsonRef payload;
      char normalized[RT_LARGE];
      char err[RT_MED];
      if (json_ref_object_get_object(ev, "payload", &payload) != 0) return -1;
      if (payload_has_runtime_owned_key(&payload)) return -1;
      if (rt_task_normalize_event(NULL, type, &payload, 1,
                                  normalized, sizeof(normalized),
                                  err, sizeof(err)) != 0) return -1;
  } else if (rt_affair_is_event_type(type)) {
    JsonRef payload;
    if (json_ref_object_get_object(ev, "payload", &payload) != 0) return -1;
    if (payload_has_runtime_owned_key(&payload)) return -1;
    /* Only affair_patch needs the heavy normalize pass (it stamps a
     * runtime-owned affair_id on create). The other affair events
     * (note_added, task_linked, trigger_set, reviewed) are
     * already-shaped — let the reducer validate at apply time. */
    if (strcmp(type, "affair_patch") == 0) {
      char normalized[RT_LARGE];
      if (rt_affair_normalize_event(ctx, &payload, 1,
                                    normalized, sizeof(normalized)) != 0) return -1;
    }
  } else {
    JsonRef payload;
    if (json_ref_object_get_object(ev, "payload", &payload) == 0 &&
        payload_has_runtime_owned_key(&payload)) return -1;
  }
  return 0;
}

int rt_append_events_from_output(RtContext *ctx, const char *source, int source_can_write_memory,
                                 const char *output_json,
                                 char *committed_out, size_t committed_len) {
  JsonRef root, events;
  size_t i, count;
  if (!ctx || !source || !output_json) return -1;
  if (committed_out && committed_len) snprintf(committed_out, committed_len, "{\"events\":[");
  if (json_ref_first_object(output_json, &root) != 0) return -1;
  if (json_ref_object_get_array(&root, "events", &events) != 0) return -1;
  count = json_ref_array_size(&events);
  /* First pass: validate everything. If any event is malformed, append
   * zero. Also enforces memory authority: only agents whose agent.json
   * declares "capabilities":["memory"] may emit a memory record (any
   * of user/asst/fact/summ). The check uses the trusted function arg
   * `source` so an agent can't spoof its identity via the event's
   * own "source" key. */
  for (i = 0; i < count; ++i) {
    JsonRef ev;
    char type[RT_SMALL] = "";
    if (json_ref_array_get(&events, i, &ev) != 0) {
      rt_log_rejected_agent_event(source, "invalid_agent_event", "event array entry unreadable");
      return -1;
    }
    if (validate_agent_event(ctx, &ev) != 0) {
      rt_log_rejected_agent_event(source, "invalid_agent_event", "event failed envelope validation");
      return -1;
    }
    json_ref_object_get_string(&ev, "type", type, sizeof(type));
    if ((rt_memory_is_record_type(type) || rt_memory_is_control_type(type)) &&
        !source_can_write_memory) {
      char p[RT_LARGE];
      (void)rt_append_event_format(
          ctx, "error", "kernel", p, sizeof(p), 0,
          "{\"message\":\"agent %s not authorised to write memory\","
          "\"agent\":\"%s\",\"type\":\"%s\"}", source, source, type);
      return -1;
    }
  }
  /* Second pass: append. */
  size_t committed_pos = committed_out && committed_len ? strlen("{\"events\":[") : 0;
  for (i = 0; i < count; ++i) {
    JsonRef ev, payload_ref;
    char type[RT_SMALL];
    char payload[RT_XL];
    int event_seq = ctx->next_event;
    if (json_ref_array_get(&events, i, &ev) != 0) return -1;
    json_ref_object_get_string(&ev, "type", type, sizeof(type));
    if (strcmp(type, "action_request") == 0) {
      if (json_ref_object_get_object(&ev, "payload", &payload_ref) != 0 ||
          normalize_action_request(ctx, &payload_ref, event_seq, payload, sizeof(payload)) != 0)
        return -1;
    } else if (rt_task_is_event_type(type)) {
      char err[RT_MED];
      if (json_ref_object_get_object(&ev, "payload", &payload_ref) != 0 ||
          rt_task_normalize_event(ctx, type, &payload_ref, event_seq,
                                  payload, sizeof(payload), err, sizeof(err)) != 0)
        return -1;
    } else if (rt_affair_is_event_type(type)) {
      if (json_ref_object_get_object(&ev, "payload", &payload_ref) != 0)
        return -1;
      if (strcmp(type, "affair_patch") == 0) {
        int ar = rt_affair_normalize_event(ctx, &payload_ref, event_seq,
                                           payload, sizeof(payload));
        if (ar > 0) continue;
        if (ar != 0) return -1;
      } else {
        /* Other affair events ship the payload as-is. */
        if (normalize_generic_payload(&ev, payload, sizeof(payload)) != 0) return -1;
      }
    } else {
      if (normalize_generic_payload(&ev, payload, sizeof(payload)) != 0) return -1;
    }
    if (append_committed(committed_out, committed_len, &committed_pos,
                         ctx, type, source, event_seq, payload) != 0) return -1;
    if ((strcmp(type, "action_request") == 0 &&
         action_request_is_semantic_selection(payload)
             ? rt_append_event_sync(ctx, type, source, payload)
             : rt_append_event(ctx, type, source, payload)) != 0)
      return -1;
    /* Affair watcher: when a run that started from an affair_review
     * creates a work task, automatically link it back to the affair. */
    if (strcmp(type, "task_created") == 0 && ctx->inbound_affair_id[0]) {
      JsonRef payload_obj;
      char kind[RT_SMALL] = "";
      if (json_ref_first_object(payload, &payload_obj) == 0)
        (void)json_ref_object_get_string(&payload_obj, "kind", kind, sizeof(kind));
      if (strcmp(kind, "work") == 0) {
        char link_payload[RT_LARGE];
        char eaff[RT_MED], etask[RT_MED];
        char new_task[RT_SMALL];
        snprintf(new_task, sizeof(new_task), "task_%s_%06d", ctx->run_id, event_seq);
        if (json_escape(ctx->inbound_affair_id, eaff, sizeof(eaff)) == 0 &&
            json_escape(new_task, etask, sizeof(etask)) == 0) {
          (void)rt_append_event_format(
              ctx, "affair_task_linked", "kernel", link_payload,
              sizeof(link_payload), 0,
              "{\"affair_id\":\"%s\",\"task_id\":\"%s\"}", eaff, etask);
        }
      }
    }
  }
  if (committed_out && committed_len && rt_append_text(committed_out, committed_len, &committed_pos, "]}") != 0)
    return -1;
  return 0;
}
