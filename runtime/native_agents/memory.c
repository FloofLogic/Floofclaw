/* runtime/native_agents/memory.c
 *
 * Native memory agent. Branches on step_id:
 *   memory.before — derive workspace/memory/memory.json from the last
 *                   configured user/asst records, then emit memory_recalled.
 *   memory.after  — capture user_text as a "user" record, committed tool
 *                   activity as an "asst" transcript record, and the latest
 *                   composed message as an "asst" record. Runtime policy
 *                   decides later whether memory.compact is due.
 *
 * Input JSON is the standard processor task envelope. Phase and run state
 * come from trusted runtime ctx/step, not model-facing roots. */

#include "../runtime.h"
#include "../agents.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_LOG          "workspace/memory/memory.jsonl"
#define MEM_STATE        "workspace/memory/memory.json"

/* Duplicate detection needs only the recent JSONL window that can fit in the
 * fixed buffer. fs_read_tail drops a partial first record when it seeks. */
static int read_log(char *buf, size_t cap, size_t *out_len) {
  char *text = NULL;
  size_t n;
  if (!buf || cap < 2U) return -1;
  if (fs_read_tail(MEM_LOG, &text, cap - 1U) != 0 || !text) {
    if (out_len) *out_len = 0;
    if (cap) buf[0] = '\0';
    return 0; /* missing file is not an error — empty log */
  }
  n = strlen(text);
  if (n + 1 > cap) { free(text); return -1; }
  memcpy(buf, text, n + 1);
  free(text);
  if (out_len) *out_len = n;
  return 0;
}

static int run_memory_before(const RtContext *ctx, const RtAgentMeta *meta, char *out, size_t cap) {
  char state[RT_XL];
  int n;
  if (rt_memory_projection_json(ctx && ctx->context_id[0] ? ctx->context_id : NULL,
                                meta && meta->recent > 0 ? meta->recent : 20,
                                state, sizeof(state)) != 0)
    snprintf(state, sizeof(state),
             "{\"conversation_summary\":null,\"recent_summary\":null,\"recent\":[],\"memory_tail\":[]}");
  (void)fs_mkdir_p("workspace/memory");
  (void)fs_write_text_atomic(MEM_STATE, state);
  n = snprintf(out, cap, "{\"calls\":[{\"name\":\"memory.recalled\","
               "\"args\":%s}],\"notes\":[],\"error\":null}\n", state);
  return (n < 0 || (size_t)n >= cap) ? RT_ERR_OUTPUT_TOO_LARGE : 0;
}

static int emit_no_events(char *out, size_t cap) {
  int n = snprintf(out, cap,
                   "{\"calls\":[],\"notes\":[],\"error\":null}\n");
  return (n < 0 || (size_t)n >= cap) ? RT_ERR_OUTPUT_TOO_LARGE : 0;
}

/* memory.after emits only the existing user/asst record types. Tool activity
 * is rendered as an assistant transcript line so the existing projection and
 * compactor retain it without a second transcript store or another role
 * schema. Compaction policy is evaluated after these records commit. */
