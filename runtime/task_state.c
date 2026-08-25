#include "task_internal.h"
#include "bus/bus.h"
#include "support/scratch_guard.h"
#include "support/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TASK_MAX_ITEMS 32
/* Field caps sized per field from observed production data, not from one
 * uniform maximum -- 32 slots of RT_LARGE everywhere cost 681KB of BSS to
 * hold rows that average ~500 bytes. Headroom is set by each field's
 * OVERFLOW BEHAVIOUR, which differs and is what actually matters:
 *   input      value_compact failure degrades the row to null; the event
 *              log keeps the text. Graceful, but it is user-pasted prose
 *              with no producer bound, so it keeps the full RT_LARGE.
 *   work       snprintf truncates the prose. Graceful. Observed max 221.
 *   done_when  snprintf truncates the prose. Graceful. Observed max 82.
 *   external   value_compact failure REJECTS the event, so this must clear
 *              the producer's worst case: operation_driver bounds the
 *              excerpt at OP_EXTERNAL_EXCERPT_MAX (500) and adds escaped
 *              handle/action/status, ~2.6KB in total.
 *   artifacts  value_compact failure REJECTS the event. append_artifacts
 *              already degrades to keeping the latest entry, but only if
 *              its merge buffer matches this cap. Observed max 483.
 *   working_memory rejects an append before the event is committed when the
 *              resulting opaque string would exceed its fixed cap. It is
 *              never truncated or compacted. */
#define TASK_INPUT_MAX     RT_LARGE
#define TASK_WORK_MAX      1024
#define TASK_DONE_MAX      512
#define TASK_EXTERNAL_MAX  3072
#define TASK_ARTIFACTS_MAX 2048
#define TASK_WORKING_MEMORY_MAX RT_TASK_WORKING_MEMORY_MAX
typedef struct {
  char task_id[RT_SMALL];
  char context_id[RT_MED];
  char kind[RT_SMALL];
  char state[RT_SMALL];
  char input[TASK_INPUT_MAX];
  char work[TASK_WORK_MAX];
  char done_when[TASK_DONE_MAX];
  char affair_id[RT_SMALL];
  char delegate[RT_SMALL];
  char external[TASK_EXTERNAL_MAX];
  char artifacts[TASK_ARTIFACTS_MAX];
  char working_memory[TASK_WORKING_MEMORY_MAX];
  char created_event_id[RT_SMALL];
  long long created_ms;
  long long updated_ms;
  /* Monotonic work-revision counter, owned by the reducer. Starts at 1
   * on create and increments on every accepted work-mutating update
   * (state.work / state.done_when). Attempt scheduling compares this
   * integer; it never reads prose. */
  long long work_rev;
} TaskItem;

typedef struct {
  TaskItem items[TASK_MAX_ITEMS];
  size_t count;
} TaskStore;

typedef struct {
  char out[RT_XL];
  char item[RT_XL];
  char tid[RT_MED];
  char ctx[RT_MED];
  char kind[RT_MED];
  char state[RT_MED];
  /* Escaping can double a field, so each mirror is 2x its source. */
  char work[TASK_WORK_MAX * 2];
  char affair[RT_MED];
  char delegate[RT_MED];
  char done[TASK_DONE_MAX * 2];
  char working_memory[TASK_WORKING_MEMORY_MAX * 2];
  char created[RT_MED];
  char work_field[TASK_WORK_MAX * 2 + 32];
  char done_field[TASK_DONE_MAX * 2 + 32];
  char working_memory_field[TASK_WORKING_MEMORY_MAX * 2 + 32];
  char affair_field[RT_MED + 32];
  char delegate_field[RT_MED + 32];
  /* external is copied raw, not escaped. */
  char external_field[TASK_EXTERNAL_MAX + 32];
} TaskWriteScratch;

typedef struct {
  TaskStore store;
  TaskWriteScratch write;
} TaskScratch;

static TaskScratch g_task_scratch;
static FcScratchGuard g_task_guard = FC_SCRATCH_GUARD_INIT("task_state");

static TaskStore *task_store_borrow(void) {
  if (fc_scratch_guard_acquire(&g_task_guard) != 0) return NULL;
  return &g_task_scratch.store;
}

static int task_store_release_result(int result) {
  fc_scratch_guard_release(&g_task_guard);
  return result;
}

static int value_compact(const JsonRef *v, char *out, size_t out_len) {
  char raw[RT_LARGE];
  if (!v || json_ref_value_copy(v, raw, sizeof(raw)) != 0) return -1;
  return json_compact(raw, out, out_len);
}

static int task_runtime_key(const char *key, int allow_task_id) {
  if (allow_task_id && strcmp(key, "task_id") == 0) return 0;
  return strcmp(key, "task_id") == 0 ||
         strcmp(key, "context_id") == 0 || strcmp(key, "created_event_id") == 0 ||
         strcmp(key, "created_ms") == 0 || strcmp(key, "updated_ms") == 0;
}

static int payload_has_task_runtime_key(const JsonRef *payload, int allow_task_id) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef value;
  while (json_ref_object_iter(payload, &cursor, key, sizeof(key), &value) == 1)
    if (task_runtime_key(key, allow_task_id)) return 1;
  return 0;
}

static int payload_has_immutable_task_root_key(const JsonRef *payload) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef value;
  if (!payload || payload->type != JSON_REF_OBJECT) return 1;
  while (json_ref_object_iter(payload, &cursor, key, sizeof(key), &value) == 1) {
    if (strcmp(key, "task_id") == 0 ||
        strcmp(key, "state") == 0 ||
        strcmp(key, "status") == 0 ||
        strcmp(key, "work") == 0 ||
        strcmp(key, "done_when") == 0 ||
        strcmp(key, "artifacts") == 0 ||
        strcmp(key, "affair_id") == 0 ||
        strcmp(key, "delegate") == 0 ||
        strcmp(key, "external") == 0 ||
        strcmp(key, "updated_ms") == 0) continue;
    return 1;
  }
  return 0;
}

static int payload_is_working_memory_append(const JsonRef *payload) {
  size_t cursor = 0;
  char key[RT_SMALL];
  JsonRef value;
  int saw_text = 0;
  if (!payload || payload->type != JSON_REF_OBJECT) return 0;
  while (json_ref_object_iter(payload, &cursor, key, sizeof(key), &value) == 1) {
    if (strcmp(key, "task_id") == 0) continue;
    if (strcmp(key, "working_memory") == 0 && value.type == JSON_REF_STRING) {
      saw_text = 1;
      continue;
    }
    return 0;
  }
  return saw_text;
}

