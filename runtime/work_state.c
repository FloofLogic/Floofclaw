/* Reducer-owned work-step evidence and semantic budgets.
 *
 * This is a bounded materialized view of action/operation truth, keyed by
 * task_id + work_rev + request_id. Revision records preserve the external
 * attempt lineage and cumulative selection count across controller-created
 * work revisions. A later controller turn receives this projection instead
 * of scraping prior run directories. */

#include "runtime.h"
#include "publication_outbox.h"
#include "support/heap_guard.h"
#include "support/scratch_guard.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORK_STATE_PATH "workspace/memory/state/work_steps.json"
#define WORK_STEP_MAX_ITEMS 256
#define WORK_REVISION_MAX_ITEMS 128
#define WORK_STEP_PROJECT_MAX 8
#define WORK_DETAIL_MAX 1024
#define WORK_STATE_SERIALIZED_MAX (4U * 1024U * 1024U)

typedef struct {
  char context_id[RT_MED];
  char task_id[RT_SMALL];
  long long work_rev;
  char request_id[RT_SMALL];
  char action[RT_SMALL];
  char trigger_event_id[RT_SMALL];
  char selected_args[WORK_DETAIL_MAX];
  char handle[RT_SMALL];
  char status[RT_SMALL];
  char selected_event_id[RT_SMALL];
  char started_event_id[RT_SMALL];
  char result_event_id[RT_SMALL];
  char result_kind[RT_SMALL];
  char terminal_event_id[RT_SMALL];
  char terminal_kind[RT_SMALL];
  char repair_state[RT_SMALL];
  char detail[WORK_DETAIL_MAX];
  char terminal_detail[WORK_DETAIL_MAX];
} WorkStep;

typedef struct {
  char context_id[RT_MED];
  char task_id[RT_SMALL];
  long long work_rev;
  long long parent_work_rev;
  long long lineage_root_rev;
  int semantic_steps;
  int repair_attempts;
  char request_id[RT_SMALL];
  char action[RT_SMALL];
  char trigger_event_id[RT_SMALL];
  char selected_event_id[RT_SMALL];
} WorkRevision;

typedef struct {
  WorkRevision revisions[WORK_REVISION_MAX_ITEMS];
  size_t revision_count;
  WorkStep items[WORK_STEP_MAX_ITEMS];
  size_t count;
} WorkStore;

#define WORK_REBUILD_RUN_CAP 4096

static WorkStore g_work_store;
static FcScratchGuard g_work_guard =
    FC_SCRATCH_GUARD_INIT("work_state");

static WorkStore *store_borrow(void) {
  if (fc_scratch_guard_acquire(&g_work_guard) != 0) return NULL;
  return &g_work_store;
}

static int store_release(int result) {
  fc_scratch_guard_release(&g_work_guard);
  return result;
}

