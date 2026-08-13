/* Memory subsystem.
 *
 * Durable truth remains append-only:
 *   workspace/memory/memory.jsonl
 *
 * Conversation compaction state is a materialized view:
 *   workspace/memory/state/summary_state.json
 *
 * The runtime owns message ids, compaction windows, cutoff validation,
 * and summary-state commits. The LLM compactor only proposes updated
 * summary text for a runtime-selected request.
 */

#include "runtime.h"
#include "support/heap_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MEM_DIR           "workspace/memory"
#define MEM_LOG           "workspace/memory/memory.jsonl"
#define MEM_STATE         "workspace/memory/memory.json"
#define MEM_SUMMARY_DIR   "workspace/memory/state"
#define MEM_SUMMARY_STATE "workspace/memory/state/summary_state.json"
#define MEMORY_LOG_TAIL_BYTES (1024U * 1024U)

typedef struct {
  char message_id[RT_SMALL];
  char context_id[RT_MED];
  char role[RT_SMALL];
  char text[RT_LARGE];
  size_t chars;
} MemoryMessage;

typedef struct {
  MemoryMessage *items;
  size_t count;
  size_t cap;
} MemoryMessages;

typedef struct {
  char context_id[RT_MED];
  char conversation_summary[RT_LARGE];
  char recent_summary[RT_LARGE];
  char covers_through[RT_SMALL];
  char recent_starts_at[RT_SMALL];
  int recent_count;
} MemorySummaryState;

static int memory_ensure_dir(void) {
  return fs_mkdir_p(MEM_DIR);
}

int rt_memory_is_record_type(const char *type) {
  if (!type) return 0;
  return strcmp(type, "user") == 0 || strcmp(type, "asst") == 0 ||
         strcmp(type, "fact") == 0 || strcmp(type, "summ") == 0;
}

int rt_memory_is_control_type(const char *type) {
  return type && strcmp(type, "memory_compacted") == 0;
}

static int read_tail_or_empty(const char *path, char **out) {
  if (!out) return -1;
  *out = NULL;
  if (fs_read_tail(path, out, MEMORY_LOG_TAIL_BYTES) == 0 && *out) return 0;
  *out = (char *)fc_xstrdup("");
  return *out ? 0 : -1;
}