static int task_state_ok(const char *state) {
  return strcmp(state, "open") == 0 || strcmp(state, "working") == 0 ||
         strcmp(state, "blocked") == 0 || strcmp(state, "completed") == 0 ||
         strcmp(state, "failed") == 0 || strcmp(state, "canceled") == 0;
}

/* Terminal = the reducer will never advance it again. "blocked" is NOT
 * terminal: a revision-bearing update revives it. */
static int task_state_terminal(const char *state) {
  return strcmp(state, "completed") == 0 || strcmp(state, "failed") == 0 ||
         strcmp(state, "canceled") == 0;
}

static int task_kind_is_message(const char *kind) {
  return kind &&
         (strcmp(kind, "input") == 0 ||
          strcmp(kind, "notification") == 0);
}

int rt_task_is_event_type(const char *type) {
  return type &&
         (strcmp(type, "task_created") == 0 ||
          strcmp(type, "task_updated") == 0 ||
          strcmp(type, "task_working_memory_appended") == 0 ||
          strcmp(type, "task_completed") == 0 ||
          strcmp(type, "task_failed") == 0 ||
          strcmp(type, "task_canceled") == 0 ||
          strcmp(type, "work_completed") == 0 ||
          strcmp(type, "work_blocked") == 0);
}

static int load_store(TaskStore *st) {
  char *text = NULL;
  JsonRef root, tasks;
  memset(st, 0, sizeof(*st));
  if (fs_read_text(RT_TASK_STATE_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0) {
    free(text);
    return 0;
  }
  for (size_t i = 0; i < json_ref_array_size(&tasks) && st->count < TASK_MAX_ITEMS; ++i) {
    JsonRef item, arts, input;
    TaskItem *t = &st->items[st->count];
    if (json_ref_array_get(&tasks, i, &item) != 0 || item.type != JSON_REF_OBJECT) continue;
    if (json_ref_object_get_string(&item, "task_id", t->task_id, sizeof(t->task_id)) != 0 ||
        json_ref_object_get_string(&item, "context_id", t->context_id, sizeof(t->context_id)) != 0 ||
        rt_task_json_status(&item, t->state, sizeof(t->state)) != 0 ||
        !task_state_ok(t->state)) continue;
    if (json_ref_object_get_string(&item, "kind", t->kind, sizeof(t->kind)) != 0 || !t->kind[0])
      snprintf(t->kind, sizeof(t->kind), "%s", "work");
    if (json_ref_object_get(&item, "input", &input) == 0)
      (void)value_compact(&input, t->input, sizeof(t->input));
    else snprintf(t->input, sizeof(t->input), "null");
    (void)rt_task_json_state_string(&item, "work", t->work, sizeof(t->work));
    (void)rt_task_json_state_string(&item, "done_when", t->done_when, sizeof(t->done_when));
    (void)rt_task_json_state_string(&item, "working_memory",
                                    t->working_memory,
                                    sizeof(t->working_memory));
    (void)rt_task_json_state_string(&item, "affair_id", t->affair_id, sizeof(t->affair_id));
    (void)rt_task_json_state_string(&item, "delegate", t->delegate, sizeof(t->delegate));
    {
      JsonRef ext;
      if (rt_task_json_state_value(&item, "external", &ext) == 0)
        (void)value_compact(&ext, t->external, sizeof(t->external));
      else snprintf(t->external, sizeof(t->external), "null");
    }
    (void)json_ref_object_get_string(&item, "created_event_id", t->created_event_id, sizeof(t->created_event_id));
    (void)json_ref_object_get_long(&item, "created_ms", &t->created_ms);
    /* write_store nests this under "state"; reading it at the top level
     * silently zeroed every task on reload. The helper tries state.<key>
     * first and falls back to the top level. */
    (void)rt_task_json_state_long(&item, "updated_ms", &t->updated_ms);
    if (rt_task_json_state_long(&item, "work_rev", &t->work_rev) != 0 || t->work_rev <= 0)
      t->work_rev = t->work[0] ? 1 : 0;
    if (rt_task_json_state_array(&item, "artifacts", &arts) == 0)
      (void)value_compact(&arts, t->artifacts, sizeof(t->artifacts));
    else snprintf(t->artifacts, sizeof(t->artifacts), "[]");
    st->count++;
  }
  free(text);
  return 0;
}

static int write_store(const TaskStore *st) {
  TaskWriteScratch *scratch = &g_task_scratch.write;
  size_t pos = 0;
  if (fs_mkdir_p("workspace/memory/state") != 0) return -1;
  if (rt_append_text(scratch->out, sizeof(scratch->out), &pos, "{\"tasks\":[") != 0)
    return -1;
  for (size_t i = 0; i < st->count; ++i) {
    const TaskItem *t = &st->items[i];
    int n;
    scratch->affair_field[0] = '\0';
    scratch->delegate_field[0] = '\0';
    scratch->external_field[0] = '\0';
    if (json_escape(t->task_id, scratch->tid, sizeof(scratch->tid)) != 0 ||
        json_escape(t->context_id, scratch->ctx, sizeof(scratch->ctx)) != 0 ||
        json_escape(t->kind[0] ? t->kind : "work", scratch->kind,
                    sizeof(scratch->kind)) != 0 ||
        json_escape(t->state, scratch->state, sizeof(scratch->state)) != 0 ||
        json_escape(t->work, scratch->work, sizeof(scratch->work)) != 0 ||
        json_escape(t->done_when, scratch->done, sizeof(scratch->done)) != 0 ||
        json_escape(t->working_memory, scratch->working_memory,
                    sizeof(scratch->working_memory)) != 0 ||
        json_escape(t->affair_id, scratch->affair, sizeof(scratch->affair)) != 0 ||
        json_escape(t->delegate, scratch->delegate, sizeof(scratch->delegate)) != 0 ||
        json_escape(t->created_event_id, scratch->created,
                    sizeof(scratch->created)) != 0)
      return -1;
    snprintf(scratch->work_field, sizeof(scratch->work_field),
             t->work[0] ? "\"%s\"" : "null", scratch->work);
    snprintf(scratch->done_field, sizeof(scratch->done_field),
             t->done_when[0] ? "\"%s\"" : "null", scratch->done);
    snprintf(scratch->working_memory_field,
             sizeof(scratch->working_memory_field), "\"%s\"",
             scratch->working_memory);
    if (t->affair_id[0])
      snprintf(scratch->affair_field, sizeof(scratch->affair_field),
               ",\"affair_id\":\"%s\"", scratch->affair);
    if (t->delegate[0])
      snprintf(scratch->delegate_field, sizeof(scratch->delegate_field),
               ",\"delegate\":\"%s\"", scratch->delegate);
    if (t->external[0])
      snprintf(scratch->external_field, sizeof(scratch->external_field),
               ",\"external\":%s", t->external);
    else
      snprintf(scratch->external_field, sizeof(scratch->external_field),
               ",\"external\":null");
    n = snprintf(scratch->item, sizeof(scratch->item),
                 "%s{\"task_id\":\"%s\",\"context_id\":\"%s\",\"kind\":\"%s\","
                 "\"input\":%s,\"created_event_id\":\"%s\","
                 "\"created_ms\":%lld,\"state\":{\"status\":\"%s\","
                 "\"work\":%s,\"done_when\":%s,\"work_rev\":%lld,"
                 "\"working_memory\":%s,\"artifacts\":%s%s%s%s,"
                 "\"updated_ms\":%lld}}",
                 i ? "," : "", scratch->tid, scratch->ctx, scratch->kind,
                 t->input[0] ? t->input : "null",
                 scratch->created, t->created_ms, scratch->state,
                 scratch->work_field, scratch->done_field, t->work_rev,
                 scratch->working_memory_field,
                 t->artifacts[0] ? t->artifacts : "[]",
                 scratch->affair_field, scratch->delegate_field,
                 scratch->external_field,
                 t->updated_ms);
    if (n < 0 || (size_t)n >= sizeof(scratch->item) ||
        rt_append_text(scratch->out, sizeof(scratch->out), &pos,
                    scratch->item) != 0)
      return -1;
  }
  if (rt_append_text(scratch->out, sizeof(scratch->out), &pos, "]}\n") != 0)
    return -1;
  return fs_write_text_atomic(RT_TASK_STATE_PATH, scratch->out);
}

int rt_tasks_state_json(char *out, size_t out_len) {
  char *text = NULL;
  if (!out || out_len == 0) return -1;
  if (fs_read_text(RT_TASK_STATE_PATH, &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
    snprintf(out, out_len, "{\"tasks\":[]}");
    return 0;
  }
  snprintf(out, out_len, "%s", text);
  free(text);
  return 0;
}

int rt_tasks_array_json(char *out, size_t out_len) {
  char state[RT_XL];
  JsonRef root, tasks;
  if (!out || out_len == 0) return -1;
  if (rt_tasks_state_json(state, sizeof(state)) != 0 ||
      json_ref_from_text(state, &root) != 0 ||
      json_ref_object_get_array(&root, "tasks", &tasks) != 0 ||
      json_ref_value_copy(&tasks, out, out_len) != 0) {
    snprintf(out, out_len, "[]");
  }
  return 0;
}

static int task_index_of(const TaskStore *st, const char *task_id) {
  if (!st || !task_id || !*task_id) return -1;
  for (size_t i = 0; i < st->count; ++i)
    if (strcmp(st->items[i].task_id, task_id) == 0) return (int)i;
  return -1;
}

static int task_id_belongs_to_run(const char *task_id, const char *run_id) {
  char prefix[RT_SMALL + 16];
  if (!task_id || !*task_id || !run_id || !*run_id) return 0;
  if (snprintf(prefix, sizeof(prefix), "task_%s_", run_id) >= (int)sizeof(prefix))
    return 0;
  return strncmp(task_id, prefix, strlen(prefix)) == 0;
}

int rt_task_ids_exist_for_run(const char *run_id) {
  TaskStore *st;
  char *text = NULL;
  int found = 0;
  if (!run_id || !*run_id) return 0;
  st = task_store_borrow();
  if (!st) return 0;
  if (load_store(st) != 0) return task_store_release_result(0);
  for (size_t i = 0; i < st->count; ++i) {
    if (task_id_belongs_to_run(st->items[i].task_id, run_id))
      return task_store_release_result(1);
  }
  task_store_release_result(0);
  if (fs_read_text(RT_TASK_ARCHIVE_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  for (char *line = text; line && *line && !found; ) {
    char *next = strchr(line, '\n');
    JsonRef root;
    char task_id[RT_SMALL] = "";
    if (next) *next = '\0';
    if (*line && json_ref_top_object(line, &root) == 0 &&
        json_ref_object_get_string(&root, "task_id", task_id, sizeof(task_id)) == 0 &&
        task_id_belongs_to_run(task_id, run_id))
      found = 1;
    line = next ? next + 1 : NULL;
  }
  free(text);
  return found;
}

int rt_workspace_next_run_task_collision(const char *workspace,
                                         char *run_id_out,
                                         size_t run_id_len) {
  char run_id[RT_SMALL];
  int n = rt_next_run_number_for_workspace(workspace ? workspace : "workspace");
  if (run_id_out && run_id_len) run_id_out[0] = '\0';
  if (n <= 0) return -1;
  if (snprintf(run_id, sizeof(run_id), "run_%03d", n) >= (int)sizeof(run_id))
    return -1;
  if (run_id_out && run_id_len) snprintf(run_id_out, run_id_len, "%s", run_id);
  return rt_task_ids_exist_for_run(run_id) ? 1 : 0;
}

static int append_artifacts(TaskItem *t, const char *incoming) {
  /* Must match t->artifacts: a merge that fits here but not in the field
   * would be snprintf-truncated into invalid JSON. */
  char merged[TASK_ARTIFACTS_MAX];
  const char *cur;
  size_t cur_len, in_len;
  if (!t || !incoming || !*incoming || strcmp(incoming, "[]") == 0) return 0;
  cur = t->artifacts[0] ? t->artifacts : "[]";
  if (strcmp(cur, "[]") == 0) {
    snprintf(t->artifacts, sizeof(t->artifacts), "%s", incoming);
    return 0;
  }
  cur_len = strlen(cur);
  in_len = strlen(incoming);
  if (cur_len < 2 || in_len < 2 || cur[0] != '[' || cur[cur_len - 1] != ']' ||
      incoming[0] != '[' || incoming[in_len - 1] != ']')
    return -1;
  if (in_len == 2) return 0;
  /* Bound the accumulated artifacts array. A multi-step task (e.g.
   * paginating a large fetched page across many tool calls) appends an
   * artifact per call; unbounded accumulation overflows this buffer.
   * Per Principle #7 the cap must not silently wedge the task: keeping
   * the *latest* artifact (newest tool outcome — what the worker's
   * next decision and the reviewer's done_when check actually need;
   * file parts still point at durable on-disk action_calls files) is
   * far better than failing every subsequent task_updated and
   * stranding the run. Stale read-chunk previews are transient. */
  if (snprintf(merged, sizeof(merged), "%.*s,%.*s",
               (int)(cur_len - 1), cur,
               (int)(in_len - 1), incoming + 1) >= (int)sizeof(merged)) {
    snprintf(t->artifacts, sizeof(t->artifacts), "%s", incoming);
    return 0;
  }
  snprintf(t->artifacts, sizeof(t->artifacts), "%s", merged);
  return 0;
}

int rt_task_reference_exists(const char *task_id) {
  TaskStore *st;
  int found;
  if (!task_id || !*task_id) return 0;
  st = task_store_borrow();
  if (!st) return 0;
  if (load_store(st) != 0) return task_store_release_result(0);
  found = task_index_of(st, task_id) >= 0;
  return task_store_release_result(found);
}

int rt_task_working_memory_of(const char *task_id,
                              char *out, size_t out_len) {
  TaskStore *st;
  int idx;
  if (out && out_len) out[0] = '\0';
  if (!task_id || !*task_id || !out || out_len == 0) return -1;
  st = task_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return task_store_release_result(-1);
  idx = task_index_of(st, task_id);
  if (idx < 0) return task_store_release_result(1);
  if (strlen(st->items[idx].working_memory) >= out_len)
    return task_store_release_result(-1);
  snprintf(out, out_len, "%s", st->items[idx].working_memory);
  return task_store_release_result(0);
}

int rt_task_reference_in_context(const char *context_id,
                                 const char *task_id) {
  TaskStore *st;
  if (!context_id || !*context_id || !task_id || !*task_id) return 0;
  st = task_store_borrow();
  if (!st) return 0;
  if (load_store(st) != 0) return task_store_release_result(0);
  for (size_t i = 0; i < st->count; ++i) {
    if (strcmp(st->items[i].context_id, context_id) == 0 &&
        strcmp(st->items[i].task_id, task_id) == 0)
      return task_store_release_result(1);
  }
  return task_store_release_result(0);
}

int rt_task_reference_active_in_context(const char *context_id,
                                        const char *task_id) {
  return rt_task_reference_in_context(context_id, task_id);
}

int rt_task_reference_is_ready_for_context(const char *context_id,
                                           const char *task_id) {
  TaskStore *st;
  int ready = 0;
  if (!context_id || !*context_id || !task_id || !*task_id) return 0;
  st = task_store_borrow();
  if (!st) return 0;
  if (load_store(st) != 0) return task_store_release_result(0);
  for (size_t i = 0; i < st->count; ++i) {
    TaskItem *t = &st->items[i];
    if (strcmp(t->context_id, context_id) == 0 &&
        strcmp(t->task_id, task_id) == 0) {
      ready = strcmp(t->state, "open") == 0 ||
              strcmp(t->state, "working") == 0;
      break;
    }
  }
  return task_store_release_result(ready);
}

static void context_from_ctx(RtContext *ctx, char *out, size_t out_len) {
  if (ctx && ctx->context_id[0]) snprintf(out, out_len, "%s", ctx->context_id);
  else if (ctx && ctx->origin_channel[0] && ctx->origin_adapter_id[0])
    snprintf(out, out_len, "chat:%s:%s", ctx->origin_channel, ctx->origin_adapter_id);
  else if (ctx && ctx->origin_channel[0])
    snprintf(out, out_len, "chat:%s", ctx->origin_channel);
  else snprintf(out, out_len, "chat:unknown");
}

static int normalize_create(RtContext *ctx, const JsonRef *payload, int event_seq,
                            char *out, size_t out_len) {
  char work[RT_LARGE], done[RT_LARGE], ework[RT_LARGE * 2], edone[RT_LARGE * 2];
  char work_field[RT_LARGE * 2 + 16], done_field[RT_LARGE * 2 + 16];
  char context[RT_MED], ectx[RT_MED], event_id[RT_SMALL], arts[RT_LARGE];
  char task_id[RT_SMALL], etask[RT_MED], kind[RT_SMALL] = "work", ekind[RT_MED];
  char state[RT_SMALL] = "", estate[RT_MED];
  char affair_id[RT_SMALL] = "", delegate[RT_SMALL] = "";
  char eaffair[RT_MED], edelegate[RT_MED], affair_field[RT_MED + 32] = "";
  char delegate_field[RT_MED + 32] = "", external_field[RT_LARGE + 32] = "";
  char input_field[RT_LARGE + 32] = "";
  JsonRef external_ref;
  long long ms = timing_system_wall_ms();
  int n;
  const char *run_id = ctx && ctx->run_id[0] ? ctx->run_id : "validate";
  (void)json_ref_object_get_string(payload, "kind", kind, sizeof(kind));
  (void)rt_task_json_status(payload, state, sizeof(state));
  if (!state[0] || !task_state_ok(state)) snprintf(state, sizeof(state), "open");
  if (payload_has_task_runtime_key(payload, 0) ||
      rt_task_normalize_artifacts(payload, run_id, event_seq,
                                  task_kind_is_message(kind) ? 0 : 1,
                                  arts, sizeof(arts)) != 0 ||
      json_escape(kind, ekind, sizeof(ekind)) != 0 ||
      json_escape(state, estate, sizeof(estate)) != 0) return -1;
  if (task_kind_is_message(kind)) {
    JsonRef input_ref;
    char raw[RT_LARGE], compact[RT_LARGE], input_type[RT_SMALL] = "";
    if (json_ref_object_get_object(payload, "input", &input_ref) != 0 ||
        json_ref_object_get_string(&input_ref, "type", input_type, sizeof(input_type)) != 0 ||
        strcmp(input_type, "message") != 0 ||
        json_ref_value_copy(&input_ref, raw, sizeof(raw)) != 0 ||
        json_compact(raw, compact, sizeof(compact)) != 0)
      return -1;
    snprintf(input_field, sizeof(input_field), "\"input\":%s", compact);
    work[0] = done[0] = '\0';
  } else {
    if (rt_task_json_state_string(payload, "work", work, sizeof(work)) != 0 ||
        rt_task_json_state_string(payload, "done_when", done, sizeof(done)) != 0 ||
        !work[0] || !done[0] ||
        json_escape(work, ework, sizeof(ework)) != 0 ||
        json_escape(done, edone, sizeof(edone)) != 0)
      return -1;
    snprintf(input_field, sizeof(input_field), "\"input\":null");
    (void)rt_task_json_state_string(payload, "affair_id", affair_id, sizeof(affair_id));
    (void)rt_task_json_state_string(payload, "delegate", delegate, sizeof(delegate));
  }
  context_from_ctx(ctx, context, sizeof(context));
  snprintf(task_id, sizeof(task_id), "task_%s_%06d", run_id, event_seq);
  if (json_escape(context, ectx, sizeof(ectx)) != 0 ||
      json_escape(task_id, etask, sizeof(etask)) != 0) return -1;
  snprintf(event_id, sizeof(event_id), "evt_%s_%06d", run_id, event_seq);
  snprintf(work_field, sizeof(work_field), work[0] ? "\"%s\"" : "null", ework);
  snprintf(done_field, sizeof(done_field), done[0] ? "\"%s\"" : "null", edone);
  if (affair_id[0]) {
    if (json_escape(affair_id, eaffair, sizeof(eaffair)) != 0) return -1;
    snprintf(affair_field, sizeof(affair_field), ",\"affair_id\":\"%s\"", eaffair);
  }
  if (delegate[0]) {
    if (json_escape(delegate, edelegate, sizeof(edelegate)) != 0) return -1;
    snprintf(delegate_field, sizeof(delegate_field), ",\"delegate\":\"%s\"", edelegate);
  }
  if (rt_task_json_state_value(payload, "external", &external_ref) == 0) {
    char ext[RT_LARGE];
    if (value_compact(&external_ref, ext, sizeof(ext)) != 0) return -1;
    snprintf(external_field, sizeof(external_field), ",\"external\":%s", ext);
  } else {
    snprintf(external_field, sizeof(external_field), ",\"external\":null");
  }
  n = snprintf(out, out_len,
               "{\"task_id\":\"%s\",\"context_id\":\"%s\",\"kind\":\"%s\","
               "%s,\"created_event_id\":\"%s\",\"created_ms\":%lld,"
               "\"state\":{\"status\":\"%s\",\"work\":%s,\"done_when\":%s,"
               "\"working_memory\":\"\",\"artifacts\":%s%s%s%s,"
               "\"updated_ms\":%lld}}",
               etask, ectx, ekind, input_field, event_id, ms,
               estate, work_field, done_field, arts,
               affair_field, delegate_field, external_field, ms);
  return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
}

int rt_task_normalize_event(RtContext *ctx, const char *type, const JsonRef *payload,
                            int event_seq, char *out, size_t out_len,
                            char *err, size_t err_len) {
  char task_id[RT_SMALL], etask[RT_MED], arts[RT_LARGE];
  char art_field[RT_LARGE + 32] = "";
  JsonRef present;
  long long ms = timing_system_wall_ms();
  long long artifact_work_rev = 0;
  int has_artifacts;
  int n;
  if (err && err_len) err[0] = '\0';
  if (!type || !payload || payload->type != JSON_REF_OBJECT || !out || out_len == 0) return -1;
  if (strcmp(type, "task_created") == 0) return normalize_create(ctx, payload, event_seq, out, out_len);
  if (payload_has_task_runtime_key(payload, 1)) return -1;
  has_artifacts = rt_task_json_state_array(payload, "artifacts", &present) == 0;
  if (json_ref_object_get_string(payload, "task_id", task_id, sizeof(task_id)) != 0 ||
      !rt_task_reference_exists(task_id) ||
      json_escape(task_id, etask, sizeof(etask)) != 0) {
    if (err) snprintf(err, err_len, "invalid task reference or artifacts");
    return -1;
  }
  if (strcmp(type, "task_working_memory_appended") == 0) {
    char incoming[TASK_WORKING_MEMORY_MAX] = "";
    char current[TASK_WORKING_MEMORY_MAX] = "";
    char eincoming[TASK_WORKING_MEMORY_MAX * 2];
    size_t needed;
    if (!payload_is_working_memory_append(payload) ||
        json_ref_object_get_string(payload, "working_memory", incoming,
                                   sizeof(incoming)) != 0 ||
        !incoming[0] ||
        rt_task_working_memory_of(task_id, current, sizeof(current)) != 0 ||
        json_escape(incoming, eincoming, sizeof(eincoming)) != 0) {
      if (err) snprintf(err, err_len, "invalid working_memory append");
      return -1;
    }
    needed = strlen(current) + (current[0] ? 1U : 0U) + strlen(incoming);
    if (needed >= TASK_WORKING_MEMORY_MAX) {
      if (err) snprintf(err, err_len, "working_memory limit exceeded");
      return -1;
    }
    n = snprintf(out, out_len,
                 "{\"task_id\":\"%s\",\"working_memory\":\"%s\","
                 "\"updated_ms\":%lld}",
                 etask, eincoming, ms);
    return n < 0 || (size_t)n >= out_len ? -1 : 0;
  }
  (void)rt_task_work_rev_of(task_id, &artifact_work_rev);
  if (strcmp(type, "task_updated") == 0) {
    char probe[RT_LARGE];
    if (rt_task_json_state_string(payload, "work",
                                  probe, sizeof(probe)) == 0 ||
        rt_task_json_state_string(payload, "done_when",
                                  probe, sizeof(probe)) == 0)
      artifact_work_rev++;
  }
  if (has_artifacts &&
      rt_task_normalize_artifacts(
          payload, ctx && ctx->run_id[0] ? ctx->run_id : "validate",
          event_seq, artifact_work_rev, arts, sizeof(arts)) != 0) {
    if (err) snprintf(err, err_len, "invalid task reference or artifacts");
    return -1;
  }
  if (has_artifacts) snprintf(art_field, sizeof(art_field), ",\"artifacts\":%s", arts);
  if (strcmp(type, "task_completed") == 0 || strcmp(type, "task_failed") == 0 ||
      strcmp(type, "task_canceled") == 0) {
    const char *state = strcmp(type, "task_completed") == 0 ? "completed" :
                        strcmp(type, "task_failed") == 0 ? "failed" : "canceled";
    n = snprintf(out, out_len,
                 "{\"task_id\":\"%s\",\"state\":{\"status\":\"%s\"%s,\"updated_ms\":%lld}}",
                 etask, state, art_field, ms);
    return (n < 0 || (size_t)n >= out_len) ? -1 : 0;
  }
  if (strcmp(type, "task_updated") == 0) {
    char work[RT_LARGE] = "", done[RT_LARGE] = "", state[RT_SMALL] = "";
    char ework[RT_LARGE * 2], edone[RT_LARGE * 2], estate[RT_MED];
    char affair_id[RT_SMALL] = "", delegate[RT_SMALL] = "";
    char eaffair[RT_MED], edelegate[RT_MED], external[RT_LARGE] = "";
    char outbuf[RT_XL];
    size_t pos = 0;
    int first = 1;
    int has_work, has_done;
    int has_affair, has_delegate, has_external;
    if (payload_has_immutable_task_root_key(payload)) return -1;
    has_work = rt_task_json_state_string(payload, "work", work, sizeof(work)) == 0;
    has_done = rt_task_json_state_string(payload, "done_when", done, sizeof(done)) == 0;
    has_affair = rt_task_json_state_string(payload, "affair_id", affair_id, sizeof(affair_id)) == 0;
    has_delegate = rt_task_json_state_string(payload, "delegate", delegate, sizeof(delegate)) == 0;
    has_external = rt_task_json_state_value(payload, "external", &present) == 0;
    if (has_external && value_compact(&present, external, sizeof(external)) != 0) return -1;
    (void)rt_task_json_status(payload, state, sizeof(state));
    if ((state[0] && !task_state_ok(state)) ||
        (has_work && json_escape(work, ework, sizeof(ework)) != 0) ||
        (has_done && json_escape(done, edone, sizeof(edone)) != 0) ||
        (has_affair && json_escape(affair_id, eaffair, sizeof(eaffair)) != 0) ||
        (has_delegate && json_escape(delegate, edelegate, sizeof(edelegate)) != 0) ||
        json_escape(state, estate, sizeof(estate)) != 0) return -1;
    if (rt_append_text(outbuf, sizeof(outbuf), &pos, "{\"task_id\":\"") != 0 ||
        rt_append_text(outbuf, sizeof(outbuf), &pos, etask) != 0 ||
        rt_append_text(outbuf, sizeof(outbuf), &pos, "\",\"state\":{") != 0)
      return -1;
    if (state[0]) {
      if (rt_append_text(outbuf, sizeof(outbuf), &pos, "\"status\":\"") != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, estate) != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, "\"") != 0)
        return -1;
      first = 0;
    }
    if (has_work) {
      if ((!first && rt_append_text(outbuf, sizeof(outbuf), &pos, ",") != 0) ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, "\"work\":\"") != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, ework) != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, "\"") != 0)
        return -1;
      first = 0;
    }
    if (has_done) {
      if ((!first && rt_append_text(outbuf, sizeof(outbuf), &pos, ",") != 0) ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, "\"done_when\":\"") != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, edone) != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, "\"") != 0)
        return -1;
      first = 0;
    }
    {
      char updated_field[RT_MED];
      snprintf(updated_field, sizeof(updated_field), "%s\"updated_ms\":%lld",
               first ? "" : ",", ms);
      if (rt_append_text(outbuf, sizeof(outbuf), &pos, updated_field) != 0) return -1;
      first = 0;
    }
    if (has_artifacts) {
      if (rt_append_text(outbuf, sizeof(outbuf), &pos, ",\"artifacts\":") != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, arts) != 0)
        return -1;
    }
    if (has_affair) {
      if (rt_append_text(outbuf, sizeof(outbuf), &pos, ",\"affair_id\":\"") != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, eaffair) != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, "\"") != 0)
        return -1;
    }
    if (has_delegate) {
      if (rt_append_text(outbuf, sizeof(outbuf), &pos, ",\"delegate\":\"") != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, edelegate) != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, "\"") != 0)
        return -1;
    }
    if (has_external) {
      if (rt_append_text(outbuf, sizeof(outbuf), &pos, ",\"external\":") != 0 ||
          rt_append_text(outbuf, sizeof(outbuf), &pos, external) != 0)
        return -1;
    }
    if (rt_append_text(outbuf, sizeof(outbuf), &pos, "}}") != 0 ||
        snprintf(out, out_len, "%s", outbuf) >= (int)out_len)
      return -1;
    return 0;
  }
  return -1;
}

