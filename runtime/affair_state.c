#include "runtime.h"
#include "support/scratch_guard.h"
#include "support/timing.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AFFAIR_STATE_PATH "workspace/memory/state/affairs.json"
#define AFFAIR_MAX_NOTES 16
#define AFFAIR_MAX_TASK_IDS 16
#define AFFAIR_MAX_COUNTERS 16

typedef struct {
  char name[32];
  int64_t value;
} AffairCounter;

typedef struct {
  char ts[RT_SMALL];          /* ISO 8601 */
  char text[RT_MED];          /* stored size; longer inputs truncate on write. */
} AffairNote;

typedef struct {
  char affair_id[RT_SMALL];
  char context_id[RT_MED];
  char status[RT_SMALL];      /* active | paused | closed */
  char manage[RT_MED];        /* "what to manage" — the model-facing description */

  /* The single scheduling field. 0 means manual-only (the watcher
   * never fires this affair). Otherwise the watcher publishes an
   * affair_review envelope when now_ms >= next_review_at_ms and
   * clears the field via affair_reviewed. The speaker can re-arm via
   * `defer:"<duration>"` in its output, or the CLI via
   * `fclaw affair schedule <id> --in <duration>`. */
  long long next_review_at_ms;

  char task_ids[AFFAIR_MAX_TASK_IDS][RT_SMALL];
  size_t task_id_count;

  AffairNote notes[AFFAIR_MAX_NOTES];
  size_t note_count;

  AffairCounter counters[AFFAIR_MAX_COUNTERS];
  size_t counter_count;

  long long created_ms;
  long long updated_ms;
} AffairItem;

typedef struct {
  AffairItem items[AFFAIR_MAX_ITEMS];
  size_t count;
} AffairStore;

typedef struct {
  AffairStore store;
  char write_out[RT_XL * 4];
  char write_head[RT_XL];
} AffairScratch;

static AffairScratch g_affair_scratch;
static FcScratchGuard g_affair_guard = FC_SCRATCH_GUARD_INIT("affair_state");

static AffairStore *affair_store_borrow(void) {
  if (fc_scratch_guard_acquire(&g_affair_guard) != 0) return NULL;
  return &g_affair_scratch.store;
}

static int affair_store_release_result(int result) {
  fc_scratch_guard_release(&g_affair_guard);
  return result;
}