static int load_store(WorkStore *store) {
  char *text = NULL;
  JsonRef root, revisions, steps;
  memset(store, 0, sizeof(*store));
  if (fs_read_text(WORK_STATE_PATH, &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return 0;
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_array(&root, "steps", &steps) != 0) {
    free(text);
    return -1;
  }
  if (json_ref_object_get_array(&root, "revisions", &revisions) == 0) {
    if (json_ref_array_size(&revisions) > WORK_REVISION_MAX_ITEMS) {
      free(text);
      return -1;
    }
    for (size_t i = 0;
         i < json_ref_array_size(&revisions) &&
         store->revision_count < WORK_REVISION_MAX_ITEMS; ++i) {
      JsonRef item;
      WorkRevision *revision =
          &store->revisions[store->revision_count];
      JsonRef repair_value;
      long long semantic_steps = 0;
      long long repair_attempts = 0;
      if (json_ref_array_get(&revisions, i, &item) != 0 ||
          item.type != JSON_REF_OBJECT ||
          json_ref_object_get_string(&item, "task_id",
                                     revision->task_id,
                                     sizeof(revision->task_id)) != 0 ||
          json_ref_object_get_long(&item, "work_rev",
                                   &revision->work_rev) != 0 ||
          revision->work_rev <= 0 ||
          json_ref_object_get_long(&item, "lineage_root_rev",
                                   &revision->lineage_root_rev) != 0 ||
          revision->lineage_root_rev <= 0 ||
          json_ref_object_get_long(&item, "semantic_steps",
                                   &semantic_steps) != 0 ||
          semantic_steps < 0 ||
          semantic_steps > RT_WORK_SEMANTIC_STEP_LIMIT) {
        free(text);
        return -1;
      }
      if (json_ref_object_get(&item, "repair_attempts",
                              &repair_value) == 0) {
        if (json_ref_object_get_long(&item, "repair_attempts",
                                     &repair_attempts) != 0 ||
            repair_attempts < 0 ||
            repair_attempts > RT_WORK_REPAIR_ATTEMPT_LIMIT) {
          free(text);
          return -1;
        }
      }
      (void)json_ref_object_get_string(&item, "context_id",
                                       revision->context_id,
                                       sizeof(revision->context_id));
      (void)json_ref_object_get_long(&item, "parent_work_rev",
                                     &revision->parent_work_rev);
      revision->semantic_steps = (int)semantic_steps;
      revision->repair_attempts = (int)repair_attempts;
      (void)json_ref_object_get_string(&item, "request_id",
                                       revision->request_id,
                                       sizeof(revision->request_id));
      (void)json_ref_object_get_string(&item, "action",
                                       revision->action,
                                       sizeof(revision->action));
      (void)json_ref_object_get_string(&item, "trigger_event_id",
                                       revision->trigger_event_id,
                                       sizeof(revision->trigger_event_id));
      (void)json_ref_object_get_string(&item, "selected_event_id",
                                       revision->selected_event_id,
                                       sizeof(revision->selected_event_id));
      store->revision_count++;
    }
  }
  if (json_ref_array_size(&steps) > WORK_STEP_MAX_ITEMS) {
    free(text);
    return -1;
  }
  for (size_t i = 0;
       i < json_ref_array_size(&steps) &&
       store->count < WORK_STEP_MAX_ITEMS; ++i) {
    JsonRef item;
    WorkStep *step = &store->items[store->count];
    if (json_ref_array_get(&steps, i, &item) != 0 ||
        item.type != JSON_REF_OBJECT ||
        json_ref_object_get_string(&item, "task_id", step->task_id,
                                   sizeof(step->task_id)) != 0 ||
        json_ref_object_get_long(&item, "work_rev", &step->work_rev) != 0 ||
        step->work_rev <= 0 ||
        json_ref_object_get_string(&item, "request_id", step->request_id,
                                   sizeof(step->request_id)) != 0 ||
        !step->request_id[0])
      continue;
    (void)json_ref_object_get_string(&item, "context_id", step->context_id,
                                     sizeof(step->context_id));
    (void)json_ref_object_get_string(&item, "action", step->action,
                                     sizeof(step->action));
    (void)json_ref_object_get_string(&item, "trigger_event_id",
                                     step->trigger_event_id,
                                     sizeof(step->trigger_event_id));
    (void)json_ref_object_get_string(&item, "selected_args",
                                     step->selected_args,
                                     sizeof(step->selected_args));
    (void)json_ref_object_get_string(&item, "handle", step->handle,
                                     sizeof(step->handle));
    (void)json_ref_object_get_string(&item, "status", step->status,
                                     sizeof(step->status));
    (void)json_ref_object_get_string(&item, "selected_event_id",
                                     step->selected_event_id,
                                     sizeof(step->selected_event_id));
    (void)json_ref_object_get_string(&item, "started_event_id",
                                     step->started_event_id,
                                     sizeof(step->started_event_id));
    (void)json_ref_object_get_string(&item, "result_event_id",
                                     step->result_event_id,
                                     sizeof(step->result_event_id));
    (void)json_ref_object_get_string(&item, "result_kind",
                                     step->result_kind,
                                     sizeof(step->result_kind));
    (void)json_ref_object_get_string(&item, "terminal_event_id",
                                     step->terminal_event_id,
                                     sizeof(step->terminal_event_id));
    (void)json_ref_object_get_string(&item, "terminal_kind",
                                     step->terminal_kind,
                                     sizeof(step->terminal_kind));
    (void)json_ref_object_get_string(&item, "repair_state",
                                     step->repair_state,
                                     sizeof(step->repair_state));
    if (!step->repair_state[0])
      snprintf(step->repair_state, sizeof(step->repair_state), "none");
    (void)json_ref_object_get_string(&item, "detail", step->detail,
                                     sizeof(step->detail));
    (void)json_ref_object_get_string(&item, "terminal_detail",
                                     step->terminal_detail,
                                     sizeof(step->terminal_detail));
    store->count++;
  }
  free(text);
  return 0;
}

static int append_escaped_field(char *out, size_t out_len, size_t *pos,
                                const char *name, const char *value) {
  char escaped[WORK_DETAIL_MAX * 6 + 1];
  char prefix[RT_SMALL];
  if (json_escape(value ? value : "", escaped, sizeof(escaped)) != 0)
    return -1;
  if (snprintf(prefix, sizeof(prefix), "\"%s\":\"", name) >=
      (int)sizeof(prefix))
    return -1;
  return rt_append_text(out, out_len, pos, prefix) != 0 ||
         rt_append_text(out, out_len, pos, escaped) != 0 ||
         rt_append_text(out, out_len, pos, "\"") != 0 ? -1 : 0;
}

static int write_store(const WorkStore *store) {
  char *out;
  size_t pos = 0;
  int rc = -1;
  out = (char *)fc_xmalloc(WORK_STATE_SERIALIZED_MAX);
  if (!out) return -1;
  out[0] = '\0';
  if (fs_mkdir_p("workspace/memory/state") != 0 ||
      rt_append_text(out, WORK_STATE_SERIALIZED_MAX,
                     &pos, "{\"revisions\":[") != 0)
    goto done;
  for (size_t i = 0; i < store->revision_count; ++i) {
    const WorkRevision *revision = &store->revisions[i];
    char numbers[128];
    if ((i && rt_append_text(out, WORK_STATE_SERIALIZED_MAX,
                             &pos, ",") != 0) ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, "{") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "context_id", revision->context_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "task_id", revision->task_id) != 0)
      goto done;
    snprintf(numbers, sizeof(numbers),
             ",\"work_rev\":%lld,\"parent_work_rev\":%lld,"
             "\"lineage_root_rev\":%lld,"
             "\"semantic_steps\":%d,\"repair_attempts\":%d,",
             revision->work_rev, revision->parent_work_rev,
             revision->lineage_root_rev,
             revision->semantic_steps, revision->repair_attempts);
    if (rt_append_text(out, WORK_STATE_SERIALIZED_MAX,
                       &pos, numbers) != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "request_id", revision->request_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "action", revision->action) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "trigger_event_id",
                             revision->trigger_event_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "selected_event_id",
                             revision->selected_event_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, "}") != 0)
      goto done;
  }
  if (rt_append_text(out, WORK_STATE_SERIALIZED_MAX,
                     &pos, "],\"steps\":[") != 0)
    goto done;
  for (size_t i = 0; i < store->count; ++i) {
    const WorkStep *step = &store->items[i];
    char numbers[96];
    if ((i && rt_append_text(out, WORK_STATE_SERIALIZED_MAX,
                             &pos, ",") != 0) ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, "{") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "context_id", step->context_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "task_id", step->task_id) != 0)
      goto done;
    snprintf(numbers, sizeof(numbers), ",\"work_rev\":%lld,",
             step->work_rev);
    if (rt_append_text(out, WORK_STATE_SERIALIZED_MAX,
                       &pos, numbers) != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "request_id", step->request_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "action", step->action) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "trigger_event_id",
                             step->trigger_event_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "selected_args",
                             step->selected_args) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "handle", step->handle) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "status", step->status) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "selected_event_id",
                             step->selected_event_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "started_event_id",
                             step->started_event_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "result_event_id",
                             step->result_event_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "result_kind", step->result_kind) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "terminal_event_id",
                             step->terminal_event_id) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "terminal_kind", step->terminal_kind) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "repair_state", step->repair_state) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "detail", step->detail) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, ",") != 0 ||
        append_escaped_field(out, WORK_STATE_SERIALIZED_MAX, &pos,
                             "terminal_detail",
                             step->terminal_detail) != 0 ||
        rt_append_text(out, WORK_STATE_SERIALIZED_MAX, &pos, "}") != 0)
      goto done;
  }
  if (rt_append_text(out, WORK_STATE_SERIALIZED_MAX,
                     &pos, "]}\n") != 0)
    goto done;
  rc = fs_write_text_atomic(WORK_STATE_PATH, out);
done:
  fc_xfree(out);
  return rc;
}

static int step_index(const WorkStore *store,
                      const char *task_id, long long work_rev,
                      const char *request_id) {
  for (size_t i = 0; i < store->count; ++i) {
    const WorkStep *step = &store->items[i];
    if (step->work_rev == work_rev &&
        strcmp(step->task_id, task_id) == 0 &&
        strcmp(step->request_id, request_id) == 0)
      return (int)i;
  }
  return -1;
}

static int step_result_index(const WorkStore *store,
                             const char *task_id, long long work_rev,
                             const char *result_event_id) {
  for (size_t i = 0; i < store->count; ++i) {
    const WorkStep *step = &store->items[i];
    if (step->work_rev == work_rev &&
        strcmp(step->task_id, task_id) == 0 &&
        strcmp(step->result_event_id, result_event_id) == 0)
      return (int)i;
  }
  return -1;
}

static int revision_index(const WorkStore *store,
                          const char *task_id, long long work_rev) {
  for (size_t i = 0; i < store->revision_count; ++i) {
    const WorkRevision *revision = &store->revisions[i];
    if (revision->work_rev == work_rev &&
        strcmp(revision->task_id, task_id) == 0)
      return (int)i;
  }
  return -1;
}

static WorkRevision *add_revision(WorkStore *store,
                                  const char *task_id,
                                  long long work_rev) {
  if (store->revision_count >= WORK_REVISION_MAX_ITEMS) {
    int evict = -1;
    for (size_t i = 0; i < store->revision_count; ++i) {
      long long current_rev = 0;
      if (rt_task_work_rev_of(store->revisions[i].task_id,
                              &current_rev) != 0 ||
          current_rev != store->revisions[i].work_rev) {
        evict = (int)i;
        break;
      }
    }
    /* Active current revisions are never silently evicted. */
    if (evict < 0) return NULL;
    memmove(&store->revisions[evict], &store->revisions[evict + 1],
            (store->revision_count - (size_t)evict - 1U) *
                sizeof(store->revisions[0]));
    store->revision_count--;
  }
  memset(&store->revisions[store->revision_count], 0,
         sizeof(store->revisions[0]));
  snprintf(store->revisions[store->revision_count].task_id,
           sizeof(store->revisions[store->revision_count].task_id),
           "%s", task_id);
  store->revisions[store->revision_count].work_rev = work_rev;
  store->revisions[store->revision_count].lineage_root_rev = work_rev;
  return &store->revisions[store->revision_count++];
}