int rt_task_create_input_for_user_message(RtContext *ctx,
                                          const char *user_text) {
  char context[RT_MED], ectx[RT_MED], text[RT_LARGE * 2];
  char task_id[RT_SMALL], etask[RT_MED], event_id[RT_SMALL];
  char payload[RT_XL];
  long long ms = timing_system_wall_ms();
  int event_seq;
  if (!ctx) return -1;
  event_seq = ctx->next_event;
  context_from_ctx(ctx, context, sizeof(context));
  snprintf(task_id, sizeof(task_id), "task_%s_%06d", ctx->run_id, event_seq);
  snprintf(event_id, sizeof(event_id), "evt_%s_%06d", ctx->run_id, event_seq);
  if (json_escape(context, ectx, sizeof(ectx)) != 0 ||
      json_escape(user_text ? user_text : "", text, sizeof(text)) != 0 ||
      json_escape(task_id, etask, sizeof(etask)) != 0)
    return -1;
  return rt_append_event_format(
      ctx, "task_created", "kernel", payload, sizeof(payload), 0,
      "{\"task_id\":\"%s\",\"context_id\":\"%s\",\"kind\":\"input\","
      "\"input\":{\"type\":\"message\",\"text\":\"%s\"},"
      "\"created_event_id\":\"%s\",\"created_ms\":%lld,"
      "\"state\":{\"status\":\"open\",\"work\":null,\"done_when\":null,"
      "\"working_memory\":\"\",\"artifacts\":[],\"updated_ms\":%lld}}",
      etask, ectx, text, event_id, ms, ms);
}

