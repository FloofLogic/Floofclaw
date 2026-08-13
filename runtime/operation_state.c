/* Managed-operation store — durable scheduling + correlation state for
 * detached external operations, one record per opaque handle.
 *
 * The kernel treats every stored value as opaque bytes: handles, action
 * names, task ids, and delivery routes are recorded and echoed, never
 * interpreted. The store answers exactly three questions:
 *
 *   - is this handle running or finished?           (exactly-once results)
 *   - which handles are due for a poll right now?   (one timer per handle)
 *   - what correlation rides on that handle's events? (context + route)
 *
 * workspace/memory/state/operations.json is a materialized view of
 * operation_started / operation_result events plus the driver's
 * arm/clear poll scheduling, mirroring how the affair store treats
 * next_review_at_ms. */

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPERATION_STATE_PATH "workspace/memory/state/operations.json"

/* Hard ceiling on records in-memory. Includes both running and
 * finished handles. Sized so a burst of managed_op activity (a
 * pricing lookup + a managed operation in flight + assorted
 * others) has plenty of headroom. A soft cap on finished records
 * (see purge_finished_over_cap below) further bounds the on-disk
 * file: default 100 finished records preserved, oldest terminals
 * dropped opportunistically on every reducer write. */
#define OPERATION_DEFAULT_KEEP_FINISHED 100

typedef struct {
  char handle[RT_SMALL];
  char action[RT_SMALL];
  char request_id[RT_SMALL];
  char trigger_event_id[RT_SMALL];
  char task_id[RT_SMALL];
  char context_id[RT_MED];
  char channel[RT_SMALL];
  char adapter_id[RT_SMALL];
  char origin_event_id[RT_SMALL];
  char ref_json[RT_MED];
  char status[RT_SMALL]; /* "running" | "finished" */
  long long work_rev;
  long long poll_interval_ms;
  long long next_poll_at_ms; /* 0 = no outstanding poll timer */
} OperationItem;

typedef struct {
  OperationItem items[OPERATION_MAX_ITEMS];
  size_t count;
} OperationStore;

