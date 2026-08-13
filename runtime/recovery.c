/* Cold-start recovery for durable nonterminal runs.
 *
 * Scheduler owns run lifecycle, so it owns rehydrating RtRun slots. Ports
 * remains the only layer that decodes processed bus envelopes and the action
 * runner remains the only layer that reconciles unfinished action effects.
 * Event logs are replayed here only into in-memory run control; durable
 * behavioral truth stays in event_log.jsonl. */

#include "run_internal.h"
#include "ports.h"
#include "support/fsutil.h"
#include "support/heap_guard.h"
#include "support/json.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char name[RT_SMALL];
  int number;
} RecoveryCandidate;

static int candidate_compare(const void *a, const void *b) {
  const RecoveryCandidate *left = (const RecoveryCandidate *)a;
  const RecoveryCandidate *right = (const RecoveryCandidate *)b;
  return left->number < right->number ? -1 : left->number > right->number;
}

static int collect_candidates(RecoveryCandidate *out, size_t cap,
                              size_t *out_count, char *err, size_t err_len) {
  DIR *dir = opendir("workspace/runs");
  struct dirent *entry;
  size_t count = 0;
  int saved_errno = 0;
  if (!out || !out_count) return -1;
  *out_count = 0;
  if (!dir) return errno == ENOENT ? 0 : -1;
  for (;;) {
    char path[PATH_MAX];
    char *text = NULL;
    JsonRef root;
    char status[RT_SMALL] = "";
    int number = 0;
    errno = 0;
    entry = fs_readdir_checked(dir);
    if (!entry) {
      saved_errno = errno;
      break;
    }
    if (sscanf(entry->d_name, "run_%d", &number) != 1 || number < 1) continue;
    if (snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json",
                 entry->d_name) >= (int)sizeof(path)) continue;
    if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
      continue;
    if (json_ref_top_object(text, &root) != 0 ||
        json_ref_object_get_string(&root, "status", status, sizeof(status)) != 0) {
      fc_xfree(text);
      if (err) snprintf(err, err_len, "recovery: invalid runstate %s", path);
      (void)closedir(dir);
      return -1;
    }
    fc_xfree(text);
    if (rt_run_status_is_terminal(status)) continue;
    if (count >= cap) {
      if (err) snprintf(err, err_len,
                        "recovery: nonterminal run count exceeds cap=%zu", cap);
      (void)closedir(dir);
      return -1;
    }
    snprintf(out[count].name, sizeof(out[count].name), "%s", entry->d_name);
    out[count].number = number;
    count++;
  }
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    if (err) snprintf(err, err_len, "recovery: runs scan failed: %s",
                      strerror(saved_errno));
    errno = saved_errno;
    return -1;
  }
  if (count > 1) qsort(out, count, sizeof(out[0]), candidate_compare);
  *out_count = count;
  return 0;
}

static int event_sequence(const char *event_id) {
  const char *tail = event_id ? strrchr(event_id, '_') : NULL;
  int value = 0;
  return tail && sscanf(tail + 1, "%d", &value) == 1 && value > 0 ? value : 0;
}

static int replay_action_record(RtRun *r, const char *type,
                                const char *source, const JsonRef *payload) {
  char rid[RT_SMALL] = "";
  char action[RT_SMALL] = "";
  char task_id[RT_SMALL] = "";
  char trigger_event_id[RT_SMALL] = "";
  long long work_rev = 0;
  JsonRef args_ref;
  char *args = NULL;
  int rc = 0;
  if (strcmp(type, "action_request") == 0) {
    if (json_ref_object_get_string(payload, "request_id", rid, sizeof(rid)) != 0 ||
        json_ref_object_get_string(payload, "action", action, sizeof(action)) != 0)
      return -1;
    (void)json_ref_object_get_string(payload, "task_id", task_id, sizeof(task_id));
    (void)json_ref_object_get_long(payload, "work_rev", &work_rev);
    (void)json_ref_object_get_string(payload, "trigger_event_id",
                                     trigger_event_id,
                                     sizeof(trigger_event_id));
    if (json_ref_object_get(payload, "args", &args_ref) == 0)
      args = json_ref_value_compact_dup(&args_ref);
    rc = rt_action_runner_note_request(r, source, rid, action,
                                       args ? args : "{}", task_id, work_rev);
    if (rc == 1)
      rt_action_runner_note_trigger(r, rid, trigger_event_id);
    fc_xfree(args);
    return rc < 0 ? -1 : 0;
  }
  if (strcmp(type, "action_started") == 0) {
    if (json_ref_object_get_string(payload, "request_id", rid, sizeof(rid)) == 0)
      rt_action_runner_note_started(r, rid);
    return 0;
  }
  if (strcmp(type, "action_succeeded") == 0 ||
      strcmp(type, "action_failed") == 0 ||
      strcmp(type, "action_rejected") == 0) {
    if (json_ref_object_get_string(payload, "request_id", rid, sizeof(rid)) == 0)
      rt_action_runner_note_terminal(r, rid);
    if (strcmp(type, "action_succeeded") == 0 &&
        json_ref_object_get_string(payload, "action", action, sizeof(action)) == 0 &&
        strcmp(action, "message") == 0)
      r->advance_delivered_message = 1;
  } else if (strcmp(type, "work_completed") == 0 ||
             strcmp(type, "work_blocked") == 0) {
    /* A mechanical recovery blocker is itself the durable terminal for the
     * selected outside-world request; no synthetic action terminal follows. */
    if (json_ref_object_get_string(payload, "request_id",
                                   rid, sizeof(rid)) == 0)
      rt_action_runner_note_terminal(r, rid);
  }
  return 0;
}

