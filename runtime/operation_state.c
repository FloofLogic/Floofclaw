/* Managed-operation store — durable completion + correlation state for
 * detached external operations, one record per runtime-owned operation id.
 *
 * The kernel treats every stored value as opaque bytes: operation ids,
 * action names, task ids, and delivery routes are recorded and echoed,
 * never interpreted. The store answers exactly three questions:
 *
 *   - is this operation running or finished?     (one terminal transition)
 *   - which operations' deadlines are due?       (one deadline per op)
 *   - what correlation rides on the operation?   (context + route)
 *
 * workspace/memory/state/operations.json is a materialized view of
 * operation_started / operation_result events plus three direct
 * capability/metadata writes (completion token, worker handle, timeout
 * claim envelope id), mirroring how the affair store treats
 * next_review_at_ms.
 *
 * Identity model: `handle` is the RUNTIME-OWNED operation id, allocated
 * before the action launches ("op_<request_id>"). `worker_handle` is
 * whatever implementation-specific identity the action's start returned
 * (a PID, a provider job id) — kill/query/debug metadata, never the
 * identity of the operation.
 *
 * The completion token is the secret capability a detached worker must
 * present to complete the operation. It lives ONLY here and inside bus
 * claim envelopes; it must never reach event payloads, run artifacts,
 * prompts, or projections. It is cleared on terminal transition. */

#include "runtime.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPERATION_STATE_PATH "workspace/memory/state/operations.json"

/* Hard ceiling on records in-memory. Includes both running and
 * finished operations. A soft cap on finished records (see
 * purge_finished_over_cap below) further bounds the on-disk file:
 * default 100 finished records preserved, oldest terminals dropped
 * opportunistically on every reducer write.
 *
 * NOTE: a finished record is still read once more when its result run
 * recovers after a crash (the claim-run payload is rebuilt from the
 * record's immutable correlation). The purge drops OLDEST terminals
 * first, so the just-finished record survives until ~keep_finished
 * newer operations terminalize — a comfortable recovery window. */
#define OPERATION_DEFAULT_KEEP_FINISHED 100

typedef struct {
  char handle[RT_SMALL];        /* runtime-owned operation id (primary key) */
  char worker_handle[RT_SMALL]; /* implementation-specific worker identity */
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
  long long deadline_ms; /* absolute stable-wall ms; 0 = legacy/none */
  char completion_token[RT_OPERATION_TOKEN_HEX + 1]; /* secret; cleared on terminal */
  char claim_envelope_id[RT_SMALL]; /* reserved timeout-claim bus id */
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
    (void)json_ref_object_get_string(&item, "worker_handle", o->worker_handle,
                                     sizeof(o->worker_handle));
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
    (void)json_ref_object_get_long(&item, "deadline_ms", &o->deadline_ms);
    (void)json_ref_object_get_string(&item, "completion_token",
                                     o->completion_token,
                                     sizeof(o->completion_token));
    (void)json_ref_object_get_string(&item, "claim_envelope_id",
                                     o->claim_envelope_id,
                                     sizeof(o->claim_envelope_id));
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
    (void)rt_narrate("ops store: pruned finished operations down to keep_finished=%d",
                     cap);
}