static int load_store(OperationStore *st) {
  char *text = NULL;
  JsonRef root, ops;
  memset(st, 0, sizeof(*st));
  if (fs_read_text(OPERATION_STATE_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_array(&root, "operations", &ops) != 0) {
    free(text);
    return 0;
  }
  for (size_t i = 0; i < json_ref_array_size(&ops) && st->count < OPERATION_MAX_ITEMS; ++i) {
    JsonRef item, ref;
    OperationItem *o = &st->items[st->count];
    if (json_ref_array_get(&ops, i, &item) != 0 || item.type != JSON_REF_OBJECT) continue;
    if (json_ref_object_get_string(&item, "handle", o->handle, sizeof(o->handle)) != 0 ||
        !o->handle[0]) continue;
    (void)json_ref_object_get_string(&item, "action", o->action, sizeof(o->action));
    (void)json_ref_object_get_string(&item, "request_id", o->request_id,
                                     sizeof(o->request_id));
    (void)json_ref_object_get_string(&item, "trigger_event_id",
                                     o->trigger_event_id,
                                     sizeof(o->trigger_event_id));
    (void)json_ref_object_get_string(&item, "task_id", o->task_id, sizeof(o->task_id));
    (void)json_ref_object_get_string(&item, "context_id", o->context_id, sizeof(o->context_id));
    (void)json_ref_object_get_string(&item, "channel", o->channel, sizeof(o->channel));
    (void)json_ref_object_get_string(&item, "adapter_id", o->adapter_id, sizeof(o->adapter_id));
    (void)json_ref_object_get_string(&item, "origin_event_id",
                                     o->origin_event_id,
                                     sizeof(o->origin_event_id));
    if (json_ref_object_get(&item, "ref", &ref) == 0) {
      char raw[RT_MED];
      if (json_ref_value_copy(&ref, raw, sizeof(raw)) == 0 &&
          strcmp(raw, "null") != 0)
        snprintf(o->ref_json, sizeof(o->ref_json), "%s", raw);
    }
    if (json_ref_object_get_string(&item, "status", o->status, sizeof(o->status)) != 0 ||
        !o->status[0])
      snprintf(o->status, sizeof(o->status), "running");
    (void)json_ref_object_get_long(&item, "work_rev", &o->work_rev);
    (void)json_ref_object_get_long(&item, "poll_interval_ms", &o->poll_interval_ms);
    (void)json_ref_object_get_long(&item, "next_poll_at_ms", &o->next_poll_at_ms);
    st->count++;
  }
  free(text);
  return 0;
}

/* Read appliance.operations.keep_finished from config. Absent → default.
 * Cheap enough at per-write frequency (config file is <1KB and rare). */
static int keep_finished_cap(void) {
  char *text = NULL;
  JsonRef root, appliance, ops;
  long long v = OPERATION_DEFAULT_KEEP_FINISHED;
  if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return OPERATION_DEFAULT_KEEP_FINISHED;
  if (json_ref_first_object(text, &root) == 0 &&
      json_ref_object_get_object(&root, "appliance", &appliance) == 0 &&
      json_ref_object_get_object(&appliance, "operations", &ops) == 0)
    (void)json_ref_object_get_long(&ops, "keep_finished", &v);
  free(text);
  return (int)(v > 0 ? v : OPERATION_DEFAULT_KEEP_FINISHED);
}

/* Opportunistic terminal-record cap. Called before every write. If
 * finished-count exceeds keep_finished, drop the oldest finished
 * records until the count matches. Preserves insertion order
 * (write_store emits in array order) so "oldest" = smallest index
 * with status=finished.
 *
 * Running records are never touched — even a runaway concurrency
 * problem leaves the running set intact so correlation lookups still
 * work for whatever is actually in flight. */
static void purge_finished_over_cap(OperationStore *st) {
  int cap = keep_finished_cap();
  int finished_count = 0;
  size_t i;
  int dropped_any = 0;
  if (!st) return;
  for (i = 0; i < st->count; ++i)
    if (strcmp(st->items[i].status, "finished") == 0) finished_count++;
  while (finished_count > cap) {
    int oldest = -1;
    for (i = 0; i < st->count; ++i) {
      if (strcmp(st->items[i].status, "finished") == 0) { oldest = (int)i; break; }
    }
    if (oldest < 0) break;  /* shouldn't happen — count says there's one */
    memmove(&st->items[oldest], &st->items[oldest + 1],
            (st->count - (size_t)oldest - 1) * sizeof(OperationItem));
    st->count--;
    finished_count--;
    dropped_any = 1;
  }
  if (dropped_any)
    (void)rt_narrate("ops store: pruned finished handles down to keep_finished=%d",
                     cap);
}

static int write_store(const OperationStore *st) {
  char out[RT_XL];
  size_t pos = 0;
  if (fs_mkdir_p("workspace/memory/state") != 0) return -1;
  if (rt_append_text(out, sizeof(out), &pos, "{\"operations\":[") != 0) return -1;
  for (size_t i = 0; i < st->count; ++i) {
    const OperationItem *o = &st->items[i];
    char eh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED];
    char et[RT_MED], ec[RT_MED], ech[RT_MED];
    char ead[RT_MED], eorigin[RT_MED], es[RT_MED];
    char item[RT_LARGE];
    int n;
    if (json_escape(o->handle, eh, sizeof(eh)) != 0 ||
        json_escape(o->action, ea, sizeof(ea)) != 0 ||
        json_escape(o->request_id, erid, sizeof(erid)) != 0 ||
        json_escape(o->trigger_event_id, etrigger, sizeof(etrigger)) != 0 ||
        json_escape(o->task_id, et, sizeof(et)) != 0 ||
        json_escape(o->context_id, ec, sizeof(ec)) != 0 ||
        json_escape(o->channel, ech, sizeof(ech)) != 0 ||
        json_escape(o->adapter_id, ead, sizeof(ead)) != 0 ||
        json_escape(o->origin_event_id, eorigin, sizeof(eorigin)) != 0 ||
        json_escape(o->status, es, sizeof(es)) != 0) return -1;
    n = snprintf(item, sizeof(item),
                 "%s{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
                 "\"trigger_event_id\":\"%s\","
                 "\"task_id\":\"%s\","
                 "\"context_id\":\"%s\",\"channel\":\"%s\",\"adapter_id\":\"%s\","
                 "\"origin_event_id\":\"%s\",\"ref\":%s,\"status\":\"%s\",\"work_rev\":%lld,"
                 "\"poll_interval_ms\":%lld,\"next_poll_at_ms\":%lld}",
                 i ? "," : "", eh, ea, erid, etrigger, et, ec, ech, ead,
                 eorigin,
                 o->ref_json[0] ? o->ref_json : "null",
                 es, o->work_rev, o->poll_interval_ms, o->next_poll_at_ms);
    if (n < 0 || (size_t)n >= sizeof(item) ||
        rt_append_text(out, sizeof(out), &pos, item) != 0) return -1;
  }
  if (rt_append_text(out, sizeof(out), &pos, "]}\n") != 0) return -1;
  return fs_write_text_atomic(OPERATION_STATE_PATH, out);
}