static int replay_run_log(RtRun *r, int *out_inbound, int *out_terminal,
                          RtRunState *out_terminal_state,
                          char *err, size_t err_len) {
  char path[PATH_MAX];
  char *text = NULL;
  char *line;
  int inbound = 0;
  int terminal = 0;
  RtRunState terminal_state = RT_RUN_FAILED;
  if (!r || !out_inbound || !out_terminal || !out_terminal_state) return -1;
  r->ctx.next_event = 1;
  r->ctx.next_delivery = 1;
  rt_event_log_path(&r->ctx, path, sizeof(path));
  if (fs_truncate_incomplete_line(path) != 0) {
    if (err) snprintf(err, err_len, "recovery: torn-tail repair failed for %s", path);
    return -1;
  }
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
    if (errno == ENOENT) {
      *out_inbound = 0;
      *out_terminal = 0;
      *out_terminal_state = RT_RUN_FAILED;
      return 0;
    }
    if (err) snprintf(err, err_len, "recovery: event log unreadable for %s", r->ctx.run_id);
    return -1;
  }
  r->ctx.replay_mode = 1;
  line = text;
  while (*line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload, delivery;
    char type[RT_SMALL] = "";
    char source[RT_SMALL] = "";
    char event_id[RT_SMALL] = "";
    char payload_json[RT_XL];
    int seq;
    if (next) *next = '\0';
    if (json_ref_top_object(line, &event) != 0 ||
        json_ref_object_get_string(&event, "type", type, sizeof(type)) != 0 ||
        json_ref_object_get_object(&event, "payload", &payload) != 0 ||
        json_ref_value_copy(&payload, payload_json, sizeof(payload_json)) != 0) {
      fc_xfree(text);
      r->ctx.replay_mode = 0;
      if (err) snprintf(err, err_len,
                        "recovery: malformed committed event in %s", r->ctx.run_id);
      return -1;
    }
    (void)json_ref_object_get_string(&event, "source", source, sizeof(source));
    (void)json_ref_object_get_string(&event, "event_id", event_id, sizeof(event_id));
    seq = event_sequence(event_id);
    if (seq >= r->ctx.next_event) r->ctx.next_event = seq + 1;
    rt_apply_event_to_context(&r->ctx, type, payload_json);
    if (strcmp(type, "run_started") == 0) r->run_started_emitted = 1;
    if (strcmp(type, "run_done") == 0) {
      terminal = 1;
      terminal_state = RT_RUN_DONE;
    } else if (strcmp(type, "run_failed") == 0 ||
               strcmp(type, "run_canceled") == 0) {
      terminal = 1;
      terminal_state = RT_RUN_FAILED;
    }
    if (json_ref_object_get_object(&payload, "delivery", &delivery) == 0)
      r->ctx.next_delivery++;
    if (replay_action_record(r, type, source, &payload) != 0) {
      fc_xfree(text);
      r->ctx.replay_mode = 0;
      if (err) snprintf(err, err_len,
                        "recovery: action ledger overflow in %s", r->ctx.run_id);
      return -1;
    }
    if (!next) break;
    line = next + 1;
  }
  fc_xfree(text);
  r->ctx.replay_mode = 0;
  *out_inbound = inbound;
  *out_terminal = terminal;
  *out_terminal_state = terminal_state;
  return 0;
}