int rt_task_complete_message_on_send(RtContext *ctx, const char *task_id) {
  TaskStore *st;
  int idx;
  char etask[RT_MED], payload[RT_MED];
  TaskItem *t;
  if (!ctx || !ctx->context_id[0] || !task_id || !*task_id) return 0;
  st = task_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return task_store_release_result(0);
  if ((idx = task_index_of(st, task_id)) < 0)
    return task_store_release_result(0);
  t = &st->items[idx];
  if (strcmp(t->context_id, ctx->context_id) != 0 ||
      !task_kind_is_message(t->kind) ||
      strcmp(t->state, "open") != 0)
    return task_store_release_result(0);
  if (json_escape(task_id, etask, sizeof(etask)) != 0)
    return task_store_release_result(-1);
  task_store_release_result(0);
  return rt_append_event_format(
      ctx, "task_completed", "action_runner", payload, sizeof(payload), 0,
      "{\"task_id\":\"%s\",\"state\":\"completed\"}", etask);
}

/* A run that ends in failure or cancellation records that on the open
 * message task it was responsible for. Two tasks qualify: the one born in
 * this run (the user_message that started it), and the one this run was
 * bound to by its triggering event -- a runtime-owned kind such as
 * operation_result carries task_id explicitly. The second is a continuation
 * of an older ask: when that run dies nothing else is scheduled for the ask,
 * the human has already seen the failure narration, and an "open" record
 * would claim work in flight that no longer exists. Only reserved kinds are
 * trusted for the binding -- in a product-owned typed event a "task_id" key
 * is producer data (bus_type_is_reserved owns that boundary, exactly as in
 * agent_exec's task inference) -- and only within the run's own context. */