static int write_store(const OperationStore *st) {
  char *out;
  size_t pos = 0;
  int rc = -1;
  if (fs_mkdir_p("workspace/memory/state") != 0) return -1;
  out = (char *)malloc(RT_XL * 2U);
  if (!out) return -1;
  if (rt_append_text(out, RT_XL * 2U, &pos, "{\"operations\":[") != 0) goto done;
  for (size_t i = 0; i < st->count; ++i) {
    const OperationItem *o = &st->items[i];
    char eh[RT_MED], ewh[RT_MED], ea[RT_MED], erid[RT_MED], etrigger[RT_MED];
    char et[RT_MED], ec[RT_MED], ech[RT_MED];
    char ead[RT_MED], eorigin[RT_MED], es[RT_MED];
    char etoken[RT_OPERATION_TOKEN_HEX * 2 + 2], eclaim[RT_MED];
    char item[RT_LARGE];
    int n;
    if (json_escape(o->handle, eh, sizeof(eh)) != 0 ||
        json_escape(o->worker_handle, ewh, sizeof(ewh)) != 0 ||
        json_escape(o->action, ea, sizeof(ea)) != 0 ||
        json_escape(o->request_id, erid, sizeof(erid)) != 0 ||
        json_escape(o->trigger_event_id, etrigger, sizeof(etrigger)) != 0 ||
        json_escape(o->task_id, et, sizeof(et)) != 0 ||
        json_escape(o->context_id, ec, sizeof(ec)) != 0 ||
        json_escape(o->channel, ech, sizeof(ech)) != 0 ||
        json_escape(o->adapter_id, ead, sizeof(ead)) != 0 ||
        json_escape(o->origin_event_id, eorigin, sizeof(eorigin)) != 0 ||
        json_escape(o->completion_token, etoken, sizeof(etoken)) != 0 ||
        json_escape(o->claim_envelope_id, eclaim, sizeof(eclaim)) != 0 ||
        json_escape(o->status, es, sizeof(es)) != 0) goto done;
    n = snprintf(item, sizeof(item),
                 "%s{\"handle\":\"%s\",\"worker_handle\":\"%s\","
                 "\"action\":\"%s\",\"request_id\":\"%s\","
                 "\"trigger_event_id\":\"%s\","
                 "\"task_id\":\"%s\","
                 "\"context_id\":\"%s\",\"channel\":\"%s\",\"adapter_id\":\"%s\","
                 "\"origin_event_id\":\"%s\",\"ref\":%s,\"status\":\"%s\",\"work_rev\":%lld,"
                 "\"deadline_ms\":%lld,\"completion_token\":\"%s\","
                 "\"claim_envelope_id\":\"%s\"}",
                 i ? "," : "", eh, ewh, ea, erid, etrigger, et, ec, ech, ead,
                 eorigin,
                 o->ref_json[0] ? o->ref_json : "null",
                 es, o->work_rev, o->deadline_ms, etoken, eclaim);
    if (n < 0 || (size_t)n >= sizeof(item) ||
        rt_append_text(out, RT_XL * 2U, &pos, item) != 0) goto done;
  }
  if (rt_append_text(out, RT_XL * 2U, &pos, "]}\n") != 0) goto done;
  rc = fs_write_text_atomic(OPERATION_STATE_PATH, out);
done:
  free(out);
  return rc;
}

static int op_index_of(const OperationStore *st, const char *operation_id) {
  if (!st || !operation_id || !*operation_id) return -1;
  for (size_t i = 0; i < st->count; ++i)
    if (strcmp(st->items[i].handle, operation_id) == 0) return (int)i;
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
      /* Already recorded. A duplicate start for a finished operation is
       * stale; for a running one it is a replay no-op only when the
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
    (void)json_ref_object_get_long(&payload, "deadline_ms", &o->deadline_ms);
    st.count++;
    purge_finished_over_cap(&st);
    return write_store(&st);
  }
  /* operation_result */
  if (idx < 0) {
    /* A result for an operation the store never recorded (e.g. a stray
     * replay after the record was recycled). Upsert a terminal record so
     * the exactly-once guard still holds for any later stray event. */
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
  /* Terminal transition: the deadline can never fire again, and the
   * completion capability is dead — clear the secret so retention ends
   * exactly at terminal truth. */
  st.items[idx].deadline_ms = 0;
  memset(st.items[idx].completion_token, 0,
         sizeof(st.items[idx].completion_token));
  purge_finished_over_cap(&st);
  return write_store(&st);
}

int rt_operation_status(const char *operation_id, char *status_out, size_t status_len) {
  OperationStore st;
  int idx;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  if (status_out && status_len)
    snprintf(status_out, status_len, "%s", st.items[idx].status);
  return 0;
}

int rt_operation_snapshot(const char *operation_id, RtOperationSnapshot *out) {
  OperationStore st;
  OperationItem *o;
  int idx;
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  o = &st.items[idx];
  snprintf(out->handle, sizeof(out->handle), "%s", o->handle);
  snprintf(out->worker_handle, sizeof(out->worker_handle), "%s",
           o->worker_handle);
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
  out->deadline_ms = o->deadline_ms;
  return 0;
}

/* ----- capability + metadata writes ---------------------------------
 * Direct store writes, mirroring how the affair watcher re-arms
 * next_review_at_ms: capability and worker metadata are view state,
 * not behavioral truth. A finished operation accepts none of them. */

int rt_operation_bind_secret(const char *operation_id, const char *token) {
  OperationStore st;
  int idx;
  if (!token || !*token || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0 || strcmp(st.items[idx].status, "finished") == 0) return 1;
  snprintf(st.items[idx].completion_token,
           sizeof(st.items[idx].completion_token), "%s", token);
  return write_store(&st);
}

int rt_operation_set_worker_handle(const char *operation_id,
                                   const char *worker_handle) {
  OperationStore st;
  int idx;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0 || strcmp(st.items[idx].status, "finished") == 0) return 1;
  snprintf(st.items[idx].worker_handle,
           sizeof(st.items[idx].worker_handle), "%s",
           worker_handle ? worker_handle : "");
  return write_store(&st);
}

