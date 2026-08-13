#include "runtime_kernel.h"

#include <stdio.h>
#include <string.h>

const char *rt_run_status_name(RtRunState state) {
  switch (state) {
    case RT_RUN_READY:              return "ready";
    case RT_RUN_RUNNING:            return "running";
    case RT_RUN_WAITING_JOBS:       return "waiting_jobs";
    case RT_RUN_WAITING_EVENT:      return "waiting_event";
    case RT_RUN_WAITING_ACTION_SLOT: return "waiting_event";
    case RT_RUN_DONE:               return "done";
    case RT_RUN_FAILED:             return "failed";
  }
  return "failed";
}

static int run_state_for_status(const char *status, RtRunState *out) {
  if (!status || !*status) return -1;
  for (int value = RT_RUN_READY; value <= RT_RUN_FAILED; ++value) {
    RtRunState state = (RtRunState)value;
    if (strcmp(status, rt_run_status_name(state)) == 0) {
      if (out) *out = state;
      return 0;
    }
  }
  return -1;
}

int rt_run_status_is_valid(const char *status) {
  return run_state_for_status(status, NULL) == 0;
}

int rt_run_status_is_terminal(const char *status) {
  RtRunState state;
  return run_state_for_status(status, &state) == 0 &&
         (state == RT_RUN_DONE || state == RT_RUN_FAILED);
}

static int waiting_jobs_json(RtRun *r, char *out, size_t out_len) {
  size_t pos = 0;
  if (!r || !out || out_len == 0) return -1;
  out[pos++] = '[';
  out[pos] = '\0';
  for (size_t i = 0; i < r->waiting_job_count; ++i) {
    char esc[RT_SMALL * 2];
    int n;
    if (json_escape(r->waiting_job_ids[i], esc, sizeof(esc)) != 0) return -1;
    n = snprintf(out + pos, out_len - pos, "%s\"%s\"", i ? "," : "", esc);
    if (n < 0 || (size_t)n >= out_len - pos) return -1;
    pos += (size_t)n;
  }
  if (pos + 2 > out_len) return -1;
  out[pos++] = ']';
  out[pos] = '\0';
  return 0;
}

static int last_error_json(RtRun *r, char *out, size_t out_len) {
  char code[RT_SMALL * 2], msg[RT_MED * 2], job[RT_SMALL * 2];
  if (!r || !out || out_len == 0) return -1;
  if (!r->last_error_code[0]) {
    snprintf(out, out_len, "null");
    return 0;
  }
  if (json_escape(r->last_error_code, code, sizeof(code)) != 0) return -1;
  if (json_escape(r->last_error_message, msg, sizeof(msg)) != 0) return -1;
  if (json_escape(r->last_error_job_id, job, sizeof(job)) != 0) return -1;
  return snprintf(out, out_len,
                  "{\"class\":%d,\"code\":\"%s\",\"message\":\"%s\"%s%s%s}",
                  r->last_error_class ? (int)r->last_error_class : (int)FC_ERROR_RUNTIME,
                  code, msg,
                  r->last_error_job_id[0] ? ",\"job_id\":\"" : "",
                  r->last_error_job_id[0] ? job : "",
                  r->last_error_job_id[0] ? "\"" : "")
         >= (int)out_len ? -1 : 0;
}