int rt_task_terminalize_open_message_tasks_for_run(RtContext *ctx,
                                                   const char *task_event_type) {
  TaskStore *st;
  JsonRef payload;
  char prefix[RT_SMALL + 16];
  char bound[RT_SMALL] = "";
  char task_ids[TASK_MAX_ITEMS][RT_SMALL];
  const char *state;
  size_t task_count = 0;
  if (!ctx || !ctx->run_id[0] || !task_event_type ||
      (strcmp(task_event_type, "task_failed") != 0 &&
       strcmp(task_event_type, "task_canceled") != 0))
    return 0;
  if (snprintf(prefix, sizeof(prefix), "task_%s_", ctx->run_id) >= (int)sizeof(prefix))
    return -1;
  if (ctx->event_payload_json[0] && bus_type_is_reserved(ctx->event_kind) &&
      json_ref_top_object(ctx->event_payload_json, &payload) == 0)
    (void)json_ref_object_get_string(&payload, "task_id", bound, sizeof(bound));
  st = task_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return task_store_release_result(0);
  state = strcmp(task_event_type, "task_failed") == 0 ? "failed" : "canceled";
  for (size_t i = 0; i < st->count; ++i) {
    TaskItem *t = &st->items[i];
    int born_here = strncmp(t->task_id, prefix, strlen(prefix)) == 0;
    int bound_here = bound[0] && strcmp(t->task_id, bound) == 0 &&
                     strcmp(t->context_id, ctx->context_id) == 0;
    if (!task_kind_is_message(t->kind) ||
        strcmp(t->state, "open") != 0 ||
        (!born_here && !bound_here))
      continue;
    snprintf(task_ids[task_count], sizeof(task_ids[task_count]), "%s", t->task_id);
    task_count++;
  }
  task_store_release_result(0);
  for (size_t i = 0; i < task_count; ++i) {
    char etask[RT_MED], payload[RT_MED];
    if (json_escape(task_ids[i], etask, sizeof(etask)) != 0) return -1;
    if (rt_append_event_format(
            ctx, task_event_type, "kernel", payload, sizeof(payload), 0,
            "{\"task_id\":\"%s\",\"state\":\"%s\"}", etask, state) != 0)
      return -1;
  }
  return 0;
}