static int op_index_of(const OperationStore *st, const char *handle) {
  if (!st || !handle || !*handle) return -1;
  for (size_t i = 0; i < st->count; ++i)
    if (strcmp(st->items[i].handle, handle) == 0) return (int)i;
  return -1;
}

int rt_operation_is_event_type(const char *type) {
  return type &&
         (strcmp(type, "operation_started") == 0 ||
          strcmp(type, "operation_result") == 0);
}

/* Reducer for operation events. Idempotent: replaying an event that is
 * already reflected in the store is success, not an error. */
int rt_operation_apply_event(const char *type, const char *payload_json) {
  OperationStore st;
  JsonRef payload, ref;
  char handle[RT_SMALL] = "";
  int idx;
  if (!rt_operation_is_event_type(type) || !payload_json ||
      json_ref_top_object(payload_json, &payload) != 0 ||
      json_ref_object_get_string(&payload, "handle", handle, sizeof(handle)) != 0 ||
      !handle[0] ||
      load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (strcmp(type, "operation_started") == 0) {
    OperationItem *o;
    if (idx >= 0) {
      /* Already recorded. A duplicate start for a finished handle is
       * stale; for a running handle it is a replay no-op only when the
       * complete durable work identity is exact. */
      char action[RT_SMALL] = "", request_id[RT_SMALL] = "";
      char trigger[RT_SMALL] = "", task_id[RT_SMALL] = "";
      long long work_rev = 0;
      (void)json_ref_object_get_string(&payload, "action", action,
                                       sizeof(action));
      (void)json_ref_object_get_string(&payload, "request_id", request_id,
                                       sizeof(request_id));
      (void)json_ref_object_get_string(&payload, "trigger_event_id", trigger,
                                       sizeof(trigger));
      (void)json_ref_object_get_string(&payload, "task_id", task_id,
                                       sizeof(task_id));
      (void)json_ref_object_get_long(&payload, "work_rev", &work_rev);
      return strcmp(st.items[idx].action, action) == 0 &&
             strcmp(st.items[idx].request_id, request_id) == 0 &&
             strcmp(st.items[idx].trigger_event_id, trigger) == 0 &&
             strcmp(st.items[idx].task_id, task_id) == 0 &&
             st.items[idx].work_rev == work_rev ? 0 : -1;
    }
    if (st.count >= OPERATION_MAX_ITEMS) {
      /* Recycle the oldest finished record rather than refusing new
       * work; a full table of running operations is a real error. */
      int free_idx = -1;
      for (size_t i = 0; i < st.count; ++i)
        if (strcmp(st.items[i].status, "finished") == 0) { free_idx = (int)i; break; }
      if (free_idx < 0) return -1;
      memmove(&st.items[free_idx], &st.items[free_idx + 1],
              (st.count - (size_t)free_idx - 1) * sizeof(OperationItem));
      st.count--;
    }
    o = &st.items[st.count];
    memset(o, 0, sizeof(*o));
    snprintf(o->handle, sizeof(o->handle), "%s", handle);
    (void)json_ref_object_get_string(&payload, "action", o->action, sizeof(o->action));
    (void)json_ref_object_get_string(&payload, "request_id", o->request_id,
                                     sizeof(o->request_id));
    (void)json_ref_object_get_string(&payload, "trigger_event_id",
                                     o->trigger_event_id,
                                     sizeof(o->trigger_event_id));
    (void)json_ref_object_get_string(&payload, "task_id", o->task_id, sizeof(o->task_id));
    (void)json_ref_object_get_string(&payload, "context_id", o->context_id, sizeof(o->context_id));
    (void)json_ref_object_get_string(&payload, "channel", o->channel, sizeof(o->channel));
    (void)json_ref_object_get_string(&payload, "adapter_id", o->adapter_id, sizeof(o->adapter_id));
    (void)json_ref_object_get_string(&payload, "origin_event_id",
                                     o->origin_event_id,
                                     sizeof(o->origin_event_id));
    if (json_ref_object_get(&payload, "ref", &ref) == 0) {
      char raw[RT_MED];
      if (json_ref_value_copy(&ref, raw, sizeof(raw)) == 0 && strcmp(raw, "null") != 0)
        snprintf(o->ref_json, sizeof(o->ref_json), "%s", raw);
    }
    snprintf(o->status, sizeof(o->status), "running");
    (void)json_ref_object_get_long(&payload, "work_rev", &o->work_rev);
    (void)json_ref_object_get_long(&payload, "poll_interval_ms", &o->poll_interval_ms);
    (void)json_ref_object_get_long(&payload, "next_poll_at_ms", &o->next_poll_at_ms);
    st.count++;
    purge_finished_over_cap(&st);
    return write_store(&st);
  }
  /* operation_result */
  if (idx < 0) {
    /* A start that returned finished(result) immediately never had a
     * running record. Upsert a terminal record so the exactly-once
     * guard still holds for any later stray event on this handle. */
    OperationItem *o;
    if (st.count >= OPERATION_MAX_ITEMS) {
      /* Try to make room by dropping the oldest finished. */
      purge_finished_over_cap(&st);
      if (st.count >= OPERATION_MAX_ITEMS) return -1;
    }
    o = &st.items[st.count];
    memset(o, 0, sizeof(*o));
    snprintf(o->handle, sizeof(o->handle), "%s", handle);
    (void)json_ref_object_get_string(&payload, "action", o->action, sizeof(o->action));
    (void)json_ref_object_get_string(&payload, "request_id", o->request_id,
                                     sizeof(o->request_id));
    (void)json_ref_object_get_string(&payload, "trigger_event_id",
                                     o->trigger_event_id,
                                     sizeof(o->trigger_event_id));
    (void)json_ref_object_get_string(&payload, "task_id", o->task_id, sizeof(o->task_id));
    (void)json_ref_object_get_string(&payload, "context_id", o->context_id, sizeof(o->context_id));
    (void)json_ref_object_get_string(&payload, "channel", o->channel, sizeof(o->channel));
    (void)json_ref_object_get_string(&payload, "adapter_id", o->adapter_id, sizeof(o->adapter_id));
    (void)json_ref_object_get_string(&payload, "origin_event_id",
                                     o->origin_event_id,
                                     sizeof(o->origin_event_id));
    (void)json_ref_object_get_long(&payload, "work_rev", &o->work_rev);
    if (json_ref_object_get(&payload, "ref", &ref) == 0) {
      char raw[RT_MED];
      if (json_ref_value_copy(&ref, raw, sizeof(raw)) == 0 &&
          strcmp(raw, "null") != 0)
        snprintf(o->ref_json, sizeof(o->ref_json), "%s", raw);
    }
    snprintf(o->status, sizeof(o->status), "finished");
    st.count++;
    purge_finished_over_cap(&st);
    return write_store(&st);
  }
  {
    char action[RT_SMALL] = "", request_id[RT_SMALL] = "";
    char trigger[RT_SMALL] = "", task_id[RT_SMALL] = "";
    long long work_rev = 0;
    (void)json_ref_object_get_string(&payload, "action", action,
                                     sizeof(action));
    (void)json_ref_object_get_string(&payload, "request_id", request_id,
                                     sizeof(request_id));
    (void)json_ref_object_get_string(&payload, "trigger_event_id", trigger,
                                     sizeof(trigger));
    (void)json_ref_object_get_string(&payload, "task_id", task_id,
                                     sizeof(task_id));
    (void)json_ref_object_get_long(&payload, "work_rev", &work_rev);
    if ((st.items[idx].action[0] &&
         strcmp(st.items[idx].action, action) != 0) ||
        (st.items[idx].request_id[0] &&
         strcmp(st.items[idx].request_id, request_id) != 0) ||
        (st.items[idx].trigger_event_id[0] &&
         strcmp(st.items[idx].trigger_event_id, trigger) != 0) ||
        (st.items[idx].task_id[0] &&
         strcmp(st.items[idx].task_id, task_id) != 0) ||
        (st.items[idx].work_rev > 0 &&
         st.items[idx].work_rev != work_rev))
      return -1;
  }
  if (strcmp(st.items[idx].status, "finished") == 0) return 0; /* idempotent */
  snprintf(st.items[idx].status, sizeof(st.items[idx].status), "finished");
  st.items[idx].next_poll_at_ms = 0;
  purge_finished_over_cap(&st);
  return write_store(&st);
}

int rt_operation_status(const char *handle, char *status_out, size_t status_len) {
  OperationStore st;
  int idx;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0) return 1;
  if (status_out && status_len)
    snprintf(status_out, status_len, "%s", st.items[idx].status);
  return 0;
}