static int step_is_current_live(const WorkStep *step) {
  char *task;
  int result = 0;
  if (step->result_event_id[0]) {
    int pending = rt_publication_outbox_source_pending(
        step->result_event_id);
    if (pending != 0) return 1;
  }
  if (step->terminal_event_id[0]) {
    int pending = rt_publication_outbox_source_pending(
        step->terminal_event_id);
    if (pending != 0) return 1;
  }
  if (!step->context_id[0]) return 0;
  task = (char *)fc_xmalloc(RT_XL);
  if (!task) return 1; /* allocation failure must not permit eviction */
  result = rt_task_work_binding_json(step->context_id, step->task_id,
                                     step->work_rev, 0,
                                     task, RT_XL) == 0;
  fc_xfree(task);
  return result;
}

static WorkStep *add_step(WorkStore *store) {
  if (store->count >= WORK_STEP_MAX_ITEMS) {
    int evict = -1;
    for (size_t i = 0; i < store->count; ++i) {
      if (!step_is_current_live(&store->items[i])) {
        evict = (int)i;
        break;
      }
    }
    /* Active/current evidence is never silently evicted. */
    if (evict < 0) return NULL;
    memmove(&store->items[evict], &store->items[evict + 1],
            (store->count - (size_t)evict - 1U) * sizeof(store->items[0]));
    store->count--;
  }
  memset(&store->items[store->count], 0, sizeof(store->items[0]));
  snprintf(store->items[store->count].repair_state,
           sizeof(store->items[store->count].repair_state), "none");
  return &store->items[store->count++];
}

static void value_detail(const JsonRef *payload, const char *key,
                         char *out, size_t out_len) {
  JsonRef value;
  char *compact = NULL;
  out[0] = '\0';
  if (json_ref_object_get(payload, key, &value) != 0 ||
      !(compact = json_ref_value_compact_dup(&value)))
    return;
  if (strlen(compact) >= out_len)
    snprintf(out, out_len,
             "{\"truncated\":true,\"source\":\"event_log\"}");
  else
    snprintf(out, out_len, "%s", compact);
  fc_xfree(compact);
}

static void payload_detail(const char *payload_json,
                           char *out, size_t out_len) {
  JsonRef payload;
  char *compact = NULL;
  out[0] = '\0';
  if (json_ref_from_text(payload_json, &payload) == 0 &&
      (compact = json_ref_value_compact_dup(&payload)) != NULL) {
    if (strlen(compact) >= out_len)
      snprintf(out, out_len,
               "{\"truncated\":true,\"source\":\"event_log\"}");
    else
      snprintf(out, out_len, "%s", compact);
    fc_xfree(compact);
  }
}

static int apply_revision_event(WorkStore *store, const RtContext *ctx,
                                const char *type, const JsonRef *payload) {
  char task_id[RT_SMALL] = "", context_id[RT_MED] = "";
  char kind[RT_SMALL] = "", origin[RT_SMALL] = "";
  char request_id[RT_SMALL] = "", action[RT_SMALL] = "";
  char trigger_event_id[RT_SMALL] = "";
  long long work_rev = 0, parent_rev = 0;
  int idx;
  WorkRevision *revision;
  if (!store || !ctx || !type || !payload ||
      json_ref_object_get_string(payload, "task_id",
                                 task_id, sizeof(task_id)) != 0 ||
      !task_id[0])
    return -1;
  if (strcmp(type, "task_created") == 0) {
    if (json_ref_object_get_string(payload, "kind",
                                   kind, sizeof(kind)) != 0 ||
        strcmp(kind, "work") != 0)
      return 0;
    if (json_ref_object_get_long(payload, "work_rev", &work_rev) != 0)
      work_rev = 1;
    (void)json_ref_object_get_string(payload, "context_id",
                                     context_id, sizeof(context_id));
  } else if (strcmp(type, "task_updated") == 0) {
    /* Only an explicit revision-bearing update participates. Artifact,
     * status, and other ordinary task updates must not mint/reset a budget. */
    if (json_ref_object_get_long(payload, "work_rev", &work_rev) != 0 ||
        work_rev <= 0)
      return 0;
    (void)json_ref_object_get_string(payload, "context_id",
                                     context_id, sizeof(context_id));
    (void)json_ref_object_get_string(payload, "revision_origin",
                                     origin, sizeof(origin));
  } else {
    return 0;
  }
  if (work_rev <= 0) return -1;
  if (!context_id[0])
    snprintf(context_id, sizeof(context_id), "%s", ctx->context_id);
  idx = revision_index(store, task_id, work_rev);
  if (idx >= 0) {
    revision = &store->revisions[idx];
    if (strcmp(revision->context_id, context_id) != 0)
      return -1;
    if (strcmp(origin, "controller") == 0) {
      if (json_ref_object_get_long(payload, "parent_work_rev",
                                   &parent_rev) != 0 ||
          json_ref_object_get_string(payload, "request_id",
                                     request_id,
                                     sizeof(request_id)) != 0 ||
          json_ref_object_get_string(payload, "action",
                                     action, sizeof(action)) != 0 ||
          json_ref_object_get_string(payload, "trigger_event_id",
                                     trigger_event_id,
                                     sizeof(trigger_event_id)) != 0)
        return -1;
      return revision->parent_work_rev == parent_rev &&
             strcmp(revision->request_id, request_id) == 0 &&
             strcmp(revision->action, action) == 0 &&
             strcmp(revision->trigger_event_id, trigger_event_id) == 0
                 ? 0 : -1;
    }
    return revision->parent_work_rev == 0 &&
           revision->lineage_root_rev == work_rev ? 0 : -1;
  }
  if (strcmp(origin, "controller") == 0) {
    WorkRevision parent;
    WorkStep parent_step;
    int parent_idx, parent_step_idx;
    int have_parent_step = 0;
    if (json_ref_object_get_long(payload, "parent_work_rev",
                                 &parent_rev) != 0 ||
        parent_rev <= 0 || work_rev != parent_rev + 1 ||
        json_ref_object_get_string(payload, "request_id",
                                   request_id, sizeof(request_id)) != 0 ||
        !request_id[0] ||
        json_ref_object_get_string(payload, "action",
                                   action, sizeof(action)) != 0 ||
        strcmp(action, "work") != 0 ||
        json_ref_object_get_string(payload, "trigger_event_id",
                                   trigger_event_id,
                                   sizeof(trigger_event_id)) != 0 ||
        !trigger_event_id[0])
      return -1;
    parent_idx = revision_index(store, task_id, parent_rev);
    if (parent_idx < 0) return -1;
    parent = store->revisions[parent_idx];
    if (strcmp(parent.context_id, context_id) != 0 ||
        strcmp(parent.request_id, request_id) != 0 ||
        strcmp(parent.action, action) != 0 ||
        strcmp(parent.trigger_event_id, trigger_event_id) != 0)
      return -1;
    parent_step_idx = step_index(store, task_id, parent_rev, request_id);
    if (parent_step_idx < 0) return -1;
    parent_step = store->items[parent_step_idx];
    have_parent_step = 1;
    if (!parent_step.selected_event_id[0] ||
        strcmp(parent_step.context_id, context_id) != 0 ||
        strcmp(parent_step.action, action) != 0 ||
        strcmp(parent_step.trigger_event_id, trigger_event_id) != 0)
      return -1;
    revision = add_revision(store, task_id, work_rev);
    if (!revision) return -1;
    snprintf(revision->context_id, sizeof(revision->context_id), "%s",
             context_id);
    revision->parent_work_rev = parent_rev;
    revision->lineage_root_rev = parent.lineage_root_rev;
    revision->semantic_steps = parent.semantic_steps;
    revision->repair_attempts = parent.repair_attempts;
    snprintf(revision->request_id, sizeof(revision->request_id), "%s",
             request_id);
    snprintf(revision->action, sizeof(revision->action), "%s", action);
    snprintf(revision->trigger_event_id,
             sizeof(revision->trigger_event_id), "%s", trigger_event_id);
    snprintf(revision->selected_event_id,
             sizeof(revision->selected_event_id), "%s",
             have_parent_step ? parent_step.selected_event_id
                              : parent.selected_event_id);
    /* The revision-causing selection belongs to the inherited lineage but
     * was selected against the parent revision. Carry its trusted identity
     * forward without incrementing the semantic count so a mechanical
     * blocker can terminalize the now-current revision exactly. */
    if (step_index(store, task_id, work_rev, request_id) < 0) {
      WorkStep *carried = add_step(store);
      if (!carried) return -1;
      snprintf(carried->context_id, sizeof(carried->context_id), "%s",
               context_id);
      snprintf(carried->task_id, sizeof(carried->task_id), "%s", task_id);
      carried->work_rev = work_rev;
      snprintf(carried->request_id, sizeof(carried->request_id), "%s",
               request_id);
      snprintf(carried->action, sizeof(carried->action), "%s", action);
      snprintf(carried->trigger_event_id,
               sizeof(carried->trigger_event_id), "%s", trigger_event_id);
      snprintf(carried->selected_event_id,
               sizeof(carried->selected_event_id), "%s",
               revision->selected_event_id);
      if (have_parent_step) {
        snprintf(carried->selected_args, sizeof(carried->selected_args),
                 "%s", parent_step.selected_args);
        snprintf(carried->repair_state, sizeof(carried->repair_state),
                 "%s", parent_step.repair_state);
      }
      snprintf(carried->status, sizeof(carried->status), "revised");
    }
    return 0;
  }
  revision = add_revision(store, task_id, work_rev);
  if (!revision) return -1;
  snprintf(revision->context_id, sizeof(revision->context_id), "%s",
           context_id);
  revision->lineage_root_rev = work_rev;
  revision->semantic_steps = 0;
  revision->repair_attempts = 0;
  return 0;
}