int rt_run_persist_state(RtRun *r) {
  char profile[RT_SMALL * 2], context[RT_MED * 2], event_id[RT_SMALL * 2], type[RT_SMALL * 2];
  char jobs[RT_LARGE], last_error[RT_LARGE], path[PATH_MAX];
  if (!r) return -1;
  if (json_escape(r->profile && r->profile->name[0] ? r->profile->name : "", profile, sizeof(profile)) != 0) return -1;
  if (json_escape(r->context_id, context, sizeof(context)) != 0) return -1;
  if (json_escape(r->created_from_event_id, event_id, sizeof(event_id)) != 0) return -1;
  if (json_escape(r->created_from_type, type, sizeof(type)) != 0) return -1;
  if (waiting_jobs_json(r, jobs, sizeof(jobs)) != 0) return -1;
  if (last_error_json(r, last_error, sizeof(last_error)) != 0) return -1;
  if (snprintf(r->ctx.run_state_json, sizeof(r->ctx.run_state_json),
               "{\"run_id\":\"%s\",\"context_id\":\"%s\",\"profile\":\"%s\","
               "\"status\":\"%s\",\"advance\":%u,"
               "\"created_from\":{\"event_id\":\"%s\",\"type\":\"%s\"},"
               "\"waiting\":{\"jobs\":%s,\"events\":[],\"deadline\":null},"
               "\"cursors\":{\"phase_index\":%zu,\"last_advanced_event_seq\":null},"
               "\"last_error\":%s}",
               r->ctx.run_id, context, profile, rt_run_status_name(r->state), r->advance,
               event_id, type, jobs, r->phase_index, last_error) >= (int)sizeof(r->ctx.run_state_json)) return -1;
  snprintf(path, sizeof(path), "%s/runstate.json", r->ctx.run_dir);
  return fs_write_text_atomic(path, r->ctx.run_state_json);
}

void rt_run_set_error_code(RtRun *r, FcErrorCode class_code,
                           const char *code, const char *message, const char *job_id) {
  if (!r) return;
  r->last_error_class = class_code ? class_code : FC_ERROR_RUNTIME;
  snprintf(r->last_error_code, sizeof(r->last_error_code), "%s",
           code && *code ? code : fc_error_default_code(r->last_error_class));
  snprintf(r->last_error_message, sizeof(r->last_error_message), "%s", message ? message : "");
  snprintf(r->last_error_job_id, sizeof(r->last_error_job_id), "%s", job_id ? job_id : "");
}

void rt_run_set_error(RtRun *r, const char *code, const char *message, const char *job_id) {
  rt_run_set_error_code(r, FC_ERROR_RUNTIME, code, message, job_id);
}

void rt_run_set_state(RtRun *r, RtRunState state) {
  if (!r) return;
  r->state = state;
  (void)rt_run_persist_state(r);
}

int rt_run_waiting_job_index_of(RtRun *r, const char *job_id) {
  if (!r || !job_id || !*job_id) return -1;
  for (size_t i = 0; i < r->waiting_job_count; ++i)
    if (strcmp(r->waiting_job_ids[i], job_id) == 0) return (int)i;
  return -1;
}

void rt_run_waiting_job_add(RtRun *r, const char *job_id) {
  if (!r || !job_id || !*job_id || rt_run_waiting_job_index_of(r, job_id) >= 0) return;
  if (r->waiting_job_count >= RT_MAX_WAITING_JOBS) {
    rt_run_set_error_code(r, FC_ERROR_RUNTIME,
                          "waiting_jobs_overflow", "too many waiting jobs", job_id);
    (void)rt_run_persist_state(r);
    return;
  }
  snprintf(r->waiting_job_ids[r->waiting_job_count++], sizeof(r->waiting_job_ids[0]), "%s", job_id);
  (void)rt_run_persist_state(r);
}

void rt_run_waiting_job_remove(RtRun *r, const char *job_id) {
  int idx;
  if (!r || !job_id || !*job_id) return;
  idx = rt_run_waiting_job_index_of(r, job_id);
  if (idx < 0) return;
  if ((size_t)idx + 1 < r->waiting_job_count) {
    memmove(&r->waiting_job_ids[idx], &r->waiting_job_ids[idx + 1],
            (r->waiting_job_count - (size_t)idx - 1) * sizeof(r->waiting_job_ids[0]));
  }
  r->waiting_job_count--;
  memset(&r->waiting_job_ids[r->waiting_job_count], 0, sizeof(r->waiting_job_ids[0]));
}