int rt_operation_snapshot(const char *handle, RtOperationSnapshot *out) {
  OperationStore st;
  OperationItem *o;
  int idx;
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0) return 1;
  o = &st.items[idx];
  snprintf(out->handle, sizeof(out->handle), "%s", o->handle);
  snprintf(out->action, sizeof(out->action), "%s", o->action);
  snprintf(out->request_id, sizeof(out->request_id), "%s", o->request_id);
  snprintf(out->trigger_event_id, sizeof(out->trigger_event_id), "%s",
           o->trigger_event_id);
  snprintf(out->task_id, sizeof(out->task_id), "%s", o->task_id);
  snprintf(out->context_id, sizeof(out->context_id), "%s", o->context_id);
  snprintf(out->channel, sizeof(out->channel), "%s", o->channel);
  snprintf(out->adapter_id, sizeof(out->adapter_id), "%s", o->adapter_id);
  snprintf(out->origin_event_id, sizeof(out->origin_event_id), "%s",
           o->origin_event_id);
  snprintf(out->ref_json, sizeof(out->ref_json), "%s", o->ref_json);
  snprintf(out->status, sizeof(out->status), "%s", o->status);
  out->work_rev = o->work_rev;
  out->poll_interval_ms = o->poll_interval_ms;
  out->next_poll_at_ms = o->next_poll_at_ms;
  return 0;
}