int rt_task_work_rev_of(const char *task_id, long long *out_rev) {
  TaskStore *st;
  int idx;
  if (out_rev) *out_rev = 0;
  if (!task_id || !*task_id) return -1;
  st = task_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return task_store_release_result(-1);
  idx = task_index_of(st, task_id);
  if (idx < 0) return task_store_release_result(1);
  if (out_rev) *out_rev = st->items[idx].work_rev;
  return task_store_release_result(0);
}

/* Snapshot of the context's single open work task for attempt
 * scheduling: id, latest work + completion intent, the reducer-owned
 * work_rev, and the opaque external attempt record. Returns 0 when a
 * task was found, 1 when the context has no open work task. */
int rt_task_open_work_snapshot(const char *context_id,
                               char *task_id, size_t task_id_len,
                               char *work, size_t work_len,
                               char *done_when, size_t done_len,
                               long long *work_rev,
                               char *external_json, size_t external_len) {
  TaskStore *st;
  if (!context_id || !*context_id) return 1;
  st = task_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return task_store_release_result(1);
  for (size_t i = 0; i < st->count; ++i) {
    TaskItem *t = &st->items[i];
    if (strcmp(t->context_id, context_id) != 0) continue;
    if (strcmp(t->kind, "work") != 0) continue;
    if (strcmp(t->state, "open") != 0 && strcmp(t->state, "working") != 0) continue;
    if (task_id) snprintf(task_id, task_id_len, "%s", t->task_id);
    if (work) snprintf(work, work_len, "%s", t->work);
    if (done_when) snprintf(done_when, done_len, "%s", t->done_when);
    if (work_rev) *work_rev = t->work_rev;
    if (external_json)
      snprintf(external_json, external_len, "%s",
               t->external[0] ? t->external : "null");
    return task_store_release_result(0);
  }
  return task_store_release_result(1);
}