int rt_work_state_is_event_type(const char *type) {
  return type &&
         (strcmp(type, "task_created") == 0 ||
          strcmp(type, "task_updated") == 0 ||
          strcmp(type, "action_request") == 0 ||
          strcmp(type, "action_started") == 0 ||
          strcmp(type, "action_succeeded") == 0 ||
          strcmp(type, "action_failed") == 0 ||
          strcmp(type, "action_rejected") == 0 ||
          strcmp(type, "operation_started") == 0 ||
          strcmp(type, "operation_result") == 0 ||
          strcmp(type, "work_completed") == 0 ||
          strcmp(type, "work_blocked") == 0);
}

static int apply_to_store(WorkStore *store, const RtContext *ctx,
                          const char *type, const char *payload_json,
                          const char *event_id) {
  JsonRef payload;
  char task_id[RT_SMALL] = "", request_id[RT_SMALL] = "";
  char action[RT_SMALL] = "", source_event_id[RT_SMALL] = "";
  char trigger_event_id[RT_SMALL] = "";
  char context_id[RT_MED] = "";
  long long work_rev = 0;
  int idx;
  WorkStep *step;
  if (!store || !rt_work_state_is_event_type(type) || !ctx || !payload_json ||
      !event_id || !*event_id ||
      json_ref_top_object(payload_json, &payload) != 0)
    return -1;
  (void)json_ref_object_get_string(&payload, "task_id",
                                   task_id, sizeof(task_id));
  (void)json_ref_object_get_long(&payload, "work_rev", &work_rev);
  (void)json_ref_object_get_string(&payload, "request_id",
                                   request_id, sizeof(request_id));
  (void)json_ref_object_get_string(&payload, "action",
                                   action, sizeof(action));
  (void)json_ref_object_get_string(&payload, "trigger_event_id",
                                   trigger_event_id,
                                   sizeof(trigger_event_id));
  (void)json_ref_object_get_string(&payload, "context_id",
                                   context_id, sizeof(context_id));
  if (!context_id[0])
    snprintf(context_id, sizeof(context_id), "%s", ctx->context_id);
  if (strcmp(type, "task_created") == 0 ||
      strcmp(type, "task_updated") == 0)
    return apply_revision_event(store, ctx, type, &payload);

  /* Ordinary actions and unbound legacy operations do not belong in the
   * work-controller ledger. */
  if (strcmp(type, "task_created") != 0 &&
      strcmp(type, "task_updated") != 0 &&
      (!task_id[0] || work_rev <= 0 || !request_id[0]))
    return 0;
  if ((strcmp(type, "work_completed") == 0 ||
       strcmp(type, "work_blocked") == 0) &&
      !trigger_event_id[0])
    return 0;
  if (strcmp(type, "operation_started") == 0 &&
      !trigger_event_id[0])
    return 0;

  if (strcmp(type, "action_request") == 0) {
    WorkRevision *revision;
    int revision_idx;
    int repair_source_idx = -1;
    /* Mechanical operation polls and legacy fixed-worker bindings carry a
     * revision but no controller consequence. They are not semantic work
     * selections and must not consume ledger capacity. */
    if (!trigger_event_id[0]) return 0;
    idx = step_index(store, task_id, work_rev, request_id);
    if (idx >= 0) {
      step = &store->items[idx];
      return strcmp(step->action, action) == 0 &&
             strcmp(step->context_id, context_id) == 0 &&
             strcmp(step->trigger_event_id, trigger_event_id) == 0 &&
             strcmp(step->selected_event_id, event_id) == 0 ? 0 : -1;
    }
    revision_idx = revision_index(store, task_id, work_rev);
    if (revision_idx < 0) {
      revision = add_revision(store, task_id, work_rev);
      if (!revision) return -1;
      snprintf(revision->context_id, sizeof(revision->context_id), "%s",
               context_id);
      revision->lineage_root_rev = work_rev;
    } else {
      revision = &store->revisions[revision_idx];
      if (strcmp(revision->context_id, context_id) != 0)
        return -1;
    }
    repair_source_idx = step_result_index(store, task_id, work_rev,
                                          trigger_event_id);
    if (repair_source_idx >= 0) {
      WorkStep *source = &store->items[repair_source_idx];
      int repairable =
          strcmp(source->result_kind, "action_failed") == 0 ||
          strcmp(source->result_kind, "action_rejected") == 0;
      if (repairable) {
        if (revision->repair_attempts >= RT_WORK_REPAIR_ATTEMPT_LIMIT)
          return -1;
        revision->repair_attempts++;
        snprintf(source->repair_state, sizeof(source->repair_state),
                 "repair_selected");
      } else {
        repair_source_idx = -1;
      }
    }
    if (revision->semantic_steps >= RT_WORK_SEMANTIC_STEP_LIMIT)
      return -1;
    step = add_step(store);
    if (!step) return -1;
    snprintf(step->context_id, sizeof(step->context_id), "%s",
             context_id);
    snprintf(step->task_id, sizeof(step->task_id), "%s", task_id);
    step->work_rev = work_rev;
    snprintf(step->request_id, sizeof(step->request_id), "%s", request_id);
    snprintf(step->action, sizeof(step->action), "%s", action);
    snprintf(step->trigger_event_id, sizeof(step->trigger_event_id), "%s",
             trigger_event_id);
    value_detail(&payload, "args", step->selected_args,
                 sizeof(step->selected_args));
    snprintf(step->status, sizeof(step->status), "selected");
    if (repair_source_idx >= 0)
      snprintf(step->repair_state, sizeof(step->repair_state),
               "repair_attempt");
    snprintf(step->selected_event_id, sizeof(step->selected_event_id),
             "%s", event_id);
    revision->semantic_steps++;
    snprintf(revision->request_id, sizeof(revision->request_id), "%s",
             request_id);
    snprintf(revision->action, sizeof(revision->action), "%s", action);
    snprintf(revision->trigger_event_id,
             sizeof(revision->trigger_event_id), "%s", trigger_event_id);
    snprintf(revision->selected_event_id,
             sizeof(revision->selected_event_id), "%s", event_id);
    return 0;
  }

  idx = step_index(store, task_id, work_rev, request_id);
  if (idx < 0 && strcmp(type, "operation_result") == 0) {
    long long current_rev = 0;
    /* A late old-revision operation can outlive retained selection
     * evidence. Its result remains normal truth; preserve a compact stale
     * record when capacity allows, otherwise leave the event log as the
     * evidence without reporting reducer corruption. */
    if (rt_task_work_rev_of(task_id, &current_rev) != 0 ||
        current_rev == work_rev)
      return 0;
    step = add_step(store);
    if (!step) return 0;
    snprintf(step->context_id, sizeof(step->context_id), "%s", context_id);
    snprintf(step->task_id, sizeof(step->task_id), "%s", task_id);
    step->work_rev = work_rev;
    snprintf(step->request_id, sizeof(step->request_id), "%s", request_id);
    snprintf(step->action, sizeof(step->action), "%s", action);
    snprintf(step->status, sizeof(step->status), "stale_result");
    idx = (int)(store->count - 1U);
  } else if (idx < 0) {
    if (!trigger_event_id[0] &&
        (strcmp(type, "action_started") == 0 ||
         strcmp(type, "action_succeeded") == 0 ||
         strcmp(type, "action_failed") == 0 ||
         strcmp(type, "action_rejected") == 0))
      return 0;
    return -1;
  }
  step = &store->items[idx];
  if (step->selected_event_id[0] &&
      (strcmp(step->context_id, context_id) != 0 ||
       !trigger_event_id[0] ||
       strcmp(step->trigger_event_id, trigger_event_id) != 0)
      )
    return -1;
  if (action[0] && step->action[0] &&
      strcmp(action, step->action) != 0)
    return -1;
  if (action[0] && !step->action[0])
    snprintf(step->action, sizeof(step->action), "%s", action);

  if (strcmp(type, "action_started") == 0) {
    if (step->started_event_id[0] &&
        strcmp(step->started_event_id, event_id) != 0)
      return -1;
    if (!step->result_event_id[0])
      snprintf(step->status, sizeof(step->status), "started");
    snprintf(step->started_event_id, sizeof(step->started_event_id),
             "%s", event_id);
  } else if (strcmp(type, "operation_started") == 0) {
    char handle[RT_SMALL] = "";
    (void)json_ref_object_get_string(&payload, "handle",
                                     handle, sizeof(handle));
    if (step->handle[0] && strcmp(step->handle, handle) != 0)
      return -1;
    snprintf(step->handle, sizeof(step->handle), "%s", handle);
    if (!step->result_event_id[0])
      snprintf(step->status, sizeof(step->status), "running");
  } else {
    char continuation[RT_SMALL] = "";
    (void)json_ref_object_get_string(&payload, "continuation",
                                     continuation, sizeof(continuation));
    if (strcmp(type, "action_succeeded") == 0 &&
        (strcmp(continuation, "managed_operation") == 0 ||
         strcmp(continuation, "work_terminal") == 0))
      return 0;
    /* Wake-bearing source events stamp their own exact id. An inbound bus
     * copy has a different event id and must not become fresh evidence. */
    if (json_ref_object_get_string(&payload, "source_event_id",
                                   source_event_id,
                                   sizeof(source_event_id)) != 0 ||
        strcmp(source_event_id, event_id) != 0)
      return 0;
    if (strcmp(type, "work_completed") == 0 ||
        strcmp(type, "work_blocked") == 0) {
      if (step->terminal_event_id[0])
        return strcmp(step->terminal_event_id, event_id) == 0 &&
               strcmp(step->terminal_kind, type) == 0 ? 0 : -1;
      snprintf(step->terminal_event_id,
               sizeof(step->terminal_event_id), "%s", event_id);
      snprintf(step->terminal_kind, sizeof(step->terminal_kind), "%s",
               type);
      if (strcmp(type, "work_completed") == 0) {
        snprintf(step->status, sizeof(step->status), "completed");
        value_detail(&payload, "summary", step->terminal_detail,
                     sizeof(step->terminal_detail));
      } else {
        snprintf(step->status, sizeof(step->status), "blocked");
        value_detail(&payload, "blocker", step->terminal_detail,
                     sizeof(step->terminal_detail));
      }
      /* Backward-compatible evidence for terminal actions that have no
       * separate step result. Budget/repair blockers retain the prior action
       * result and use the dedicated terminal slot. */
      if (!step->result_event_id[0]) {
        snprintf(step->result_event_id, sizeof(step->result_event_id),
                 "%s", event_id);
        snprintf(step->result_kind, sizeof(step->result_kind), "%s", type);
        snprintf(step->detail, sizeof(step->detail), "%s",
                 step->terminal_detail);
      }
      return 0;
    }
    if (step->result_event_id[0])
      return strcmp(step->result_event_id, event_id) == 0 &&
             strcmp(step->result_kind, type) == 0 ? 0 : -1;
    snprintf(step->result_event_id, sizeof(step->result_event_id),
             "%s", event_id);
    snprintf(step->result_kind, sizeof(step->result_kind), "%s", type);
    if (strcmp(type, "action_succeeded") == 0) {
      snprintf(step->status, sizeof(step->status), "succeeded");
      value_detail(&payload, "result", step->detail, sizeof(step->detail));
    } else if (strcmp(type, "action_failed") == 0) {
      snprintf(step->status, sizeof(step->status), "failed");
      snprintf(step->repair_state, sizeof(step->repair_state), "%s",
               strcmp(step->repair_state, "repair_attempt") == 0
                   ? "repair_failed" : "repair_available");
      payload_detail(payload_json, step->detail, sizeof(step->detail));
    } else if (strcmp(type, "action_rejected") == 0) {
      snprintf(step->status, sizeof(step->status), "rejected");
      snprintf(step->repair_state, sizeof(step->repair_state), "%s",
               strcmp(step->repair_state, "repair_attempt") == 0
                   ? "repair_failed" : "repair_available");
      payload_detail(payload_json, step->detail, sizeof(step->detail));
    } else if (strcmp(type, "operation_result") == 0) {
      (void)json_ref_object_get_string(&payload, "handle",
                                       step->handle, sizeof(step->handle));
      snprintf(step->status, sizeof(step->status), "finished");
      value_detail(&payload, "text", step->detail, sizeof(step->detail));
    }
  }
  return 0;
}

