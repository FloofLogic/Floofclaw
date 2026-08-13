#include "llm.h"

#include "../support/json.h"
#include "../support/heap_guard.h"

#include <stdio.h>
#include <string.h>

static int try_model_object(const char *text, char *out, size_t out_len) {
  JsonRef root;
  char *compact = NULL;
  int rc = -1;
  if (json_ref_top_object(text, &root) != 0)
    return -1;
  compact = json_ref_value_compact_dup(&root);
  if (!compact) return LLM_ERR_MODEL_OUTPUT_TOO_LARGE;
  rc = snprintf(out, out_len, "%s", compact) >= (int)out_len
     ? LLM_ERR_MODEL_OUTPUT_TOO_LARGE : 0;
  fc_xfree(compact);
  return rc;
}

static int try_first_object_then_tool_noop(const char *text, char *out, size_t out_len) {
  JsonRef first, tail;
  char *compact = NULL;
  char tail_raw[128], tail_compact[128];
  const char *p;
  int rc = -1;
  if (!text || json_ref_first_object(text, &first) != 0) return -1;
  for (p = text; p < first.start; ++p)
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return -1;
  p = first.end;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p == '\0') return -1;
  if (json_ref_from_text(p, &tail) != 0 ||
      json_ref_value_copy(&tail, tail_raw, sizeof(tail_raw)) != 0 ||
      json_compact(tail_raw, tail_compact, sizeof(tail_compact)) != 0 ||
      strcmp(tail_compact, "{\"tool\":null}") != 0)
    return -1;
  compact = json_ref_value_compact_dup(&first);
  if (!compact) return LLM_ERR_MODEL_OUTPUT_TOO_LARGE;
  rc = snprintf(out, out_len, "%s", compact) >= (int)out_len
     ? LLM_ERR_MODEL_OUTPUT_TOO_LARGE : 0;
  fc_xfree(compact);
  return rc;
}

int llm_normalize_response_text(const char *model_text,
                                char *out, size_t out_len,
                                LlmUsage *usage_out) {
  char candidate[LLM_RESP_MAX];
  if (!model_text) model_text = "";
  if (usage_out) {
    usage_out->json_repair_checked = 0;
    usage_out->json_repair_status = FJR_OK_UNCHANGED;
    usage_out->json_repair_raw_len = strlen(model_text);
    usage_out->json_repair_repaired_len = strlen(model_text);
    memset(&usage_out->json_repair_report, 0, sizeof(usage_out->json_repair_report));
  }
  {
    int rc = try_model_object(model_text, out, out_len);
    if (rc == 0) {
      if (usage_out) usage_out->json_repair_checked = 1;
      return 0;
    }
    if (rc == LLM_ERR_MODEL_OUTPUT_TOO_LARGE) return rc;
  }
  {
    int rc = try_first_object_then_tool_noop(model_text, out, out_len);
    if (rc == 0) {
      if (usage_out) {
        usage_out->json_repair_checked = 1;
        usage_out->json_repair_status = FJR_OK_REPAIRED;
        usage_out->json_repair_repaired_len = strlen(out);
        usage_out->json_repair_report.trimmed_trailing_junk = 1;
        usage_out->json_repair_report.repairs = 1;
      }
      return 0;
    }
    if (rc == LLM_ERR_MODEL_OUTPUT_TOO_LARGE) return rc;
  }
  if (strchr(model_text, '{') || strchr(model_text, '[') ||
      strstr(model_text, "```") || strstr(model_text, "\"calls\"")) {
    FjrReport report;
    size_t repaired_len = 0;
    int repair_status = fjson_repair(model_text, strlen(model_text),
                                     candidate, sizeof(candidate),
                                     &repaired_len, &report);
    if (usage_out) {
      usage_out->json_repair_checked = 1;
      usage_out->json_repair_status = (FjrStatus)repair_status;
      usage_out->json_repair_report = report;
      usage_out->json_repair_repaired_len = repaired_len;
    }
    if (repair_status == FJR_OK_REPAIRED) {
      int rc = try_model_object(candidate, out, out_len);
      if (rc == 0) return 0;
      if (rc == LLM_ERR_MODEL_OUTPUT_TOO_LARGE) return rc;
    }
  }
  return -1;
}