static int restore_runstate(RtScheduler *s, RtRun *r, const char *name,
                            char *err, size_t err_len) {
  char path[PATH_MAX];
  char envelope_path[PATH_MAX];
  char *text = NULL;
  JsonRef root, created, cursors;
  char profile[RT_SMALL] = "";
  long long advance = 0;
  long long phase = 0;
  int inbound = 0;
  int terminal = 0;
  RtRunState terminal_state = RT_RUN_FAILED;
  if (snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json", name)
      >= (int)sizeof(path) ||
      fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_string(&root, "run_id", r->ctx.run_id,
                                 sizeof(r->ctx.run_id)) != 0 ||
      strcmp(r->ctx.run_id, name) != 0 ||
      json_ref_object_get_string(&root, "profile", profile, sizeof(profile)) != 0 ||
      strcmp(profile, s->profile.name) != 0 ||
      json_ref_object_get_string(&root, "context_id", r->context_id,
                                 sizeof(r->context_id)) != 0 ||
      json_ref_object_get_object(&root, "created_from", &created) != 0 ||
      json_ref_object_get_string(&created, "event_id", r->created_from_event_id,
                                 sizeof(r->created_from_event_id)) != 0 ||
      json_ref_object_get_string(&created, "type", r->created_from_type,
                                 sizeof(r->created_from_type)) != 0 ||
      json_ref_object_get_object(&root, "cursors", &cursors) != 0) {
    fc_xfree(text);
    if (err) snprintf(err, err_len, "recovery: invalid control fields in %s", path);
    return -1;
  }
  (void)json_ref_object_get_long(&root, "advance", &advance);
  (void)json_ref_object_get_long(&cursors, "phase_index", &phase);
  fc_xfree(text);
  if (advance < 0 || phase < 0 || phase > s->profile.step_count) {
    if (err) snprintf(err, err_len, "recovery: invalid cursor in %s", path);
    return -1;
  }
  r->scheduler = s;
  r->profile = &s->profile;
  r->advance = (unsigned int)advance;
  r->phase_index = (size_t)phase;
  snprintf(r->ctx.workspace, sizeof(r->ctx.workspace), "workspace");
  snprintf(r->ctx.run_dir, sizeof(r->ctx.run_dir), "workspace/runs/%s", name);
  snprintf(r->ctx.context_id, sizeof(r->ctx.context_id), "%s", r->context_id);
  if (snprintf(envelope_path, sizeof(envelope_path),
               "workspace/bus/processed/%s.json", r->created_from_event_id)
      >= (int)sizeof(envelope_path) ||
      rt_ports_restore_origin(r, envelope_path) != 0) {
    if (err) snprintf(err, err_len,
                      "recovery: processed envelope missing for %s", name);
    return -1;
  }
  if (replay_run_log(r, &inbound, &terminal, &terminal_state,
                     err, err_len) != 0) return -1;
  inbound = rt_ports_inbound_commit_state(r);
  if (inbound < 0) {
    if (err) snprintf(err, err_len,
                      "recovery: claimed first inbound is not exact for %s",
                      name);
    return -1;
  }
  if (terminal) {
    r->lifecycle_terminal_emitted = 1;
    r->state = terminal_state;
    return rt_run_persist_state(r);
  }
  if (!inbound) {
    (void)rt_ports_recommit_inbound(r, envelope_path);
    inbound = rt_ports_inbound_commit_state(r);
    if (inbound != 1) {
      if (err) snprintf(err, err_len,
                        "recovery: inbound recommit failed for %s", name);
      return -1;
    }
  }
  if (rt_action_runner_recover_started(r) != 0) {
    if (err) snprintf(err, err_len,
                      "recovery: unfinished action reconciliation failed for %s", name);
    return -1;
  }
  memset(r->waiting_job_ids, 0, sizeof(r->waiting_job_ids));
  r->waiting_job_count = 0;
  rt_run_wait_clear(r);
  if (r->advance_delivered_message) {
    /* A committed delivery is an externally visible effect. Continue from
     * the durable phase cursor with the replayed advance flags intact; a
     * phase-zero restart could mint a different request id and redeliver. */
    r->state = RT_RUN_RUNNING;
  } else {
    /* An orphaned agent job has no committed output. Re-run the pass; action
     * requests already in the log retain their stable request ids and are
     * reconciled above before any new effect can launch. */
    r->phase_index = 0;
    if (r->advance > 0) r->advance--;
    r->state = RT_RUN_READY;
  }
  if (rt_run_persist_state(r) != 0) {
    if (err) snprintf(err, err_len,
                      "recovery: control persist failed for %s", name);
    return -1;
  }
  (void)rt_narrate("recovery: resumed %s from %s", name,
                   r->created_from_event_id);
  return 1;
}

int rt_scheduler_recover_runs(RtScheduler *s, char *err, size_t err_len) {
  RecoveryCandidate candidates[RT_MAX_ACTIVE_RUNS];
  size_t count = 0;
  if (!s) return -1;
  if (collect_candidates(candidates, RT_MAX_ACTIVE_RUNS, &count,
                         err, err_len) != 0) return -1;
  for (size_t i = 0; i < count; ++i) {
    RtRun *r = rt_scheduler_alloc_run(s);
    int rc;
    if (!r) {
      if (err) snprintf(err, err_len, "recovery: active run pool exhausted");
      return -1;
    }
    rc = restore_runstate(s, r, candidates[i].name, err, err_len);
    if (rc < 0) {
      rt_scheduler_free_run(s, r);
      return -1;
    }
    if (rc == 0) {
      rt_scheduler_free_run(s, r);
      continue;
    }
    s->runs[s->run_count++] = r;
  }
  if (s->run_count > s->run_count_peak) s->run_count_peak = s->run_count;
  return 0;
}