int rt_work_state_apply_event(const RtContext *ctx, const char *type,
                              const char *payload_json,
                              const char *event_id) {
  WorkStore *store;
  JsonRef payload;
  char task_id[RT_SMALL] = "", request_id[RT_SMALL] = "";
  long long work_rev = 0;
  int rc;
  if (!rt_work_state_is_event_type(type) || !ctx || !payload_json ||
      !event_id || !*event_id ||
      json_ref_top_object(payload_json, &payload) != 0)
    return -1;
  (void)json_ref_object_get_string(&payload, "task_id",
                                   task_id, sizeof(task_id));
  (void)json_ref_object_get_long(&payload, "work_rev", &work_rev);
  (void)json_ref_object_get_string(&payload, "request_id",
                                   request_id, sizeof(request_id));
  if (strcmp(type, "task_created") == 0 ||
      strcmp(type, "task_updated") == 0) {
    if (!task_id[0]) return 0;
  } else if (!task_id[0] || work_rev <= 0 || !request_id[0]) {
    return 0;
  }
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0) return store_release(-1);
  rc = apply_to_store(store, ctx, type, payload_json, event_id);
  if (rc == 0) rc = write_store(store);
  return store_release(rc);
}

static int project_step(const WorkStep *step, char *out, size_t out_len,
                        size_t *pos, int first) {
  if ((!first && rt_append_text(out, out_len, pos, ",") != 0) ||
      rt_append_text(out, out_len, pos, "{") != 0 ||
      append_escaped_field(out, out_len, pos, "request_id",
                           step->request_id) != 0 ||
      rt_append_text(out, out_len, pos, ",") != 0 ||
      append_escaped_field(out, out_len, pos, "action",
                           step->action) != 0 ||
      rt_append_text(out, out_len, pos, ",") != 0 ||
      append_escaped_field(out, out_len, pos, "trigger_event_id",
                           step->trigger_event_id) != 0 ||
      rt_append_text(out, out_len, pos, ",") != 0 ||
      append_escaped_field(out, out_len, pos, "selected_args",
                           step->selected_args) != 0 ||
      rt_append_text(out, out_len, pos, ",") != 0 ||
      append_escaped_field(out, out_len, pos, "status",
                           step->status) != 0)
    return -1;
  if (rt_append_text(out, out_len, pos, ",") != 0 ||
      append_escaped_field(out, out_len, pos, "repair_state",
                           step->repair_state) != 0)
    return -1;
  if (step->handle[0]) {
    if (rt_append_text(out, out_len, pos, ",") != 0 ||
        append_escaped_field(out, out_len, pos, "handle",
                             step->handle) != 0)
      return -1;
  }
  if (rt_append_text(out, out_len, pos, ",") != 0 ||
      append_escaped_field(out, out_len, pos, "selected_event_id",
                           step->selected_event_id) != 0)
    return -1;
  if (step->result_event_id[0]) {
    if (rt_append_text(out, out_len, pos, ",") != 0 ||
        append_escaped_field(out, out_len, pos, "result_event_id",
                             step->result_event_id) != 0 ||
        rt_append_text(out, out_len, pos, ",") != 0 ||
        append_escaped_field(out, out_len, pos, "result_kind",
                             step->result_kind) != 0 ||
        rt_append_text(out, out_len, pos, ",") != 0 ||
        append_escaped_field(out, out_len, pos, "detail",
                             step->detail) != 0)
      return -1;
  }
  if (step->terminal_event_id[0]) {
    if (rt_append_text(out, out_len, pos, ",") != 0 ||
        append_escaped_field(out, out_len, pos, "terminal_event_id",
                             step->terminal_event_id) != 0 ||
        rt_append_text(out, out_len, pos, ",") != 0 ||
        append_escaped_field(out, out_len, pos, "terminal_kind",
                             step->terminal_kind) != 0 ||
        rt_append_text(out, out_len, pos, ",") != 0 ||
        append_escaped_field(out, out_len, pos, "terminal_detail",
                             step->terminal_detail) != 0)
      return -1;
  }
  return rt_append_text(out, out_len, pos, "}");
}