int rt_operation_bind_claim_envelope(const char *operation_id,
                                     const char *envelope_id) {
  OperationStore st;
  int idx;
  if (!envelope_id || !*envelope_id || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0 || strcmp(st.items[idx].status, "finished") == 0) return 1;
  snprintf(st.items[idx].claim_envelope_id,
           sizeof(st.items[idx].claim_envelope_id), "%s", envelope_id);
  return write_store(&st);
}

/* Close a record whose terminal truth is owned by another durable event
 * (a bound call's action_failed/action_rejected consumed by the
 * work-step wake, or an ambiguous-completion recovery terminal). View
 * maintenance only: no operation_result exists and none may ever be
 * produced. */
int rt_operation_close_without_result(const char *operation_id) {
  OperationStore st;
  int idx;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  if (strcmp(st.items[idx].status, "finished") == 0) return 0;
  snprintf(st.items[idx].status, sizeof(st.items[idx].status), "finished");
  st.items[idx].deadline_ms = 0;
  memset(st.items[idx].completion_token, 0,
         sizeof(st.items[idx].completion_token));
  purge_finished_over_cap(&st);
  return write_store(&st);
}

int rt_operation_claim_envelope_of(const char *operation_id,
                                   char *out, size_t out_len) {
  OperationStore st;
  int idx;
  if (!out || out_len == 0 || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  snprintf(out, out_len, "%s", st.items[idx].claim_envelope_id);
  return 0;
}

/* Constant-time token comparison over the fixed token buffer so a
 * forger cannot time-probe prefix matches. Empty tokens never match
 * (an unbound record accepts no worker claims). */
int rt_operation_token_matches(const char *operation_id, const char *token) {
  OperationStore st;
  int idx;
  unsigned char diff = 0;
  char stored[RT_OPERATION_TOKEN_HEX + 1];
  char presented[RT_OPERATION_TOKEN_HEX + 1];
  size_t i;
  if (!token || !*token || load_store(&st) != 0) return 0;
  idx = op_index_of(&st, operation_id);
  if (idx < 0 || !st.items[idx].completion_token[0]) return 0;
  memset(stored, 0, sizeof(stored));
  memset(presented, 0, sizeof(presented));
  snprintf(stored, sizeof(stored), "%s", st.items[idx].completion_token);
  snprintf(presented, sizeof(presented), "%s", token);
  for (i = 0; i < sizeof(stored); ++i)
    diff |= (unsigned char)(stored[i] ^ presented[i]);
  return diff == 0;
}

/* Trusted launch-path read: the runtime re-stashes the capability when a
 * recovered run relaunches its managed call. Never expose to models,
 * artifacts, or projections. */
int rt_operation_token_of(const char *operation_id, char *out, size_t out_len) {
  OperationStore st;
  int idx;
  if (!out || out_len == 0 || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  snprintf(out, out_len, "%s", st.items[idx].completion_token);
  return 0;
}

/* Reverse lookup for kill/abort paths that only know the worker's own
 * identity. Running records only. */
int rt_operation_find_running_by_worker_handle(const char *worker_handle,
                                               char *operation_id_out,
                                               size_t out_len) {
  OperationStore st;
  if (!worker_handle || !*worker_handle || !operation_id_out || out_len == 0 ||
      load_store(&st) != 0)
    return -1;
  for (size_t i = 0; i < st.count; ++i) {
    if (strcmp(st.items[i].status, "running") != 0) continue;
    if (strcmp(st.items[i].worker_handle, worker_handle) != 0) continue;
    snprintf(operation_id_out, out_len, "%s", st.items[i].handle);
    return 0;
  }
  return 1;
}

/* Earliest completion deadline across running operations, 0 if none.
 * The gateway reactor uses this so a due deadline wakes it instead of
 * waiting for an unrelated module tick. A legacy running record with no
 * deadline counts as due now, so pre-deadline operations converge to
 * one timeout consequence instead of waiting forever. */
long long rt_operation_earliest_deadline_ms(void) {
  OperationStore st;
  long long best = 0;
  if (load_store(&st) != 0) return 0;
  for (size_t i = 0; i < st.count; ++i) {
    const OperationItem *o = &st.items[i];
    if (strcmp(o->status, "running") != 0) continue;
    if (o->deadline_ms <= 0) return 1; /* legacy: due immediately */
    if (best == 0 || o->deadline_ms < best) best = o->deadline_ms;
  }
  return best;
}

int rt_operation_list_deadline_due(long long now_ms, char ids[][RT_SMALL],
                                   size_t max, size_t *out_count) {
  OperationStore st;
  size_t n = 0;
  if (out_count) *out_count = 0;
  if (!ids || max == 0 || load_store(&st) != 0) return -1;
  for (size_t i = 0; i < st.count && n < max; ++i) {
    const OperationItem *o = &st.items[i];
    if (strcmp(o->status, "running") != 0) continue;
    if (o->deadline_ms > 0 && now_ms < o->deadline_ms) continue;
    snprintf(ids[n++], RT_SMALL, "%s", o->handle);
  }
  if (out_count) *out_count = n;
  return 0;
}

int rt_operation_any_running(void) {
  OperationStore st;
  if (load_store(&st) != 0) return 0;
  for (size_t i = 0; i < st.count; ++i)
    if (strcmp(st.items[i].status, "running") == 0) return 1;
  return 0;
}

int rt_operation_work_rev_of(const char *operation_id, long long *out_rev) {
  OperationStore st;
  int idx;
  if (out_rev) *out_rev = 0;
  if (load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  if (out_rev) *out_rev = st.items[idx].work_rev;
  return 0;
}

int rt_operation_channel_of(const char *operation_id, char *out, size_t out_len) {
  OperationStore st;
  int idx;
  if (!out || out_len == 0 || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  snprintf(out, out_len, "%s", st.items[idx].channel);
  return 0;
}

int rt_operation_context_id_of(const char *operation_id, char *out, size_t out_len) {
  OperationStore st;
  int idx;
  if (!out || out_len == 0 || load_store(&st) != 0) return -1;
  idx = op_index_of(&st, operation_id);
  if (idx < 0) return 1;
  snprintf(out, out_len, "%s", st.items[idx].context_id);
  return 0;
}

/* Cryptographically random completion token, lowercase hex. The token
 * is the secret half of the completion capability; the operation id is
 * public identity. */
int rt_operation_generate_token(char *out, size_t out_len) {
  unsigned char raw[RT_OPERATION_TOKEN_HEX / 2];
  int fd;
  size_t got = 0;
  static const char hex[] = "0123456789abcdef";
  if (!out || out_len < RT_OPERATION_TOKEN_HEX + 1) return -1;
  fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) return -1;
  while (got < sizeof(raw)) {
    ssize_t n = read(fd, raw + got, sizeof(raw) - got);
    if (n <= 0) { close(fd); return -1; }
    got += (size_t)n;
  }
  close(fd);
  for (size_t i = 0; i < sizeof(raw); ++i) {
    out[i * 2] = hex[raw[i] >> 4];
    out[i * 2 + 1] = hex[raw[i] & 0x0f];
  }
  out[RT_OPERATION_TOKEN_HEX] = '\0';
  return 0;
}