/* The store is a bounded projection; the event log is the record of
 * truth. Reclaim the oldest terminal task -- array order is insertion
 * order -- so a burst of finished input tasks can never wedge
 * task_created, and with it every agent call that requires a task
 * binding. Insertion order is used rather than updated_ms so eviction
 * stays deterministic even for rows written before updated_ms round
 * -tripped correctly. Returns -1 only when all TASK_MAX_ITEMS are still
 * live, which is a real backlog, not debris. */
static int task_evict_oldest_terminal(TaskStore *st) {
  for (size_t i = 0; i < st->count; ++i) {
    if (!task_state_terminal(st->items[i].state)) continue;
    memmove(&st->items[i], &st->items[i + 1],
            (st->count - i - 1) * sizeof(TaskItem));
    st->count--;
    return 0;
  }
  return -1;
}

int rt_task_apply_event(const char *type, const char *payload_json) {
  TaskStore *st;
  JsonRef payload, arts, input;
  char task_id[RT_SMALL] = "";
  long long updated = 0;
  int idx;
  if (!rt_task_is_event_type(type) || !payload_json ||
      json_ref_top_object(payload_json, &payload) != 0)
    return -1;
  st = task_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return task_store_release_result(-1);
  if (strcmp(type, "task_created") == 0) {
    if (st->count >= TASK_MAX_ITEMS &&
        task_evict_oldest_terminal(st) != 0)
      return task_store_release_result(-1);
    TaskItem *t = &st->items[st->count];
    if (json_ref_object_get_string(&payload, "task_id", t->task_id, sizeof(t->task_id)) != 0 ||
        task_index_of(st, t->task_id) >= 0 ||
        json_ref_object_get_string(&payload, "context_id", t->context_id, sizeof(t->context_id)) != 0 ||
        rt_task_json_status(&payload, t->state, sizeof(t->state)) != 0 ||
        !task_state_ok(t->state)) return task_store_release_result(-1);
    if (json_ref_object_get_string(&payload, "kind", t->kind, sizeof(t->kind)) != 0 || !t->kind[0])
      snprintf(t->kind, sizeof(t->kind), "%s", "work");
    if (json_ref_object_get(&payload, "input", &input) == 0) value_compact(&input, t->input, sizeof(t->input));
    else snprintf(t->input, sizeof(t->input), "null");
    (void)rt_task_json_state_string(&payload, "work", t->work, sizeof(t->work));
    (void)rt_task_json_state_string(&payload, "done_when", t->done_when, sizeof(t->done_when));
    (void)rt_task_json_state_string(&payload, "working_memory",
                                    t->working_memory,
                                    sizeof(t->working_memory));
    (void)rt_task_json_state_string(&payload, "affair_id", t->affair_id, sizeof(t->affair_id));
    (void)rt_task_json_state_string(&payload, "delegate", t->delegate, sizeof(t->delegate));
    {
      JsonRef ext;
      if (rt_task_json_state_value(&payload, "external", &ext) == 0)
        (void)value_compact(&ext, t->external, sizeof(t->external));
      else snprintf(t->external, sizeof(t->external), "null");
    }
    (void)json_ref_object_get_string(&payload, "created_event_id", t->created_event_id, sizeof(t->created_event_id));
    (void)json_ref_object_get_long(&payload, "created_ms", &t->created_ms);
    {
      JsonRef state_obj;
      if (json_ref_object_get_object(&payload, "state", &state_obj) == 0)
        (void)json_ref_object_get_long(&state_obj, "updated_ms", &t->updated_ms);
      if (!t->updated_ms) (void)json_ref_object_get_long(&payload, "updated_ms", &t->updated_ms);
    }
    if (rt_task_json_state_array(&payload, "artifacts", &arts) == 0) value_compact(&arts, t->artifacts, sizeof(t->artifacts));
    else snprintf(t->artifacts, sizeof(t->artifacts), "[]");
    t->work_rev = t->work[0] ? 1 : 0;
    st->count++;
    return task_store_release_result(write_store(st));
  }
  if (json_ref_object_get_string(&payload, "task_id", task_id, sizeof(task_id)) != 0)
    return task_store_release_result(-1);
  idx = task_index_of(st, task_id);
  if (idx < 0) return task_store_release_result(-1);
  TaskItem *t = &st->items[idx];
  if (strcmp(type, "task_working_memory_appended") == 0) {
    char incoming[TASK_WORKING_MEMORY_MAX] = "";
    size_t current_len, incoming_len, needed;
    if (json_ref_object_get_string(&payload, "working_memory", incoming,
                                   sizeof(incoming)) != 0 ||
        !incoming[0])
      return task_store_release_result(-1);
    current_len = strlen(t->working_memory);
    incoming_len = strlen(incoming);
    needed = current_len + (current_len ? 1U : 0U) + incoming_len;
    if (needed >= sizeof(t->working_memory))
      return task_store_release_result(-1);
    if (current_len) t->working_memory[current_len++] = '\n';
    memcpy(t->working_memory + current_len, incoming, incoming_len + 1U);
    if (json_ref_object_get_long(&payload, "updated_ms", &updated) == 0 &&
        updated > 0)
      t->updated_ms = updated;
    return task_store_release_result(write_store(st));
  }
  if (strcmp(type, "work_completed") == 0 ||
      strcmp(type, "work_blocked") == 0) {
    char context_id[RT_MED] = "", status[RT_SMALL] = "";
    long long terminal_rev = 0;
    const char *expected_status =
        strcmp(type, "work_completed") == 0 ? "completed" : "blocked";
    if (strcmp(t->kind, "work") != 0 ||
        json_ref_object_get_long(&payload, "work_rev", &terminal_rev) != 0 ||
        terminal_rev != t->work_rev ||
        json_ref_object_get_string(&payload, "context_id",
                                   context_id, sizeof(context_id)) != 0 ||
        strcmp(context_id, t->context_id) != 0 ||
        rt_task_json_status(&payload, status, sizeof(status)) != 0 ||
        strcmp(status, expected_status) != 0)
      return task_store_release_result(-1);
  }
  {
    JsonRef state_obj;
    if (json_ref_object_get_object(&payload, "state", &state_obj) == 0)
      (void)json_ref_object_get_long(&state_obj, "updated_ms", &updated);
    if (!updated) (void)json_ref_object_get_long(&payload, "updated_ms", &updated);
  }
  if (updated > 0) t->updated_ms = updated;
  if (strcmp(type, "task_updated") == 0) {
    char tmp[RT_LARGE], requested_state[RT_SMALL] = "";
    int work_mutated = 0;
    int has_work =
        rt_task_json_state_string(&payload, "work", tmp, sizeof(tmp)) == 0;
    int has_done =
        rt_task_json_state_string(&payload, "done_when", tmp, sizeof(tmp)) == 0;
    int has_requested_state =
        rt_task_json_status(&payload, requested_state,
                            sizeof(requested_state)) == 0 &&
        requested_state[0];
    /* A blocked work task is eligible for a controller again only after a
     * revision-bearing update. Merely flipping it back to an active state
     * would reuse the terminalized revision. Presence of either work field
     * makes the accepted update increment work_rev below. */
    if (strcmp(t->kind, "work") == 0 &&
        strcmp(t->state, "blocked") == 0 &&
        has_requested_state &&
        (strcmp(requested_state, "open") == 0 ||
         strcmp(requested_state, "working") == 0) &&
        !has_work && !has_done)
      return task_store_release_result(-1);
    if (has_requested_state && task_state_ok(requested_state))
      snprintf(t->state, sizeof(t->state), "%s", requested_state);
    /* tmp is read at RT_LARGE so an oversized value is still seen and the
     * revision still bumps; the field keeps as much prose as it holds.
     * The explicit precision states that clipping is intended -- these are
     * the two fields whose overflow is defined as graceful truncation. */
    if (rt_task_json_state_string(&payload, "work", tmp, sizeof(tmp)) == 0) {
      work_mutated = 1;
      snprintf(t->work, sizeof(t->work), "%.*s",
               (int)sizeof(t->work) - 1, tmp);
    }
    if (rt_task_json_state_string(&payload, "done_when", tmp, sizeof(tmp)) == 0) {
      work_mutated = 1;
      snprintf(t->done_when, sizeof(t->done_when), "%.*s",
               (int)sizeof(t->done_when) - 1, tmp);
    }
    if (work_mutated) t->work_rev = (t->work_rev > 0 ? t->work_rev : 0) + 1;
    if (rt_task_json_state_string(&payload, "affair_id", tmp, sizeof(tmp)) == 0) snprintf(t->affair_id, sizeof(t->affair_id), "%s", tmp);
    if (rt_task_json_state_string(&payload, "delegate", tmp, sizeof(tmp)) == 0) snprintf(t->delegate, sizeof(t->delegate), "%s", tmp);
    {
      JsonRef ext;
      if (rt_task_json_state_value(&payload, "external", &ext) == 0 &&
          value_compact(&ext, t->external, sizeof(t->external)) != 0)
        return task_store_release_result(-1);
    }
  } else if (rt_task_json_status(&payload, t->state, sizeof(t->state)) != 0 ||
             !task_state_ok(t->state)) {
    return task_store_release_result(-1);
  }
  if (rt_task_json_state_array(&payload, "artifacts", &arts) == 0) {
    char tmp[TASK_ARTIFACTS_MAX];
    if (value_compact(&arts, tmp, sizeof(tmp)) != 0 ||
        append_artifacts(t, tmp) != 0) return task_store_release_result(-1);
  }
  return task_store_release_result(write_store(st));
}