int rt_work_state_projection_json(const char *task_id, long long work_rev,
                                  int max_repair_attempts,
                                  char *out, size_t out_len) {
  WorkStore *store;
  size_t matches[WORK_STEP_MAX_ITEMS], count = 0, start, pos = 0;
  char etask[RT_MED];
  char prefix[RT_MED + 192];
  long long lineage_root_rev = work_rev;
  int semantic_steps = 0;
  int repair_attempts = 0;
  if (!task_id || !*task_id || work_rev <= 0 ||
      max_repair_attempts < 0 ||
      max_repair_attempts > RT_WORK_REPAIR_ATTEMPT_LIMIT ||
      !out || out_len == 0 ||
      json_escape(task_id, etask, sizeof(etask)) != 0)
    return -1;
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0) return store_release(-1);
  {
    int ridx = revision_index(store, task_id, work_rev);
    if (ridx >= 0) {
      lineage_root_rev = store->revisions[ridx].lineage_root_rev;
      semantic_steps = store->revisions[ridx].semantic_steps;
      repair_attempts = store->revisions[ridx].repair_attempts;
    }
  }
  for (size_t i = 0; i < store->count; ++i)
    if (store->items[i].work_rev == work_rev &&
        strcmp(store->items[i].task_id, task_id) == 0)
      matches[count++] = i;
  start = count > WORK_STEP_PROJECT_MAX
        ? count - WORK_STEP_PROJECT_MAX : 0;
  snprintf(prefix, sizeof(prefix),
           "{\"task_id\":\"%s\",\"work_rev\":%lld,"
           "\"lineage_root_rev\":%lld,\"semantic_steps_used\":%d,"
           "\"semantic_steps_max\":%d,\"semantic_steps_remaining\":%d,"
           "\"repair_attempts_used\":%d,\"repair_attempts_max\":%d,"
           "\"repair_attempts_remaining\":%d,"
           "\"steps\":[",
           etask, work_rev, lineage_root_rev, semantic_steps,
           RT_WORK_SEMANTIC_STEP_LIMIT,
           RT_WORK_SEMANTIC_STEP_LIMIT - semantic_steps,
           repair_attempts, max_repair_attempts,
           repair_attempts < max_repair_attempts
               ? max_repair_attempts - repair_attempts : 0);
  if (rt_append_text(out, out_len, &pos, prefix) != 0)
    return store_release(-1);
  for (size_t i = start; i < count; ++i)
    if (project_step(&store->items[matches[i]], out, out_len, &pos,
                     i == start) != 0)
      return store_release(-1);
  return store_release(rt_append_text(out, out_len, &pos, "]}"));
}

int rt_work_state_budget_snapshot(const char *task_id, long long work_rev,
                                  RtWorkBudgetSnapshot *out) {
  WorkStore *store;
  int idx;
  if (!task_id || !*task_id || work_rev <= 0 || !out)
    return -1;
  memset(out, 0, sizeof(*out));
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0) return store_release(-1);
  idx = revision_index(store, task_id, work_rev);
  if (idx < 0) return store_release(1);
  snprintf(out->context_id, sizeof(out->context_id), "%s",
           store->revisions[idx].context_id);
  snprintf(out->task_id, sizeof(out->task_id), "%s",
           store->revisions[idx].task_id);
  out->work_rev = store->revisions[idx].work_rev;
  out->lineage_root_rev = store->revisions[idx].lineage_root_rev;
  out->semantic_steps = store->revisions[idx].semantic_steps;
  out->repair_attempts = store->revisions[idx].repair_attempts;
  snprintf(out->request_id, sizeof(out->request_id), "%s",
           store->revisions[idx].request_id);
  snprintf(out->action, sizeof(out->action), "%s",
           store->revisions[idx].action);
  snprintf(out->trigger_event_id, sizeof(out->trigger_event_id), "%s",
           store->revisions[idx].trigger_event_id);
  return store_release(0);
}