static int next_message_number(void) {
  char *text = NULL;
  const char *p;
  int max_id = 0;
  if (read_tail_or_empty(MEM_LOG, &text) != 0) return 1;
  p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 0) {
      char *line = (char *)fc_xmalloc(len + 1U);
      JsonRef root;
      char msg[RT_SMALL] = "";
      int n = 0;
      if (line) {
        memcpy(line, p, len);
        line[len] = '\0';
        if (json_ref_first_object(line, &root) == 0 &&
            json_ref_object_get_string(&root, "message_id", msg, sizeof(msg)) == 0 &&
            sscanf(msg, "msg_%d", &n) == 1 && n > max_id)
          max_id = n;
        fc_xfree(line);
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  fc_xfree(text);
  return max_id + 1;
}

static int append_message(MemoryMessages *m, const char *message_id,
                          const char *context_id, const char *role,
                          const char *text) {
  MemoryMessage *grown;
  if (!m || !message_id || !context_id || !role || !text) return -1;
  if (m->count >= m->cap) {
    size_t new_cap = m->cap ? m->cap * 2U : 32U;
    grown = (MemoryMessage *)fc_xrealloc(m->items, new_cap * sizeof(*m->items));
    if (!grown) return -1;
    m->items = grown;
    m->cap = new_cap;
  }
  snprintf(m->items[m->count].message_id, sizeof(m->items[m->count].message_id), "%s", message_id);
  snprintf(m->items[m->count].context_id, sizeof(m->items[m->count].context_id), "%s", context_id);
  snprintf(m->items[m->count].role, sizeof(m->items[m->count].role), "%s", role);
  snprintf(m->items[m->count].text, sizeof(m->items[m->count].text), "%s", text);
  m->items[m->count].chars = strlen(text);
  m->count++;
  return 0;
}

static void free_messages(MemoryMessages *m) {
  if (!m) return;
  fc_xfree(m->items);
  memset(m, 0, sizeof(*m));
}

static int load_messages(const char *context_id, MemoryMessages *out) {
  char *text = NULL;
  const char *p;
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (read_tail_or_empty(MEM_LOG, &text) != 0) return -1;
  p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len > 0) {
      char *line = (char *)fc_xmalloc(len + 1U);
      JsonRef root;
      char type[RT_SMALL] = "", role[RT_SMALL] = "";
      char msg[RT_SMALL] = "", ctx[RT_MED] = "", body[RT_LARGE] = "";
      if (line) {
        memcpy(line, p, len);
        line[len] = '\0';
        if (json_ref_first_object(line, &root) == 0 &&
            json_ref_object_get_string(&root, "type", type, sizeof(type)) == 0 &&
            (strcmp(type, "user") == 0 || strcmp(type, "asst") == 0) &&
            json_ref_object_get_string(&root, "text", body, sizeof(body)) == 0) {
          (void)json_ref_object_get_string(&root, "message_id", msg, sizeof(msg));
          (void)json_ref_object_get_string(&root, "context_id", ctx, sizeof(ctx));
          (void)json_ref_object_get_string(&root, "role", role, sizeof(role));
          if (!role[0]) snprintf(role, sizeof(role), "%s", strcmp(type, "asst") == 0 ? "assistant" : "user");
          if (!ctx[0]) snprintf(ctx, sizeof(ctx), "%s", context_id && *context_id ? context_id : "chat:cli");
          if (msg[0] && (!context_id || !*context_id || strcmp(ctx, context_id) == 0))
            (void)append_message(out, msg, ctx, role, body);
        }
        fc_xfree(line);
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  fc_xfree(text);
  return 0;
}

static int load_summary_state(const char *context_id, MemorySummaryState *st) {
  char *text = NULL;
  JsonRef root, contexts, item;
  if (!st) return -1;
  memset(st, 0, sizeof(*st));
  snprintf(st->context_id, sizeof(st->context_id), "%s",
           context_id && *context_id ? context_id : "chat:cli");
  st->recent_count = 0;
  if (fs_read_text(MEM_SUMMARY_STATE, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return 0;
  if (json_ref_first_object(text, &root) != 0 ||
      json_ref_object_get_array(&root, "contexts", &contexts) != 0) {
    fc_xfree(text);
    return 0;
  }
  for (size_t i = 0; i < json_ref_array_size(&contexts); ++i) {
    char ctx[RT_MED] = "";
    if (json_ref_array_get(&contexts, i, &item) != 0 ||
        item.type != JSON_REF_OBJECT ||
        json_ref_object_get_string(&item, "context_id", ctx, sizeof(ctx)) != 0 ||
        strcmp(ctx, st->context_id) != 0) continue;
    (void)json_ref_object_get_string(&item, "conversation_summary", st->conversation_summary, sizeof(st->conversation_summary));
    (void)json_ref_object_get_string(&item, "recent_summary", st->recent_summary, sizeof(st->recent_summary));
    (void)json_ref_object_get_string(&item, "summary_covers_through_message_id", st->covers_through, sizeof(st->covers_through));
    (void)json_ref_object_get_string(&item, "recent_starts_at_message_id", st->recent_starts_at, sizeof(st->recent_starts_at));
    {
      long long recent_count = 0;
      if (json_ref_object_get_long(&item, "recent_count", &recent_count) == 0)
        st->recent_count = (int)recent_count;
    }
    break;
  }
  fc_xfree(text);
  return 0;
}

static int find_message_index(const MemoryMessages *msgs, const char *id, size_t *out_idx);

static char *summary_state_item_dup(const MemorySummaryState *st) {
  char ectx[RT_MED * 2], econv[RT_LARGE * 2], erecent[RT_LARGE * 2];
  char ecovers[RT_MED], estarts[RT_MED];
  char *body;
  int n;
  if (!st) return NULL;
  if (json_escape(st->context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(st->conversation_summary, econv, sizeof(econv)) != 0 ||
      json_escape(st->recent_summary, erecent, sizeof(erecent)) != 0 ||
      json_escape(st->covers_through, ecovers, sizeof(ecovers)) != 0 ||
      json_escape(st->recent_starts_at, estarts, sizeof(estarts)) != 0)
    return NULL;
  n = snprintf(NULL, 0,
               "{\"context_id\":\"%s\",\"conversation_summary\":\"%s\","
               "\"recent_summary\":\"%s\",\"summary_covers_through_message_id\":\"%s\","
               "\"recent_starts_at_message_id\":\"%s\",\"recent_count\":%d}",
               ectx, econv, erecent, ecovers, estarts, st->recent_count);
  if (n < 0) return NULL;
  body = (char *)fc_xmalloc((size_t)n + 1U);
  if (!body) return NULL;
  snprintf(body, (size_t)n + 1U,
           "{\"context_id\":\"%s\",\"conversation_summary\":\"%s\","
           "\"recent_summary\":\"%s\",\"summary_covers_through_message_id\":\"%s\","
           "\"recent_starts_at_message_id\":\"%s\",\"recent_count\":%d}",
           ectx, econv, erecent, ecovers, estarts, st->recent_count);
  return body;
}

static int write_single_summary_state(const MemorySummaryState *st) {
  char *old = NULL, *body = NULL, *item_new = NULL;
  size_t cap = 0, pos = 0;
  int first = 1, replaced = 0;
  if (!st) return -1;
  if (fs_mkdir_p(MEM_SUMMARY_DIR) != 0) return -1;
  item_new = summary_state_item_dup(st);
  if (!item_new) return -1;
  if (rt_dyn_append_text(&body, &cap, &pos, "{\"contexts\":[") != 0) goto fail;
  if (fs_read_text(MEM_SUMMARY_STATE, &old, FS_READ_TEXT_DEFAULT_CAP) == 0 && old) {
    JsonRef root, contexts, item_ref;
    if (json_ref_first_object(old, &root) == 0 &&
        json_ref_object_get_array(&root, "contexts", &contexts) == 0) {
      for (size_t i = 0; i < json_ref_array_size(&contexts); ++i) {
        char ctx[RT_MED] = "";
        char *compact = NULL;
        if (json_ref_array_get(&contexts, i, &item_ref) != 0 ||
            item_ref.type != JSON_REF_OBJECT) continue;
        (void)json_ref_object_get_string(&item_ref, "context_id", ctx, sizeof(ctx));
        if (strcmp(ctx, st->context_id) == 0) {
          compact = item_new;
          replaced = 1;
        } else {
          compact = json_ref_value_compact_dup(&item_ref);
        }
        if (!compact ||
            (!first && rt_dyn_append_text(&body, &cap, &pos, ",") != 0) ||
            rt_dyn_append_text(&body, &cap, &pos, compact) != 0) {
          if (compact != item_new) fc_xfree(compact);
          goto fail;
        }
        first = 0;
        if (compact != item_new) fc_xfree(compact);
      }
    }
  }
  if (!replaced) {
    if ((!first && rt_dyn_append_text(&body, &cap, &pos, ",") != 0) ||
        rt_dyn_append_text(&body, &cap, &pos, item_new) != 0) goto fail;
  }
  if (rt_dyn_append_text(&body, &cap, &pos, "]}\n") != 0) goto fail;
  {
    int rc = fs_write_text_atomic(MEM_SUMMARY_STATE, body);
    fc_xfree(old);
    fc_xfree(body);
    fc_xfree(item_new);
    return rc;
  }
fail:
  fc_xfree(old);
  fc_xfree(body);
  fc_xfree(item_new);
  return -1;
}

int rt_memory_apply(RtContext *ctx, const char *type, const char *payload_json,
                    const char *ts, const char *run_id) {
  JsonRef payload;
  char text[RT_LARGE] = "";
  char text_esc[RT_LARGE * 2] = "";
  char through[RT_SMALL] = "";
  char through_esc[RT_SMALL] = "";
  char line[RT_XL];
  if (!type || !payload_json || !ts || !run_id) return -1;
  if (!rt_memory_is_record_type(type)) return -1;
  if (memory_ensure_dir() != 0) return -1;
  if (json_ref_first_object(payload_json, &payload) != 0) return -1;
  if (json_ref_object_get_string(&payload, "text", text, sizeof(text)) != 0) return -1;
  if (text[0] == '\0') return -1;
  if (json_escape(text, text_esc, sizeof(text_esc)) != 0) return -1;
  if (strcmp(type, "user") == 0 || strcmp(type, "asst") == 0) {
    int msg_num = next_message_number();
    char context[RT_MED] = "";
    char role[RT_SMALL];
    char econtext[RT_MED * 2];
    snprintf(context, sizeof(context), "%s", ctx && ctx->context_id[0] ? ctx->context_id : "chat:cli");
    (void)json_ref_object_get_string(&payload, "context_id", context, sizeof(context));
    snprintf(role, sizeof(role), "%s", strcmp(type, "asst") == 0 ? "assistant" : "user");
    if (json_escape(context, econtext, sizeof(econtext)) != 0) return -1;
    snprintf(line, sizeof(line),
             "{\"type\":\"%s\",\"message_id\":\"msg_%06d\",\"context_id\":\"%s\","
             "\"role\":\"%s\",\"text\":\"%s\",\"ts\":\"%s\",\"run_id\":\"%s\"}\n",
             type, msg_num, econtext, role, text_esc, ts, run_id);
    return fs_append_text(MEM_LOG, line);
  }
  if (strcmp(type, "summ") == 0) {
    (void)json_ref_object_get_string(&payload, "through", through, sizeof(through));
    if (through[0]) {
      if (json_escape(through, through_esc, sizeof(through_esc)) != 0) return -1;
      snprintf(line, sizeof(line),
               "{\"type\":\"%s\",\"text\":\"%s\",\"through\":\"%s\",\"ts\":\"%s\",\"run_id\":\"%s\"}\n",
               type, text_esc, through_esc, ts, run_id);
    } else {
      snprintf(line, sizeof(line),
               "{\"type\":\"%s\",\"text\":\"%s\",\"ts\":\"%s\",\"run_id\":\"%s\"}\n",
               type, text_esc, ts, run_id);
    }
  } else {
    snprintf(line, sizeof(line),
             "{\"type\":\"%s\",\"text\":\"%s\",\"ts\":\"%s\",\"run_id\":\"%s\"}\n",
             type, text_esc, ts, run_id);
  }
  return fs_append_text(MEM_LOG, line);
}

int rt_memory_apply_control(const char *type, const char *payload_json) {
  JsonRef payload;
  MemorySummaryState st;
  char cutoff[RT_SMALL] = "";
  if (!type || strcmp(type, "memory_compacted") != 0 || !payload_json) return -1;
  memset(&st, 0, sizeof(st));
  if (json_ref_first_object(payload_json, &payload) != 0) return -1;
  if (json_ref_object_get_string(&payload, "context_id", st.context_id, sizeof(st.context_id)) != 0)
    snprintf(st.context_id, sizeof(st.context_id), "chat:cli");
  if (json_ref_object_get_string(&payload, "conversation_summary", st.conversation_summary,
                                 sizeof(st.conversation_summary)) != 0)
    st.conversation_summary[0] = '\0';
  if (json_ref_object_get_string(&payload, "recent_summary", st.recent_summary,
                                 sizeof(st.recent_summary)) != 0)
    st.recent_summary[0] = '\0';
  if (json_ref_object_get_string(&payload, "covered_through_message_id", cutoff, sizeof(cutoff)) != 0 ||
      !cutoff[0]) return -1;
  snprintf(st.covers_through, sizeof(st.covers_through), "%s", cutoff);
  {
    MemoryMessages msgs;
    size_t idx;
    if (load_messages(st.context_id, &msgs) == 0) {
      if (find_message_index(&msgs, cutoff, &idx) == 0 && idx + 1U < msgs.count) {
        snprintf(st.recent_starts_at, sizeof(st.recent_starts_at), "%s",
                 msgs.items[idx + 1U].message_id);
        st.recent_count = (int)(msgs.count - idx - 1U);
      }
      free_messages(&msgs);
    }
  }
  return write_single_summary_state(&st);
}

/* The recalled projection becomes a memory_recalled event payload, and
 * event payloads are capped at RT_LARGE at the append boundary. A
 * count-only limit lets a chatty tail blow that budget — which fails
 * memory.before and bricks every subsequent turn in the context. The
 * projection is therefore byte-bounded: summaries and texts are
 * clipped, and the oldest messages are dropped until the whole
 * projection fits. The durable records stay full; only this per-run
 * view is bounded. */
#define MEMORY_PROJECTION_BYTE_BUDGET 3400
#define MEMORY_PROJECTION_TEXT_CLIP 350
#define MEMORY_PROJECTION_SUMMARY_CLIP 1000

/* Byte-truncate without splitting a UTF-8 sequence. */
static void clip_text(const char *src, char *dst, size_t dst_len, size_t max_bytes) {
  size_t n = strlen(src ? src : "");
  if (max_bytes >= dst_len) max_bytes = dst_len - 1;
  if (n > max_bytes) {
    n = max_bytes;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
  }
  memcpy(dst, src ? src : "", n);
  dst[n] = '\0';
}

static int render_projection_from(const MemorySummaryState *st,
                                  const MemoryMessages *msgs,
                                  size_t start,
                                  char *out, size_t out_len) {
  size_t pos = 0;
  char conv_clip[MEMORY_PROJECTION_SUMMARY_CLIP + 8];
  char rsum_clip[MEMORY_PROJECTION_SUMMARY_CLIP + 8];
  char econv[MEMORY_PROJECTION_SUMMARY_CLIP * 2 + 16];
  char erecent_summary[MEMORY_PROJECTION_SUMMARY_CLIP * 2 + 16];
  int n;
  clip_text(st->conversation_summary, conv_clip, sizeof(conv_clip),
            MEMORY_PROJECTION_SUMMARY_CLIP);
  clip_text(st->recent_summary, rsum_clip, sizeof(rsum_clip),
            MEMORY_PROJECTION_SUMMARY_CLIP);
  if (json_escape(conv_clip, econv, sizeof(econv)) != 0 ||
      json_escape(rsum_clip, erecent_summary, sizeof(erecent_summary)) != 0)
    return -1;
  if (conv_clip[0])
    n = snprintf(out, out_len, "{\"conversation_summary\":\"%s\",", econv);
  else
    n = snprintf(out, out_len, "{\"conversation_summary\":null,");
  if (n < 0 || (size_t)n >= out_len) return -1;
  pos = (size_t)n;
  if (rsum_clip[0])
    n = snprintf(out + pos, out_len - pos, "\"recent_summary\":\"%s\",\"recent\":[", erecent_summary);
  else
    n = snprintf(out + pos, out_len - pos, "\"recent_summary\":null,\"recent\":[");
  if (n < 0 || (size_t)n >= out_len - pos) return -1;
  pos += (size_t)n;
  for (size_t i = start; i < msgs->count; ++i) {
    char text_clip[MEMORY_PROJECTION_TEXT_CLIP + 8];
    char erole[RT_MED], etext[MEMORY_PROJECTION_TEXT_CLIP * 2 + 16], eid[RT_MED];
    clip_text(msgs->items[i].text, text_clip, sizeof(text_clip),
              MEMORY_PROJECTION_TEXT_CLIP);
    if (json_escape(msgs->items[i].role, erole, sizeof(erole)) != 0 ||
        json_escape(text_clip, etext, sizeof(etext)) != 0 ||
        json_escape(msgs->items[i].message_id, eid, sizeof(eid)) != 0)
      return -1;
    n = snprintf(out + pos, out_len - pos,
                 "%s{\"message_id\":\"%s\",\"role\":\"%s\",\"text\":\"%s\"}",
                 i > start ? "," : "", eid, erole, etext);
    if (n < 0 || (size_t)n >= out_len - pos) return -1;
    pos += (size_t)n;
  }
  n = snprintf(out + pos, out_len - pos, "],\"memory_tail\":[");
  if (n < 0 || (size_t)n >= out_len - pos) return -1;
  pos += (size_t)n;
  for (size_t i = start; i < msgs->count; ++i) {
    char text_clip[MEMORY_PROJECTION_TEXT_CLIP + 8];
    char erole[RT_MED], etext[MEMORY_PROJECTION_TEXT_CLIP * 2 + 16];
    clip_text(msgs->items[i].text, text_clip, sizeof(text_clip),
              MEMORY_PROJECTION_TEXT_CLIP);
    if (json_escape(msgs->items[i].role, erole, sizeof(erole)) != 0 ||
        json_escape(text_clip, etext, sizeof(etext)) != 0)
      return -1;
    n = snprintf(out + pos, out_len - pos,
                 "%s{\"role\":\"%s\",\"text\":\"%s\"}",
                 i > start ? "," : "", erole, etext);
    if (n < 0 || (size_t)n >= out_len - pos) return -1;
    pos += (size_t)n;
  }
  /* memory.bytes exposes the current size of memory.jsonl so agents can
   * decide (in their own policy) when to invoke rotate_file. The engine
   * knows nothing about a threshold or cadence; see the mechanism-versus-
   * policy rule in docs/concepts/principles.md. */
  {
    struct stat mst;
    long long bytes = 0;
    if (stat(MEM_LOG, &mst) == 0) bytes = (long long)mst.st_size;
    n = snprintf(out + pos, out_len - pos, "],\"memory_bytes\":%lld}", bytes);
  }
  return (n < 0 || (size_t)n >= out_len - pos) ? -1 : 0;
}

static int render_projection(const char *context_id, int recent_limit,
                             char *out, size_t out_len) {
  MemorySummaryState st;
  MemoryMessages msgs;
  size_t start = 0;
  size_t budget = out_len < MEMORY_PROJECTION_BYTE_BUDGET
                ? out_len : MEMORY_PROJECTION_BYTE_BUDGET;
  if (!out || out_len == 0) return -1;
  if (load_summary_state(context_id, &st) != 0 ||
      load_messages(context_id, &msgs) != 0) return -1;
  if (recent_limit < 1) recent_limit = 20;
  if (msgs.count > (size_t)recent_limit) start = msgs.count - (size_t)recent_limit;
  /* Newest messages win: drop from the front until the render fits. */
  for (;;) {
    if (render_projection_from(&st, &msgs, start, out, budget) == 0) {
      free_messages(&msgs);
      return 0;
    }
    if (start >= msgs.count) break; /* even the empty tail won't fit */
    start++;
  }
  free_messages(&msgs);
  return -1;
}

int rt_memory_state_json(char *out, size_t out_len) {
  if (!out || out_len == 0) return -1;
  if (render_projection(NULL, 20, out, out_len) != 0)
    snprintf(out, out_len, "{\"conversation_summary\":null,\"recent_summary\":null,\"recent\":[],\"memory_tail\":[]}");
  return 0;
}

int rt_memory_projection_json(const char *context_id, int recent_limit,
                              char *out, size_t out_len) {
  if (!out || out_len == 0) return -1;
  if (render_projection(context_id, recent_limit, out, out_len) != 0) {
    snprintf(out, out_len,
             "{\"conversation_summary\":null,\"recent_summary\":null,\"recent\":[],\"memory_tail\":[]}");
  }
  return 0;
}

static int find_message_index(const MemoryMessages *msgs, const char *id, size_t *out_idx) {
  if (!msgs || !id || !*id) return -1;
  for (size_t i = 0; i < msgs->count; ++i)
    if (strcmp(msgs->items[i].message_id, id) == 0) {
      if (out_idx) *out_idx = i;
      return 0;
    }
  return -1;
}

static int build_request_payload(const char *context_id,
                                 const MemorySummaryState *st,
                                 const MemoryMessages *msgs,
                                 size_t start, size_t end,
                                 int keep_recent,
                                 int summary_max_tokens,
                                 int recent_summary_max_tokens,
                                 char *out, size_t out_len) {
  size_t pos = 0;
  char ectx[RT_MED * 2], econv[RT_LARGE * 2], ersum[RT_LARGE * 2], ecut[RT_MED];
  const char *cutoff = msgs->items[end].message_id;
  int n;
  if (json_escape(context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(st->conversation_summary, econv, sizeof(econv)) != 0 ||
      json_escape(st->recent_summary, ersum, sizeof(ersum)) != 0 ||
      json_escape(cutoff, ecut, sizeof(ecut)) != 0) return -1;
  n = snprintf(out, out_len,
               "{\"context_id\":\"%s\",\"old_conversation_summary\":%s%s%s,"
               "\"old_recent_summary\":%s%s%s,\"messages_to_compact\":[",
               ectx,
               st->conversation_summary[0] ? "\"" : "null",
               st->conversation_summary[0] ? econv : "",
               st->conversation_summary[0] ? "\"" : "",
               st->recent_summary[0] ? "\"" : "null",
               st->recent_summary[0] ? ersum : "",
               st->recent_summary[0] ? "\"" : "");
  if (n < 0 || (size_t)n >= out_len) return -1;
  pos = (size_t)n;
  for (size_t i = start; i <= end; ++i) {
    char eid[RT_MED], erole[RT_MED], etext[RT_LARGE * 2];
    if (json_escape(msgs->items[i].message_id, eid, sizeof(eid)) != 0 ||
        json_escape(msgs->items[i].role, erole, sizeof(erole)) != 0 ||
        json_escape(msgs->items[i].text, etext, sizeof(etext)) != 0)
      return -1;
    n = snprintf(out + pos, out_len - pos,
                 "%s{\"message_id\":\"%s\",\"role\":\"%s\",\"text\":\"%s\"}",
                 i > start ? "," : "", eid, erole, etext);
    if (n < 0 || (size_t)n >= out_len - pos) return -1;
    pos += (size_t)n;
  }
  n = snprintf(out + pos, out_len - pos,
               "],\"compact_through_message_id\":\"%s\","
               "\"keep_recent_messages\":%d,"
               "\"summary_max_tokens\":%d,"
               "\"recent_summary_max_tokens\":%d}",
               ecut, keep_recent, summary_max_tokens, recent_summary_max_tokens);
  return (n < 0 || (size_t)n >= out_len - pos) ? -1 : 0;
}

int rt_memory_maybe_request_compaction(RtContext *ctx, const RtAgentMeta *meta) {
  MemorySummaryState st;
  MemoryMessages msgs;
  char payload[RT_XL];
  const char *context_id;
  int compact_after_messages;
  int compact_after_tokens;
  int keep_recent;
  int min_compact;
  size_t start = 0, end = 0;
  size_t unsummarized = 0, chars = 0;
  if (!ctx || !meta || !meta->memory_summary_enabled) return 0;
  context_id = ctx->context_id[0] ? ctx->context_id : "chat:cli";
  compact_after_messages = meta->memory_compact_after_messages;
  compact_after_tokens = meta->memory_compact_after_tokens;
  keep_recent = meta->memory_keep_recent_messages > 0 ? meta->memory_keep_recent_messages : 20;
  min_compact = meta->memory_min_compact_messages > 0 ? meta->memory_min_compact_messages : 16;
  if (load_summary_state(context_id, &st) != 0 || load_messages(context_id, &msgs) != 0) return -1;
  if (st.covers_through[0]) {
    size_t idx;
    if (find_message_index(&msgs, st.covers_through, &idx) == 0) start = idx + 1U;
  }
  if (msgs.count <= start) { free_messages(&msgs); return 0; }
  unsummarized = msgs.count - start;
  for (size_t i = start; i < msgs.count; ++i) chars += msgs.items[i].chars;
  if (!((compact_after_messages > 0 && unsummarized > (size_t)compact_after_messages) ||
        (compact_after_tokens > 0 && (chars + 3U) / 4U > (size_t)compact_after_tokens))) {
    free_messages(&msgs);
    return 0;
  }
  if (msgs.count <= (size_t)keep_recent) { free_messages(&msgs); return 0; }
  end = msgs.count - (size_t)keep_recent - 1U;
  if (end < start || end - start + 1U < (size_t)min_compact) {
    free_messages(&msgs);
    return 0;
  }
  if (build_request_payload(context_id, &st, &msgs, start, end, keep_recent,
                            meta->memory_summary_max_tokens,
                            meta->memory_recent_summary_max_tokens,
                            payload, sizeof(payload)) != 0) {
    free_messages(&msgs);
    return -1;
  }
  free_messages(&msgs);
  return rt_append_event(ctx, "memory_compaction_requested", "kernel", payload);
}

int rt_memory_compaction_input_json(const RtContext *ctx, char *out, size_t out_len) {
  if (!ctx || !out || out_len == 0 || !ctx->memory_compaction_json[0]) return -1;
  return snprintf(out, out_len, "%s\n", ctx->memory_compaction_json) >= (int)out_len ? -1 : 0;
}

int rt_memory_compaction_cutoff(const RtContext *ctx, char *out, size_t out_len) {
  JsonRef root;
  if (!ctx || !out || out_len == 0 || !ctx->memory_compaction_json[0]) return -1;
  if (json_ref_first_object(ctx->memory_compaction_json, &root) != 0 ||
      json_ref_object_get_string(&root, "compact_through_message_id", out, out_len) != 0)
    return -1;
  return 0;
}