/* Scheduling mutations. Direct store writes, mirroring how the affair
 * watcher re-arms next_review_at_ms: scheduling state is a view, not
 * behavioral truth. Arm keeps AT MOST one outstanding timer per handle
 * by construction — there is exactly one next_poll_at_ms field. A
 * finished handle can never be re-armed. */
int rt_operation_arm_poll(const char *handle, long long next_poll_at_ms) {
  OperationStore st;
  int idx;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0 || strcmp(st.items[idx].status, "finished") == 0) return 1;
  st.items[idx].next_poll_at_ms = next_poll_at_ms;
  return write_store(&st);
}

int rt_operation_clear_poll(const char *handle) {
  OperationStore st;
  int idx;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0) return 1;
  st.items[idx].next_poll_at_ms = 0;
  return write_store(&st);
}

/* Earliest armed poll deadline across running operations, 0 if none.
 * The gateway reactor uses this so a poll timer wakes it instead of
 * waiting for an unrelated module tick. */
long long rt_operation_earliest_due_ms(void) {
  OperationStore st;
  long long best = 0;
  if (load_store(&st) != 0) return 0;
  for (size_t i = 0; i < st.count; ++i) {
    const OperationItem *o = &st.items[i];
    if (strcmp(o->status, "running") != 0 || o->next_poll_at_ms <= 0) continue;
    if (best == 0 || o->next_poll_at_ms < best) best = o->next_poll_at_ms;
  }
  return best;
}