static int memory_record_exists(const char *log_buf, const char *run_id,
                                const char *type, const char *text) {
  const char *p = log_buf ? log_buf : "";
  static const char run_id_key[] = { 'r', 'u', 'n', '_', 'i', 'd', '\0' };
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    char tmp[RT_LARGE];
    JsonRef root;
    char rtype[RT_SMALL] = "", rtext[RT_LARGE] = "", rrun[RT_SMALL] = "";
    if (len > 0 && len + 1 <= sizeof(tmp)) {
      memcpy(tmp, p, len);
      tmp[len] = '\0';
      if (json_ref_first_object(tmp, &root) == 0) {
        (void)json_ref_object_get_string(&root, "type", rtype, sizeof(rtype));
        (void)json_ref_object_get_string(&root, "text", rtext, sizeof(rtext));
        (void)json_ref_object_get_string(&root, run_id_key, rrun, sizeof(rrun));
        if (strcmp(rtype, type) == 0 && strcmp(rtext, text) == 0 &&
            strcmp(rrun, run_id) == 0)
          return 1;
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  return 0;
}

static int run_memory_after(const RtContext *ctx, char *out, size_t cap) {
  static char log_buf[RT_XL];
  char user_text[RT_LARGE]      = "";
  char tool_text[RT_LARGE]      = "";
  char latest_message[RT_LARGE] = "";
  char run_id[RT_SMALL]         = "";
  char esc_user[RT_LARGE * 2];
  char esc_tool[RT_LARGE * 2];
  char esc_asst[RT_LARGE * 2];
  size_t used = 0;
  int n;
  int first = 1;

  if (ctx) {
    snprintf(user_text, sizeof(user_text), "%s", ctx->user_text);
    if (ctx->latest_action[0] &&
        strcmp(ctx->latest_action, "message") != 0 &&
        strcmp(ctx->latest_action, "working_memory_append") != 0) {
      snprintf(tool_text, sizeof(tool_text), "Tool %s returned: %s",
               ctx->latest_action,
               ctx->latest_result_text[0] ? ctx->latest_result_text
                                          : "(no result text)");
    }
    snprintf(latest_message, sizeof(latest_message), "%s", ctx->latest_message);
    snprintf(run_id, sizeof(run_id), "%s", ctx->run_id);
  }

  /* affair_review runs are the system talking to itself — they don't
   * belong in chat memory. The affair store itself is the durable
   * record (notes, next_review_at_ms). Emit an empty calls array so
   * memory.after is a no-op for these runs. */
  if (ctx && ctx->inbound_affair_id[0]) {
    int n0 = snprintf(out, cap, "{\"calls\":[],\"notes\":[],\"error\":null}\n");
    if (n0 < 0 || (size_t)n0 >= cap) return RT_ERR_OUTPUT_TOO_LARGE;
    return 0;
  }

  /* Trim trailing whitespace (the bash version stripped). */
  for (size_t i = strlen(user_text); i > 0; --i) {
    char c = user_text[i - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') user_text[i - 1] = '\0';
    else break;
  }
  for (size_t i = strlen(latest_message); i > 0; --i) {
    char c = latest_message[i - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') latest_message[i - 1] = '\0';
    else break;
  }
  for (size_t i = strlen(tool_text); i > 0; --i) {
    char c = tool_text[i - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') tool_text[i - 1] = '\0';
    else break;
  }

  if (read_log(log_buf, sizeof(log_buf), NULL) == 0) {
  } else {
    log_buf[0] = '\0';
  }
  if (!user_text[0] || memory_record_exists(log_buf, run_id, "user", user_text))
    user_text[0] = '\0';
  if (!tool_text[0] || memory_record_exists(log_buf, run_id, "asst", tool_text))
    tool_text[0] = '\0';
  if (!latest_message[0] || memory_record_exists(log_buf, run_id, "asst", latest_message))
    latest_message[0] = '\0';

  /* Build calls array. */
  n = snprintf(out + used, cap - used, "{\"calls\":[");
  if (n < 0 || (size_t)n >= cap - used) return RT_ERR_OUTPUT_TOO_LARGE;
  used += (size_t)n;

  if (user_text[0]) {
    if (json_escape(user_text, esc_user, sizeof(esc_user)) != 0) return 1;
    n = snprintf(out + used, cap - used,
                 "%s{\"name\":\"memory.user\","
                 "\"args\":{\"text\":\"%s\"}}",
                 first ? "" : ",", esc_user);
    if (n < 0 || (size_t)n >= cap - used) return RT_ERR_OUTPUT_TOO_LARGE;
    used += (size_t)n;
    first = 0;
  }

  if (tool_text[0]) {
    if (json_escape(tool_text, esc_tool, sizeof(esc_tool)) != 0) return 1;
    n = snprintf(out + used, cap - used,
                 "%s{\"name\":\"memory.asst\","
                 "\"args\":{\"text\":\"%s\"}}",
                 first ? "" : ",", esc_tool);
    if (n < 0 || (size_t)n >= cap - used) return RT_ERR_OUTPUT_TOO_LARGE;
    used += (size_t)n;
    first = 0;
  }

  if (latest_message[0]) {
    if (json_escape(latest_message, esc_asst, sizeof(esc_asst)) != 0) return 1;
    n = snprintf(out + used, cap - used,
                 "%s{\"name\":\"memory.asst\","
                 "\"args\":{\"text\":\"%s\"}}",
                 first ? "" : ",", esc_asst);
    if (n < 0 || (size_t)n >= cap - used) return RT_ERR_OUTPUT_TOO_LARGE;
    used += (size_t)n;
    first = 0;
  }

  n = snprintf(out + used, cap - used, "],\"notes\":[],\"error\":null}\n");
  if (n < 0 || (size_t)n >= cap - used) return RT_ERR_OUTPUT_TOO_LARGE;
  return 0;
}

int rt_agent_memory_run(const RtContext *ctx, const RtStep *step,
                        const RtAgentMeta *meta,
                        const RtActionRegistry *actions,
                        const char *input_json,
                        char *stdout_buf, size_t stdout_cap,
                        char *stderr_buf, size_t stderr_cap) {
  (void)stderr_buf; (void)stderr_cap;
  (void)actions;
  (void)input_json;

  if (step && strcmp(step->id, "memory.before") == 0) {
    if (ctx && ctx->memory_json[0])
      return emit_no_events(stdout_buf, stdout_cap);
    return run_memory_before(ctx, meta, stdout_buf, stdout_cap);
  }
  if (step && strcmp(step->id, "memory.after") == 0) {
    return run_memory_after(ctx, stdout_buf, stdout_cap);
  }
  /* Unknown phase: emit no calls. */
  int n = snprintf(stdout_buf, stdout_cap,
                   "{\"calls\":[],\"notes\":[],\"error\":null}\n");
  return (n < 0 || (size_t)n >= stdout_cap) ? RT_ERR_OUTPUT_TOO_LARGE : 0;
}