int rt_work_state_repair_disposition(const char *task_id,
                                     long long work_rev,
                                     const char *request_id,
                                     const char *source_event_id,
                                     int max_repair_attempts) {
  WorkStore *store;
  int idx, revision_idx, result = 0;
  if (!task_id || !*task_id || work_rev <= 0 ||
      !request_id || !*request_id || !source_event_id ||
      max_repair_attempts < 0 ||
      max_repair_attempts > RT_WORK_REPAIR_ATTEMPT_LIMIT)
    return -1;
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0) return store_release(-1);
  idx = step_index(store, task_id, work_rev, request_id);
  revision_idx = revision_index(store, task_id, work_rev);
  if (idx < 0 || revision_idx < 0)
    return store_release(-1);
  if (source_event_id[0] &&
      strcmp(store->items[idx].result_event_id, source_event_id) != 0)
    return store_release(-1);
  if (strcmp(store->items[idx].result_kind, "action_failed") == 0 ||
      strcmp(store->items[idx].result_kind, "action_rejected") == 0)
    result =
        store->revisions[revision_idx].repair_attempts >= max_repair_attempts
            ? 2 : 1;
  return store_release(result);
}

int rt_work_state_evidence_refs_json(const char *task_id, long long work_rev,
                                     char *out, size_t out_len) {
  WorkStore *store;
  size_t matches[WORK_STEP_MAX_ITEMS], count = 0, start, pos = 0;
  if (!task_id || !*task_id || work_rev <= 0 || !out || out_len == 0)
    return -1;
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0 ||
      rt_append_text(out, out_len, &pos, "[") != 0)
    return store_release(-1);
  for (size_t i = 0; i < store->count; ++i)
    if (store->items[i].work_rev == work_rev &&
        strcmp(store->items[i].task_id, task_id) == 0)
      matches[count++] = i;
  start = count > WORK_STEP_PROJECT_MAX
        ? count - WORK_STEP_PROJECT_MAX : 0;
  for (size_t i = start; i < count; ++i) {
    const WorkStep *step = &store->items[matches[i]];
    if ((i != start && rt_append_text(out, out_len, &pos, ",") != 0) ||
        rt_append_text(out, out_len, &pos, "{") != 0 ||
        append_escaped_field(out, out_len, &pos, "request_id",
                             step->request_id) != 0 ||
        rt_append_text(out, out_len, &pos, ",") != 0 ||
        append_escaped_field(out, out_len, &pos, "action",
                             step->action) != 0 ||
        rt_append_text(out, out_len, &pos, ",") != 0 ||
        append_escaped_field(out, out_len, &pos, "selected_event_id",
                             step->selected_event_id) != 0)
      return store_release(-1);
    if (step->result_event_id[0] &&
        (rt_append_text(out, out_len, &pos, ",") != 0 ||
         append_escaped_field(out, out_len, &pos, "result_event_id",
                              step->result_event_id) != 0))
      return store_release(-1);
    if (step->terminal_event_id[0] &&
        (rt_append_text(out, out_len, &pos, ",") != 0 ||
         append_escaped_field(out, out_len, &pos, "terminal_event_id",
                              step->terminal_event_id) != 0))
      return store_release(-1);
    if (rt_append_text(out, out_len, &pos, "}") != 0)
      return store_release(-1);
  }
  return store_release(rt_append_text(out, out_len, &pos, "]"));
}

int rt_work_state_result_matches(const char *task_id, long long work_rev,
                                 const char *request_id, const char *action,
                                 const char *source_event_id,
                                 const char *wake_type,
                                 long long *effective_work_rev) {
  WorkStore *store;
  int idx, result = 0;
  if (effective_work_rev) *effective_work_rev = work_rev;
  if (!task_id || !*task_id || work_rev <= 0 ||
      !request_id || !*request_id || !source_event_id ||
      !*source_event_id || !wake_type || !action || !*action)
    return -1;
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0) return store_release(-1);
  idx = step_index(store, task_id, work_rev, request_id);
  if (idx < 0) return store_release(0);
  {
    const WorkStep *step = &store->items[idx];
    if (!step->selected_event_id[0] || !step->trigger_event_id[0] ||
        strcmp(action, step->action) != 0)
      return store_release(0);
    if (strcmp(source_event_id, step->result_event_id) != 0)
      return store_release(0);
    if (strcmp(wake_type, "operation_result") == 0)
      result = strcmp(step->result_kind, "operation_result") == 0;
    else if (strcmp(wake_type, "work_step_result") == 0)
      result = strcmp(step->result_kind, "action_succeeded") == 0 ||
               strcmp(step->result_kind, "action_failed") == 0 ||
               strcmp(step->result_kind, "action_rejected") == 0;
    if (result && strcmp(action, "work") == 0) {
      int child = -1;
      for (size_t i = 0; i < store->revision_count; ++i) {
        const WorkRevision *revision = &store->revisions[i];
        if (strcmp(revision->task_id, task_id) != 0 ||
            revision->parent_work_rev != work_rev ||
            strcmp(revision->request_id, request_id) != 0 ||
            strcmp(revision->action, action) != 0 ||
            strcmp(revision->trigger_event_id,
                   step->trigger_event_id) != 0 ||
            strcmp(revision->selected_event_id,
                   step->selected_event_id) != 0)
          continue;
        if (child >= 0)
          return store_release(-1);
        child = (int)i;
      }
      if (child >= 0 && effective_work_rev)
        *effective_work_rev = store->revisions[child].work_rev;
    }
  }
  return store_release(result);
}

static int rebuild_compare(const void *left, const void *right) {
  const int *ln = (const int *)left;
  const int *rn = (const int *)right;
  return *ln < *rn ? -1 : *ln > *rn;
}

typedef struct {
  int number;
  char name[RT_SMALL];
} WorkRunName;

static int rebuild_run_compare(const void *left, const void *right) {
  const WorkRunName *a = (const WorkRunName *)left;
  const WorkRunName *b = (const WorkRunName *)right;
  return rebuild_compare(&a->number, &b->number);
}

static void rebuild_context(const char *run_name, RtContext *ctx) {
  char path[PATH_MAX], *text = NULL;
  JsonRef root;
  memset(ctx, 0, sizeof(*ctx));
  snprintf(ctx->run_id, sizeof(ctx->run_id), "%s", run_name);
  snprintf(ctx->run_dir, sizeof(ctx->run_dir),
           "workspace/runs/%s", run_name);
  if (snprintf(path, sizeof(path), "%s/runstate.json",
               ctx->run_dir) >= (int)sizeof(path))
    return;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) == 0 && text &&
      json_ref_top_object(text, &root) == 0)
    (void)json_ref_object_get_string(&root, "context_id",
                                     ctx->context_id,
                                     sizeof(ctx->context_id));
  free(text);
}