static void iso_ts_for_ms(long long ms, char *out, size_t out_len) {
  time_t secs = (time_t)(ms / 1000);
  struct tm tmv;
  if (!out || out_len < 21) { if (out && out_len) out[0] = '\0'; return; }
  gmtime_r(&secs, &tmv);
  strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int affair_status_canonical(const char *s, char *out, size_t out_len) {
  if (!s || !*s) { snprintf(out, out_len, "active"); return 0; }
  if (strcmp(s, "active") == 0 ||
      strcmp(s, "paused") == 0 ||
      strcmp(s, "closed") == 0) {
    snprintf(out, out_len, "%s", s);
    return 0;
  }
  return -1;
}

static int append_note_text_array(char *out, size_t out_len, size_t *pos,
                                  const AffairItem *a) {
  if (rt_append_text(out, out_len, pos, "[") != 0) return -1;
  for (size_t i = 0; i < a->note_count; ++i) {
    char ets[RT_MED], etext[RT_MED * 2], item[RT_MED * 4];
    if (json_escape(a->notes[i].ts, ets, sizeof(ets)) != 0 ||
        json_escape(a->notes[i].text, etext, sizeof(etext)) != 0)
      return -1;
    if (snprintf(item, sizeof(item),
                 "%s{\"ts\":\"%s\",\"text\":\"%s\"}",
                 i ? "," : "", ets, etext) >= (int)sizeof(item) ||
        rt_append_text(out, out_len, pos, item) != 0)
      return -1;
  }
  return rt_append_text(out, out_len, pos, "]");
}

static int append_task_ids_array(char *out, size_t out_len, size_t *pos,
                                 const AffairItem *a) {
  if (rt_append_text(out, out_len, pos, "[") != 0) return -1;
  for (size_t i = 0; i < a->task_id_count; ++i) {
    char etask[RT_MED], item[RT_MED + 8];
    if (json_escape(a->task_ids[i], etask, sizeof(etask)) != 0) return -1;
    if (snprintf(item, sizeof(item), "%s\"%s\"",
                 i ? "," : "", etask) >= (int)sizeof(item) ||
        rt_append_text(out, out_len, pos, item) != 0)
      return -1;
  }
  return rt_append_text(out, out_len, pos, "]");
}

static int append_counters_array(char *out, size_t out_len, size_t *pos,
                                 const AffairItem *a) {
  if (rt_append_text(out, out_len, pos, "[") != 0) return -1;
  for (size_t i = 0; i < a->counter_count; ++i) {
    char ename[32 * 6 + 1], item[256];
    if (json_escape(a->counters[i].name, ename, sizeof(ename)) != 0)
      return -1;
    if (snprintf(item, sizeof(item),
                 "%s{\"name\":\"%s\",\"value\":%" PRId64 "}",
                 i ? "," : "", ename, a->counters[i].value) >= (int)sizeof(item) ||
        rt_append_text(out, out_len, pos, item) != 0)
      return -1;
  }
  return rt_append_text(out, out_len, pos, "]");
}

static int counter_index(const AffairItem *a, const char *name) {
  if (!a || !name || !*name) return -1;
  for (size_t i = 0; i < a->counter_count; ++i)
    if (strcmp(a->counters[i].name, name) == 0) return (int)i;
  return -1;
}

static int load_store(AffairStore *st) {
  char *text = NULL;
  JsonRef root, affairs;
  memset(st, 0, sizeof(*st));
  if (fs_read_text(AFFAIR_STATE_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_array(&root, "affairs", &affairs) != 0) {
    free(text);
    return 0;
  }
  for (size_t i = 0; i < json_ref_array_size(&affairs) && st->count < AFFAIR_MAX_ITEMS; ++i) {
    JsonRef item, notes_arr, tasks_arr, counters_arr;
    AffairItem *a = &st->items[st->count];
    char raw_status[RT_SMALL] = "";
    if (json_ref_array_get(&affairs, i, &item) != 0 || item.type != JSON_REF_OBJECT)
      continue;
    if (json_ref_object_get_string(&item, "affair_id", a->affair_id, sizeof(a->affair_id)) != 0 ||
        json_ref_object_get_string(&item, "context_id", a->context_id, sizeof(a->context_id)) != 0 ||
        json_ref_object_get_string(&item, "manage", a->manage, sizeof(a->manage)) != 0 ||
        !a->manage[0])
      continue;

    (void)json_ref_object_get_string(&item, "status", raw_status, sizeof(raw_status));
    if (affair_status_canonical(raw_status, a->status, sizeof(a->status)) != 0)
      snprintf(a->status, sizeof(a->status), "active");

    a->next_review_at_ms = 0;
    (void)json_ref_object_get_long(&item, "next_review_at_ms", &a->next_review_at_ms);

    /* task_ids array. */
    a->task_id_count = 0;
    if (json_ref_object_get_array(&item, "task_ids", &tasks_arr) == 0) {
      size_t tn = json_ref_array_size(&tasks_arr);
      for (size_t k = 0; k < tn && a->task_id_count < AFFAIR_MAX_TASK_IDS; ++k) {
        JsonRef t;
        char buf[RT_SMALL] = "";
        if (json_ref_array_get(&tasks_arr, k, &t) != 0 ||
            t.type != JSON_REF_STRING ||
            json_ref_string_copy(&t, buf, sizeof(buf)) != 0 ||
            !buf[0]) continue;
        snprintf(a->task_ids[a->task_id_count++], RT_SMALL, "%s", buf);
      }
    }

    a->note_count = 0;
    if (json_ref_object_get_array(&item, "notes", &notes_arr) == 0) {
      size_t nn = json_ref_array_size(&notes_arr);
      for (size_t k = 0; k < nn && a->note_count < AFFAIR_MAX_NOTES; ++k) {
        JsonRef n;
        char ts[RT_SMALL] = "", txt[RT_MED] = "";
        if (json_ref_array_get(&notes_arr, k, &n) != 0 ||
            n.type != JSON_REF_OBJECT) continue;
        (void)json_ref_object_get_string(&n, "ts", ts, sizeof(ts));
        if (json_ref_object_get_string(&n, "text", txt, sizeof(txt)) != 0 ||
            !txt[0]) continue;
        snprintf(a->notes[a->note_count].ts, sizeof(a->notes[a->note_count].ts), "%s", ts);
        snprintf(a->notes[a->note_count].text, sizeof(a->notes[a->note_count].text), "%s", txt);
        a->note_count++;
      }
    }

    a->counter_count = 0;
    if (json_ref_object_get_array(&item, "counters", &counters_arr) == 0) {
      size_t cn = json_ref_array_size(&counters_arr);
      for (size_t k = 0; k < cn && a->counter_count < AFFAIR_MAX_COUNTERS; ++k) {
        JsonRef c;
        char name[sizeof(a->counters[0].name)] = "";
        long long value = 0;
        if (json_ref_array_get(&counters_arr, k, &c) != 0 ||
            c.type != JSON_REF_OBJECT ||
            json_ref_object_get_string(&c, "name", name, sizeof(name)) != 0 ||
            !name[0] || counter_index(a, name) >= 0 ||
            json_ref_object_get_long(&c, "value", &value) != 0)
          continue;
        snprintf(a->counters[a->counter_count].name,
                 sizeof(a->counters[a->counter_count].name), "%s", name);
        a->counters[a->counter_count].value = (int64_t)value;
        a->counter_count++;
      }
    }

    (void)json_ref_object_get_long(&item, "created_ms", &a->created_ms);
    (void)json_ref_object_get_long(&item, "updated_ms", &a->updated_ms);
    st->count++;
  }
  free(text);
  return 0;
}

static int write_store(const AffairStore *st) {
  char *out = g_affair_scratch.write_out;
  char *head = g_affair_scratch.write_head;
  size_t pos = 0;
  if (fs_mkdir_p("workspace/memory/state") != 0) return -1;
  if (rt_append_text(out, sizeof(g_affair_scratch.write_out), &pos,
                  "{\"affairs\":[") != 0) return -1;
  for (size_t i = 0; i < st->count; ++i) {
    const AffairItem *a = &st->items[i];
    char eid[RT_MED], ectx[RT_MED], emanage[RT_MED * 2], estatus[RT_MED];
    char tail[RT_MED];
    if (json_escape(a->affair_id, eid, sizeof(eid)) != 0 ||
        json_escape(a->context_id, ectx, sizeof(ectx)) != 0 ||
        json_escape(a->manage, emanage, sizeof(emanage)) != 0 ||
        json_escape(a->status, estatus, sizeof(estatus)) != 0)
      return -1;
    if (snprintf(head, sizeof(g_affair_scratch.write_head),
                 "%s{\"affair_id\":\"%s\",\"context_id\":\"%s\","
                 "\"status\":\"%s\",\"manage\":\"%s\","
                 "\"next_review_at_ms\":%lld,\"task_ids\":",
                 i ? "," : "", eid, ectx, estatus, emanage,
                 a->next_review_at_ms) >=
                     (int)sizeof(g_affair_scratch.write_head) ||
        rt_append_text(out, sizeof(g_affair_scratch.write_out), &pos, head) != 0 ||
        append_task_ids_array(out, sizeof(g_affair_scratch.write_out), &pos, a) != 0 ||
        rt_append_text(out, sizeof(g_affair_scratch.write_out), &pos, ",\"notes\":") != 0 ||
        append_note_text_array(out, sizeof(g_affair_scratch.write_out), &pos, a) != 0 ||
        rt_append_text(out, sizeof(g_affair_scratch.write_out), &pos, ",\"counters\":") != 0 ||
        append_counters_array(out, sizeof(g_affair_scratch.write_out), &pos, a) != 0)
      return -1;
    if (snprintf(tail, sizeof(tail),
                 ",\"created_ms\":%lld,\"updated_ms\":%lld}",
                 a->created_ms, a->updated_ms) >= (int)sizeof(tail) ||
        rt_append_text(out, sizeof(g_affair_scratch.write_out), &pos, tail) != 0)
      return -1;
  }
  if (rt_append_text(out, sizeof(g_affair_scratch.write_out), &pos, "]}\n") != 0)
    return -1;
  return fs_write_text_atomic(AFFAIR_STATE_PATH, out);
}

static int index_of(const AffairStore *st, const char *affair_id) {
  if (!st || !affair_id || !*affair_id) return -1;
  for (size_t i = 0; i < st->count; ++i)
    if (strcmp(st->items[i].affair_id, affair_id) == 0) return (int)i;
  return -1;
}

static void move_to_front(AffairStore *st, size_t idx) {
  AffairItem tmp;
  if (!st || idx == 0 || idx >= st->count) return;
  tmp = st->items[idx];
  memmove(&st->items[1], &st->items[0], idx * sizeof(st->items[0]));
  st->items[0] = tmp;
}

static void context_from_ctx(RtContext *ctx, char *out, size_t out_len) {
  if (ctx && ctx->context_id[0]) snprintf(out, out_len, "%s", ctx->context_id);
  else if (ctx && ctx->origin_channel[0] && ctx->origin_adapter_id[0])
    snprintf(out, out_len, "chat:%s:%s", ctx->origin_channel, ctx->origin_adapter_id);
  else if (ctx && ctx->origin_channel[0])
    snprintf(out, out_len, "chat:%s", ctx->origin_channel);
  else snprintf(out, out_len, "chat:unknown");
}

static void note_append(AffairItem *a, long long ms, const char *text) {
  AffairNote *slot;
  if (!a || !text || !*text) return;
  if (a->note_count >= AFFAIR_MAX_NOTES) {
    memmove(&a->notes[0], &a->notes[1],
            (AFFAIR_MAX_NOTES - 1) * sizeof(a->notes[0]));
    a->note_count = AFFAIR_MAX_NOTES - 1;
  }
  slot = &a->notes[a->note_count++];
  iso_ts_for_ms(ms ? ms : timing_system_wall_ms(), slot->ts, sizeof(slot->ts));
  snprintf(slot->text, sizeof(slot->text), "%s", text);
}

static int task_id_present(const AffairItem *a, const char *task_id) {
  for (size_t i = 0; i < a->task_id_count; ++i)
    if (strcmp(a->task_ids[i], task_id) == 0) return 1;
  return 0;
}

static void task_id_link(AffairItem *a, const char *task_id) {
  if (!a || !task_id || !*task_id || task_id_present(a, task_id)) return;
  if (a->task_id_count >= AFFAIR_MAX_TASK_IDS) {
    memmove(&a->task_ids[0], &a->task_ids[1],
            (AFFAIR_MAX_TASK_IDS - 1) * sizeof(a->task_ids[0]));
    a->task_id_count = AFFAIR_MAX_TASK_IDS - 1;
  }
  snprintf(a->task_ids[a->task_id_count++], RT_SMALL, "%s", task_id);
}

int rt_affair_is_event_type(const char *type) {
  if (!type) return 0;
  return strcmp(type, "affair_patch") == 0 ||
         strcmp(type, "affair_note_added") == 0 ||
         strcmp(type, "affair_counter_bumped") == 0 ||
         strcmp(type, "affair_task_linked") == 0 ||
         strcmp(type, "affair_reviewed") == 0;
}

int rt_affair_reference_in_context(const char *context_id, const char *affair_id) {
  AffairStore *st;
  if (!context_id || !*context_id || !affair_id || !*affair_id) return 0;
  st = affair_store_borrow();
  if (!st) return 0;
  if (load_store(st) != 0) return affair_store_release_result(0);
  for (size_t i = 0; i < st->count; ++i)
    if (strcmp(st->items[i].context_id, context_id) == 0 &&
        strcmp(st->items[i].affair_id, affair_id) == 0)
      return affair_store_release_result(1);
  return affair_store_release_result(0);
}

int rt_affair_first_active_in_context(const char *context_id,
                                      char *out, size_t out_len) {
  AffairStore *st;
  if (!context_id || !*context_id || !out || out_len == 0) return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  out[0] = '\0';
  for (size_t i = 0; i < st->count; ++i) {
    AffairItem *a = &st->items[i];
    if (strcmp(a->context_id, context_id) == 0 &&
        strcmp(a->status, "active") == 0) {
      snprintf(out, out_len, "%s", a->affair_id);
      return affair_store_release_result(out[0] ? 0 : -1);
    }
  }
  return affair_store_release_result(-1);
}

int rt_affair_normalize_event(RtContext *ctx, const JsonRef *payload,
                              int event_seq, char *out, size_t out_len) {
  char action[RT_SMALL] = "", manage[RT_MED] = "", note_text[RT_MED] = "";
  char raw_status[RT_SMALL] = "", status[RT_SMALL] = "active", affair_id[RT_SMALL] = "";
  char context[RT_MED], eaction[RT_MED], emanage[RT_MED * 2], enote[RT_MED * 2];
  char estatus[RT_MED], eaffair[RT_MED], ectx[RT_MED];
  long long ms = timing_system_wall_ms();
  long long next_review_at_ms = 0;
  int has_next_review_at = 0;
  if (!payload || payload->type != JSON_REF_OBJECT || !out || out_len == 0)
    return -1;
  if (json_ref_object_get_string(payload, "action", action, sizeof(action)) != 0 ||
      !action[0])
    return -1;
  if (strcmp(action, "none") == 0) {
    snprintf(out, out_len, "{}");
    return 1;
  }
  if (strcmp(action, "create") != 0 && strcmp(action, "link") != 0 &&
      strcmp(action, "update") != 0)
    return -1;
  (void)json_ref_object_get_string(payload, "manage", manage, sizeof(manage));
  (void)json_ref_object_get_string(payload, "note_text", note_text, sizeof(note_text));
  (void)json_ref_object_get_string(payload, "status", raw_status, sizeof(raw_status));
  if (affair_status_canonical(raw_status, status, sizeof(status)) != 0)
    snprintf(status, sizeof(status), "active");
  if (json_ref_object_get_long(payload, "next_review_at_ms", &next_review_at_ms) == 0 &&
      next_review_at_ms >= 0)
    has_next_review_at = 1;
  context_from_ctx(ctx, context, sizeof(context));
  if (strcmp(action, "create") == 0) {
    if (!manage[0]) return -1;
    snprintf(affair_id, sizeof(affair_id), "aff_%s_%06d",
             ctx && ctx->run_id[0] ? ctx->run_id : "run", event_seq);
  } else {
    if (json_ref_object_get_string(payload, "affair_id", affair_id, sizeof(affair_id)) != 0 ||
        !rt_affair_reference_in_context(context, affair_id))
      return -1;
  }
  if (json_escape(action, eaction, sizeof(eaction)) != 0 ||
      json_escape(manage, emanage, sizeof(emanage)) != 0 ||
      json_escape(note_text, enote, sizeof(enote)) != 0 ||
      json_escape(status, estatus, sizeof(estatus)) != 0 ||
      json_escape(affair_id, eaffair, sizeof(eaffair)) != 0 ||
      json_escape(context, ectx, sizeof(ectx)) != 0)
    return -1;
  if (has_next_review_at) {
    return snprintf(out, out_len,
                    "{\"action\":\"%s\",\"affair_id\":\"%s\","
                    "\"context_id\":\"%s\",\"manage\":\"%s\","
                    "\"note_text\":\"%s\",\"status\":\"%s\","
                    "\"next_review_at_ms\":%lld,"
                    "\"created_ms\":%lld,\"updated_ms\":%lld}",
                    eaction, eaffair, ectx, emanage, enote, estatus,
                    next_review_at_ms, ms, ms) >= (int)out_len ? -1 : 0;
  }
  return snprintf(out, out_len,
                  "{\"action\":\"%s\",\"affair_id\":\"%s\","
                  "\"context_id\":\"%s\",\"manage\":\"%s\","
                  "\"note_text\":\"%s\",\"status\":\"%s\","
                  "\"created_ms\":%lld,\"updated_ms\":%lld}",
                  eaction, eaffair, ectx, emanage, enote, estatus,
                  ms, ms) >= (int)out_len ? -1 : 0;
}

static int apply_affair_patch(const JsonRef *payload) {
  AffairStore *st;
  char action[RT_SMALL] = "", affair_id[RT_SMALL] = "", context_id[RT_MED] = "";
  char manage[RT_MED] = "", note_text[RT_MED] = "";
  char raw_status[RT_SMALL] = "", status[RT_SMALL] = "active";
  long long created_ms = 0, updated_ms = 0;
  int idx;
  if (json_ref_object_get_string(payload, "action", action, sizeof(action)) != 0 ||
      json_ref_object_get_string(payload, "affair_id", affair_id, sizeof(affair_id)) != 0 ||
      json_ref_object_get_string(payload, "context_id", context_id, sizeof(context_id)) != 0)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  (void)json_ref_object_get_string(payload, "manage", manage, sizeof(manage));
  (void)json_ref_object_get_string(payload, "note_text", note_text, sizeof(note_text));
  (void)json_ref_object_get_string(payload, "status", raw_status, sizeof(raw_status));
  if (affair_status_canonical(raw_status, status, sizeof(status)) != 0)
    snprintf(status, sizeof(status), "active");
  (void)json_ref_object_get_long(payload, "created_ms", &created_ms);
  (void)json_ref_object_get_long(payload, "updated_ms", &updated_ms);
  if (!updated_ms) updated_ms = timing_system_wall_ms();
  if (strcmp(action, "create") == 0) {
    if (st->count >= AFFAIR_MAX_ITEMS || !manage[0] || index_of(st, affair_id) >= 0)
      return affair_store_release_result(-1);
    if (st->count > 0)
      memmove(&st->items[1], &st->items[0], st->count * sizeof(st->items[0]));
    AffairItem *a = &st->items[0];
    memset(a, 0, sizeof(*a));
    snprintf(a->affair_id, sizeof(a->affair_id), "%s", affair_id);
    snprintf(a->context_id, sizeof(a->context_id), "%s", context_id);
    snprintf(a->manage, sizeof(a->manage), "%s", manage);
    snprintf(a->status, sizeof(a->status), "%s", status);
    a->created_ms = created_ms ? created_ms : updated_ms;
    a->updated_ms = updated_ms;
    a->next_review_at_ms = 0;
    {
      long long nra = 0;
      if (json_ref_object_get_long(payload, "next_review_at_ms", &nra) == 0 && nra > 0)
        a->next_review_at_ms = nra;
    }
    if (note_text[0]) note_append(a, updated_ms, note_text);
    st->count++;
    return affair_store_release_result(write_store(st));
  }
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  AffairItem *a = &st->items[idx];
  if (strcmp(a->context_id, context_id) != 0)
    return affair_store_release_result(-1);
  if (strcmp(action, "update") == 0) {
    if (manage[0]) snprintf(a->manage, sizeof(a->manage), "%s", manage);
    if (note_text[0]) note_append(a, updated_ms, note_text);
    snprintf(a->status, sizeof(a->status), "%s", status);
    {
      long long nra = 0;
      if (json_ref_object_get_long(payload, "next_review_at_ms", &nra) == 0 && nra >= 0)
        a->next_review_at_ms = nra;
    }
  }
  a->updated_ms = updated_ms;
  move_to_front(st, (size_t)idx);
  return affair_store_release_result(write_store(st));
}

static int apply_note_added(const JsonRef *payload) {
  AffairStore *st;
  char affair_id[RT_SMALL] = "";
  /* Read into a bigger local buffer so long LLM notes don't blow past
   * json_ref_object_get_string's cap check; note_append then truncates
   * to the AffairNote's fixed storage. Silent truncation is fine here:
   * notes are meant to be concise summaries anyway (see result_manager
   * prompt guidance), and the full worker output remains in the event
   * log for humans who want it. */
  char note_text[RT_LARGE] = "";
  long long ms = 0;
  int idx;
  if (json_ref_object_get_string(payload, "affair_id", affair_id, sizeof(affair_id)) != 0 ||
      json_ref_object_get_string(payload, "text", note_text, sizeof(note_text)) != 0 ||
      !note_text[0])
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  (void)json_ref_object_get_long(payload, "ts_ms", &ms);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  note_append(&st->items[idx], ms, note_text);
  st->items[idx].updated_ms = ms ? ms : timing_system_wall_ms();
  return affair_store_release_result(write_store(st));
}

static int apply_counter_bump(const JsonRef *payload) {
  AffairStore *st;
  JsonRef delta_ref, set_ref;
  char affair_id[RT_SMALL] = "", name[32] = "";
  long long raw = 0;
  int has_delta, has_set, idx, cidx;
  int64_t value;
  if (json_ref_object_get_string(payload, "affair_id", affair_id,
                                 sizeof(affair_id)) != 0 ||
      json_ref_object_get_string(payload, "name", name, sizeof(name)) != 0 ||
      !name[0])
    return -1;
  has_delta = json_ref_object_get(payload, "delta", &delta_ref) == 0;
  has_set = json_ref_object_get(payload, "set", &set_ref) == 0;
  if (has_delta == has_set ||
      json_ref_object_get_long(payload, has_delta ? "delta" : "set", &raw) != 0)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  cidx = counter_index(&st->items[idx], name);
  if (cidx < 0) {
    if (st->items[idx].counter_count >= AFFAIR_MAX_COUNTERS)
      return affair_store_release_result(-1);
    cidx = (int)st->items[idx].counter_count;
    snprintf(st->items[idx].counters[cidx].name,
             sizeof(st->items[idx].counters[cidx].name), "%s", name);
    st->items[idx].counters[cidx].value = 0;
    st->items[idx].counter_count++;
  }
  value = st->items[idx].counters[cidx].value;
  if (has_delta) {
    int64_t delta = (int64_t)raw;
    if ((delta > 0 && value > INT64_MAX - delta) ||
        (delta < 0 && value < INT64_MIN - delta))
      return affair_store_release_result(-1);
    value += delta;
  } else {
    value = (int64_t)raw;
  }
  st->items[idx].counters[cidx].value = value;
  st->items[idx].updated_ms = timing_system_wall_ms();
  return affair_store_release_result(write_store(st));
}

static int apply_task_linked(const JsonRef *payload) {
  AffairStore *st;
  char affair_id[RT_SMALL] = "", task_id[RT_SMALL] = "";
  int idx;
  if (json_ref_object_get_string(payload, "affair_id", affair_id, sizeof(affair_id)) != 0 ||
      json_ref_object_get_string(payload, "task_id", task_id, sizeof(task_id)) != 0 ||
      !task_id[0])
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  task_id_link(&st->items[idx], task_id);
  st->items[idx].updated_ms = timing_system_wall_ms();
  return affair_store_release_result(write_store(st));
}

static int apply_reviewed(const JsonRef *payload) {
  /* The watcher fired this affair. Clear next_review_at_ms so it won't
   * fire again until something explicitly re-arms it (the speaker via
   * affair_patch action:update with next_review_at_ms, or the CLI via
   * fclaw affair schedule). */
  AffairStore *st;
  char affair_id[RT_SMALL] = "";
  long long ms = 0;
  int idx;
  if (json_ref_object_get_string(payload, "affair_id", affair_id, sizeof(affair_id)) != 0)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  (void)json_ref_object_get_long(payload, "ms", &ms);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  st->items[idx].next_review_at_ms = 0;
  st->items[idx].updated_ms = ms ? ms : timing_system_wall_ms();
  return affair_store_release_result(write_store(st));
}

int rt_affair_apply_event(const char *type, const char *payload_json) {
  JsonRef payload;
  if (!rt_affair_is_event_type(type) || !payload_json ||
      json_ref_top_object(payload_json, &payload) != 0)
    return -1;
  if (strcmp(type, "affair_patch") == 0)         return apply_affair_patch(&payload);
  if (strcmp(type, "affair_note_added") == 0)    return apply_note_added(&payload);
  if (strcmp(type, "affair_counter_bumped") == 0) return apply_counter_bump(&payload);
  if (strcmp(type, "affair_task_linked") == 0)   return apply_task_linked(&payload);
  if (strcmp(type, "affair_reviewed") == 0)      return apply_reviewed(&payload);
  return -1;
}

int rt_affairs_active_state_json(const char *context_id, char *out, size_t out_len) {
  AffairStore *st;
  size_t pos = 0;
  int first = 1;
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  if (rt_append_text(out, out_len, &pos, "{\"active\":[") != 0)
    return affair_store_release_result(-1);
  for (size_t i = 0; i < st->count; ++i) {
    AffairItem *a = &st->items[i];
    char eid[RT_MED], emanage[RT_MED * 2], estatus[RT_MED], item[RT_LARGE * 2];
    char notes_buf[RT_MED * 4] = "[]";
    char counters_buf[RT_LARGE] = "[]";
    if (context_id && *context_id && strcmp(a->context_id, context_id) != 0) continue;
    if (strcmp(a->status, "active") != 0) continue;
    if (json_escape(a->affair_id, eid, sizeof(eid)) != 0 ||
        json_escape(a->manage, emanage, sizeof(emanage)) != 0 ||
        json_escape(a->status, estatus, sizeof(estatus)) != 0)
      return affair_store_release_result(-1);
    /* Include the last 3 notes verbatim so the LLM can notice repeats
     * and reason about whether prior reviews made progress. Cap at 3
     * to keep the projection compact — the full note history stays
     * on disk in affairs.json. */
    {
      size_t start = a->note_count > 3 ? a->note_count - 3 : 0;
      size_t np = 0;
      notes_buf[0] = '[';
      np = 1;
      for (size_t k = start; k < a->note_count; ++k) {
        char ets[RT_SMALL * 2], etx[RT_MED * 2];
        char entry[RT_MED * 3];
        int en;
        if (json_escape(a->notes[k].ts, ets, sizeof(ets)) != 0 ||
            json_escape(a->notes[k].text, etx, sizeof(etx)) != 0)
          continue;
        en = snprintf(entry, sizeof(entry),
                      "%s{\"ts\":\"%s\",\"text\":\"%s\"}",
                      k == start ? "" : ",", ets, etx);
        if (en < 0 || np + (size_t)en + 2 >= sizeof(notes_buf)) break;
        memcpy(notes_buf + np, entry, (size_t)en);
        np += (size_t)en;
      }
      notes_buf[np++] = ']';
      notes_buf[np] = '\0';
    }
    {
      size_t cp = 0;
      if (append_counters_array(counters_buf, sizeof(counters_buf), &cp, a) != 0)
        return affair_store_release_result(-1);
    }
    if (snprintf(item, sizeof(item),
                 "%s{\"affair_id\":\"%s\",\"manage\":\"%s\","
                 "\"status\":\"%s\",\"note_count\":%zu,"
                 "\"recent_notes\":%s,\"counter_count\":%zu,"
                 "\"counters\":%s}",
                 first ? "" : ",", eid, emanage, estatus,
                 a->note_count, notes_buf, a->counter_count,
                 counters_buf) >= (int)sizeof(item) ||
        rt_append_text(out, out_len, &pos, item) != 0)
      return affair_store_release_result(-1);
    first = 0;
  }
  return affair_store_release_result(rt_append_text(out, out_len, &pos, "]}"));
}

/* ----- new public helpers for CLI + watcher ----- */

int rt_affair_create_manual(const char *context_id, const char *manage,
                            long long initial_next_review_at_ms,
                            char *out_affair_id, size_t out_len) {
  AffairStore *st;
  char id[RT_SMALL];
  long long ms;
  int seq = 1;
  if (!context_id || !*context_id || !manage || !*manage ||
      !out_affair_id || out_len < 24)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  /* CLI affair ids: aff_cli_<seq> — seq is max-existing + 1. */
  for (size_t i = 0; i < st->count; ++i) {
    const char *p = st->items[i].affair_id;
    int n;
    if (strncmp(p, "aff_cli_", 8) != 0) continue;
    if (sscanf(p + 8, "%d", &n) == 1 && n >= seq) seq = n + 1;
  }
  snprintf(id, sizeof(id), "aff_cli_%06d", seq);
  if (st->count >= AFFAIR_MAX_ITEMS) return affair_store_release_result(-1);
  if (st->count > 0)
    memmove(&st->items[1], &st->items[0], st->count * sizeof(st->items[0]));
  AffairItem *a = &st->items[0];
  memset(a, 0, sizeof(*a));
  ms = timing_system_wall_ms();
  snprintf(a->affair_id, sizeof(a->affair_id), "%s", id);
  snprintf(a->context_id, sizeof(a->context_id), "%s", context_id);
  snprintf(a->manage, sizeof(a->manage), "%s", manage);
  snprintf(a->status, sizeof(a->status), "active");
  a->created_ms = ms;
  a->updated_ms = ms;
  a->next_review_at_ms = initial_next_review_at_ms > 0 ? initial_next_review_at_ms : 0;
  st->count++;
  if (write_store(st) != 0) return affair_store_release_result(-1);
  snprintf(out_affair_id, out_len, "%s", id);
  return affair_store_release_result(0);
}

int rt_affair_set_status(const char *affair_id, const char *status) {
  AffairStore *st;
  char canon[RT_SMALL];
  int idx;
  if (!affair_id || !*affair_id || !status ||
      affair_status_canonical(status, canon, sizeof(canon)) != 0)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  snprintf(st->items[idx].status, sizeof(st->items[idx].status), "%s", canon);
  st->items[idx].updated_ms = timing_system_wall_ms();
  return affair_store_release_result(write_store(st));
}

int rt_affair_set_next_review_at(const char *affair_id, long long next_review_at_ms) {
  AffairStore *st;
  int idx;
  if (!affair_id || !*affair_id || next_review_at_ms < 0) return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  st->items[idx].next_review_at_ms = next_review_at_ms;
  st->items[idx].updated_ms = timing_system_wall_ms();
  return affair_store_release_result(write_store(st));
}

int rt_affair_get_manage(const char *affair_id, char *out, size_t out_len,
                         char *context_id_out, size_t context_id_len) {
  AffairStore *st;
  int idx;
  if (!affair_id || !*affair_id || !out || out_len == 0)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  snprintf(out, out_len, "%s", st->items[idx].manage);
  if (context_id_out && context_id_len)
    snprintf(context_id_out, context_id_len, "%s", st->items[idx].context_id);
  return affair_store_release_result(0);
}

int rt_affair_review_input_json(const char *affair_id, char *out, size_t out_len) {
  AffairStore *st;
  size_t pos = 0;
  char eid[RT_MED], emanage[RT_MED * 2];
  AffairItem *a;
  int idx;
  if (!affair_id || !*affair_id || !out || out_len == 0)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  a = &st->items[idx];
  if (json_escape(a->affair_id, eid, sizeof(eid)) != 0 ||
      json_escape(a->manage, emanage, sizeof(emanage)) != 0)
    return affair_store_release_result(-1);
  out[0] = '\0';
  {
    char head[RT_LARGE];
    if (snprintf(head, sizeof(head),
                 "{\"affair_id\":\"%s\",\"manage\":\"%s\",\"notes\":",
                 eid, emanage) >= (int)sizeof(head) ||
        rt_append_text(out, out_len, &pos, head) != 0 ||
        append_note_text_array(out, out_len, &pos, a) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"counters\":") != 0 ||
        append_counters_array(out, out_len, &pos, a) != 0 ||
        rt_append_text(out, out_len, &pos, ",\"task_ids\":") != 0 ||
        append_task_ids_array(out, out_len, &pos, a) != 0 ||
        rt_append_text(out, out_len, &pos, "}") != 0)
      return affair_store_release_result(-1);
  }
  return affair_store_release_result(0);
}

int rt_affair_list_due(long long now, char ids[][RT_SMALL], size_t max,
                       size_t *out_count) {
  AffairStore *st;
  size_t count = 0;
  if (!ids || !out_count || max == 0) return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  *out_count = 0;
  for (size_t i = 0; i < st->count && count < max; ++i) {
    AffairItem *a = &st->items[i];
    /* The one-line decision. Status active, scheduled (non-zero),
     * past due. Nothing smarter. */
    if (strcmp(a->status, "active") != 0) continue;
    if (a->next_review_at_ms == 0) continue;
    if (now < a->next_review_at_ms) continue;
    snprintf(ids[count], RT_SMALL, "%s", a->affair_id);
    count++;
  }
  *out_count = count;
  return affair_store_release_result(0);
}

int rt_affair_context_id_of(const char *affair_id, char *out, size_t out_len) {
  AffairStore *st;
  int idx;
  if (!affair_id || !*affair_id || !out || out_len == 0)
    return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  idx = index_of(st, affair_id);
  if (idx < 0) return affair_store_release_result(-1);
  snprintf(out, out_len, "%s", st->items[idx].context_id);
  return affair_store_release_result(0);
}

int rt_affair_each(int (*visitor)(const char *affair_id, const char *context_id,
                                  const char *manage, const char *status,
                                  long long next_review_at_ms,
                                  size_t note_count, size_t task_id_count,
                                  void *user),
                   void *user) {
  AffairStore *st;
  if (!visitor) return -1;
  st = affair_store_borrow();
  if (!st) return -1;
  if (load_store(st) != 0) return affair_store_release_result(-1);
  for (size_t i = 0; i < st->count; ++i) {
    AffairItem *a = &st->items[i];
    if (visitor(a->affair_id, a->context_id, a->manage, a->status,
                a->next_review_at_ms,
                a->note_count, a->task_id_count, user) != 0)
      return affair_store_release_result(-1);
  }
  return affair_store_release_result(0);
}