int rt_operation_list_due(long long now_ms, char handles[][RT_SMALL], size_t max,
                          size_t *out_count) {
  OperationStore st;
  size_t n = 0;
  if (out_count) *out_count = 0;
  if (!handles || max == 0 || load_store(&st) != 0) return -1;
  for (size_t i = 0; i < st.count && n < max; ++i) {
    const OperationItem *o = &st.items[i];
    if (strcmp(o->status, "running") != 0) continue;
    if (o->next_poll_at_ms <= 0 || now_ms < o->next_poll_at_ms) continue;
    snprintf(handles[n++], RT_SMALL, "%s", o->handle);
  }
  if (out_count) *out_count = n;
  return 0;
}

/* Compact correlation JSON for one handle — the payload the driver
 * stamps onto operation_poll bus envelopes. */
int rt_operation_correlation_json(const char *handle, char *out, size_t out_len) {
  OperationStore st;
  int idx, n;
  char eh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED];
  char et[RT_MED], ec[RT_MED], ech[RT_MED];
  char ead[RT_MED], eorigin[RT_MED];
  if (!out || out_len == 0 || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0) return 1;
  {
    const OperationItem *o = &st.items[idx];
    if (json_escape(o->handle, eh, sizeof(eh)) != 0 ||
        json_escape(o->action, ea, sizeof(ea)) != 0 ||
        json_escape(o->request_id, erid, sizeof(erid)) != 0 ||
        json_escape(o->trigger_event_id, etrigger, sizeof(etrigger)) != 0 ||
        json_escape(o->task_id, et, sizeof(et)) != 0 ||
        json_escape(o->context_id, ec, sizeof(ec)) != 0 ||
        json_escape(o->channel, ech, sizeof(ech)) != 0 ||
        json_escape(o->adapter_id, ead, sizeof(ead)) != 0 ||
        json_escape(o->origin_event_id, eorigin, sizeof(eorigin)) != 0)
      return -1;
    n = snprintf(out, out_len,
                 "{\"handle\":\"%s\",\"action\":\"%s\",\"request_id\":\"%s\","
                 "\"trigger_event_id\":\"%s\",\"task_id\":\"%s\","
                 "\"work_rev\":%lld,"
                 "\"context_id\":\"%s\",\"channel\":\"%s\",\"adapter_id\":\"%s\","
                 "\"origin_event_id\":\"%s\",\"ref\":%s}",
                 eh, ea, erid, etrigger, et, o->work_rev,
                 ec, ech, ead, eorigin,
                 o->ref_json[0] ? o->ref_json : "null");
  }
  return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

int rt_operation_work_rev_of(const char *handle, long long *out_rev) {
  OperationStore st;
  int idx;
  if (out_rev) *out_rev = 0;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0) return 1;
  if (out_rev) *out_rev = st.items[idx].work_rev;
  return 0;
}

int rt_operation_channel_of(const char *handle, char *out, size_t out_len) {
  OperationStore st;
  int idx;
  if (!out || out_len == 0 || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0) return 1;
  snprintf(out, out_len, "%s", st.items[idx].channel);
  return 0;
}

int rt_operation_context_id_of(const char *handle, char *out, size_t out_len) {
  OperationStore st;
  int idx;
  if (!out || out_len == 0 || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, handle);
  if (idx < 0) return 1;
  snprintf(out, out_len, "%s", st.items[idx].context_id);
  return 0;
}