int rt_work_state_rebuild_from_logs(void) {
  WorkStore *store;
  WorkRunName *runs = NULL;
  RtContext *ctx = NULL;
  char *payload_json = NULL;
  DIR *dir;
  struct dirent *entry;
  size_t count = 0;
  long long skipped = 0;
  int saved_errno = 0, rc = -1;
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0) goto done;
  runs = (WorkRunName *)fc_xcalloc(WORK_REBUILD_RUN_CAP, sizeof(*runs));
  ctx = (RtContext *)fc_xcalloc(1, sizeof(*ctx));
  payload_json = (char *)fc_xmalloc(RT_XL);
  if (!runs || !ctx || !payload_json) goto done;
  dir = opendir("workspace/runs");
  if (!dir) {
    if (errno == ENOENT) rc = write_store(store);
    goto done;
  }
  errno = 0;
  {
    size_t min_idx = 0;
    long long dropped = 0;
    while ((entry = fs_readdir_checked(dir)) != NULL) {
      int number = 0;
      if (sscanf(entry->d_name, "run_%d", &number) != 1 || number < 1)
        continue;
      if (count < WORK_REBUILD_RUN_CAP) {
        runs[count].number = number;
        snprintf(runs[count].name, sizeof(runs[count].name), "%s",
                 entry->d_name);
        if (count == 0 || number < runs[min_idx].number) min_idx = count;
        count++;
        continue;
      }
      /* More run dirs than WORK_REBUILD_RUN_CAP (e.g. a runaway loop). Keep
       * only the most-recent CAP by run number rather than hard-failing:
       * failing here left a runaway unable to restart at all. The ledger only
       * needs recent open work, so dropping the oldest runs is safe. Replace
       * the oldest kept run whenever a newer one appears, then re-find it. */
      dropped++;
      if (number <= runs[min_idx].number) continue;
      runs[min_idx].number = number;
      snprintf(runs[min_idx].name, sizeof(runs[min_idx].name), "%s",
               entry->d_name);
      min_idx = 0;
      for (size_t k = 1; k < count; ++k)
        if (runs[k].number < runs[min_idx].number) min_idx = k;
    }
    if (dropped > 0)
      fprintf(stderr,
              "work-step ledger rebuild: %lld run dirs over cap %d; "
              "rebuilt from the most recent %zu\n",
              dropped, WORK_REBUILD_RUN_CAP, count);
  }
  if (saved_errno == 0) saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    goto done;
  }
  if (count > 1)
    qsort(runs, count, sizeof(runs[0]), rebuild_run_compare);
  for (size_t i = 0; i < count; ++i) {
    char path[PATH_MAX], *text = NULL, *line;
    rebuild_context(runs[i].name, ctx);
    if (rt_event_log_path(ctx, path, sizeof(path)) != 0) continue;
    if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
      continue;
    line = text;
    while (*line) {
      char *next = strchr(line, '\n');
      JsonRef event, payload;
      char type[RT_SMALL] = "", event_id[RT_SMALL] = "";
      if (!next) break; /* no newline means a torn, uncommitted tail */
      *next = '\0';
      if (json_ref_top_object(line, &event) == 0 &&
          json_ref_object_get_string(&event, "type", type,
                                     sizeof(type)) == 0 &&
          rt_work_state_is_event_type(type) &&
          json_ref_object_get_string(&event, "event_id", event_id,
                                     sizeof(event_id)) == 0 &&
          json_ref_object_get_object(&event, "payload", &payload) == 0 &&
          json_ref_value_copy(&payload, payload_json, RT_XL) == 0 &&
          apply_to_store(store, ctx, type, payload_json, event_id) != 0) {
        /* Replay sees a WINDOW of history, not all of it: run dirs older
         * than the retention keep count are pruned, and the scan is capped
         * at WORK_REBUILD_RUN_CAP. An event whose ancestor is outside that
         * window cannot be applied and is not evidence of corruption — but
         * aborting here left the ledger unwritten and the gateway unable to
         * start at all, recoverable only by quarantining a run by hand
         * (which frees a run number for reuse and risks
         * workspace_task_id_collision). Skip the event, name it exactly, and
         * keep rebuilding. The live path still rejects the same event,
         * because there the store is complete and a rejection is real. */
        skipped++;
        (void)rt_narrate(
            "work-step ledger rebuild: skipped %s %s in %s — its lineage is "
            "outside the replayed window; the run's event log remains the "
            "evidence",
            type, event_id, runs[i].name);
      }
      line = next + 1;
    }
    free(text);
  }
  if (skipped > 0)
    (void)rt_narrate(
        "work-step ledger rebuild: %lld event(s) skipped; ledger rebuilt "
        "from the applicable history",
        skipped);
  rc = write_store(store);
done:
  fc_xfree(payload_json);
  fc_xfree(ctx);
  fc_xfree(runs);
  return store_release(rc);
}

int rt_work_state_outcome_matches(const RtContext *ctx) {
  JsonRef payload;
  char task_id[RT_SMALL] = "", request_id[RT_SMALL] = "";
  char action[RT_SMALL] = "", source_event_id[RT_SMALL] = "";
  char status[RT_SMALL] = "";
  char context_id[RT_MED] = "";
  long long work_rev = 0, current_rev = 0;
  WorkStore *store;
  int idx, result = 0;
  if (!ctx || strcmp(ctx->event_kind, "work_outcome") != 0 ||
      json_ref_top_object(ctx->event_payload_json, &payload) != 0 ||
      json_ref_object_get_string(&payload, "task_id", task_id,
                                 sizeof(task_id)) != 0 ||
      json_ref_object_get_long(&payload, "work_rev", &work_rev) != 0 ||
      json_ref_object_get_string(&payload, "request_id", request_id,
                                 sizeof(request_id)) != 0 ||
      json_ref_object_get_string(&payload, "action", action,
                                 sizeof(action)) != 0 ||
      json_ref_object_get_string(&payload, "source_event_id",
                                 source_event_id,
                                 sizeof(source_event_id)) != 0 ||
      json_ref_object_get_string(&payload, "context_id", context_id,
                                 sizeof(context_id)) != 0 ||
      json_ref_object_get_string(&payload, "status", status,
                                 sizeof(status)) != 0)
    return 0;
  if (strcmp(context_id, ctx->context_id) != 0)
    return 0;
  {
    int current = rt_task_work_rev_of(task_id, &current_rev);
    /* Completion archives the task before its durable outcome is consumed,
     * so absence from the active projection is expected only for completed
     * truth. Blocked work remains active; either kind is rejected if a newer
     * revision is visible before the outcome turn. */
    if ((current == 0 && current_rev != work_rev) ||
        (current != 0 && strcmp(status, "completed") != 0))
      return 0;
  }
  store = store_borrow();
  if (!store) return -1;
  if (load_store(store) != 0) return store_release(-1);
  idx = step_index(store, task_id, work_rev, request_id);
  if (idx >= 0) {
    const WorkStep *step = &store->items[idx];
    const char *terminal_event =
        step->terminal_event_id[0] ? step->terminal_event_id
                                   : step->result_event_id;
    const char *terminal_kind =
        step->terminal_kind[0] ? step->terminal_kind
                               : step->result_kind;
    result = step->selected_event_id[0] && step->trigger_event_id[0] &&
             strcmp(step->context_id, context_id) == 0 &&
             strcmp(step->action, action) == 0 &&
             strcmp(terminal_event, source_event_id) == 0 &&
             ((strcmp(status, "completed") == 0 &&
               strcmp(terminal_kind, "work_completed") == 0) ||
              (strcmp(status, "blocked") == 0 &&
               strcmp(terminal_kind, "work_blocked") == 0));
  }
  return store_release(result);
}
