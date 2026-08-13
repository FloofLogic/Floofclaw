#include "agent_errors.h"

#include <stdio.h>
#include <string.h>

static void first_line(const char *src, char *out, size_t out_len) {
  size_t pos = 0;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!src) return;
  while (*src && pos + 1 < out_len) {
    char c = *src++;
    if (c == '\r' || c == '\n') {
      while (*src == '\r' || *src == '\n') src++;
      if (!*src) break;
      c = ' ';
    }
    out[pos++] = c;
  }
  out[pos] = '\0';
}

static const char *after(const char *text, const char *needle) {
  const char *p = text && needle ? strstr(text, needle) : NULL;
  return p ? p + strlen(needle) : NULL;
}

FcErrorCode rt_classify_agent_failure(const RtJob *job, const char *stderr_text,
                                      char *domain, size_t domain_len,
                                      char *message, size_t message_len) {
  char detail[RT_MED];
  const char *p;
  if (domain && domain_len) domain[0] = '\0';
  if (message && message_len) message[0] = '\0';
  if (job && job->killed_for_timeout) {
    snprintf(domain, domain_len, "agent_timeout");
    snprintf(message, message_len, "Agent job timed out.");
    return FC_ERROR_TIMEOUT;
  }
  if (job && job->killed_for_output_limit) {
    snprintf(domain, domain_len, "agent_output_limit");
    snprintf(message, message_len, "Agent job exceeded the output limit.");
    return FC_ERROR_OUTPUT_LIMIT;
  }
  p = after(stderr_text, "llm/http: curl_easy_perform failed: ");
  if (p) {
    first_line(p, detail, sizeof(detail));
    if (strstr(detail, "Couldn't connect") || strstr(detail, "Failed to connect") ||
        strstr(detail, "Connection refused")) {
      snprintf(domain, domain_len, "llm_provider_connection_failed");
      snprintf(message, message_len, "LLM provider connection failed: %s", detail);
      return FC_ERROR_EXTERNAL_CONNECTION;
    }
    snprintf(domain, domain_len, "llm_provider_request_failed");
    snprintf(message, message_len, "LLM provider request failed: %s", detail);
    return FC_ERROR_EXTERNAL_REQUEST;
  }
  p = after(stderr_text, "llm/http: HTTP ");
  if (p) {
    first_line(p, detail, sizeof(detail));
    snprintf(domain, domain_len, "llm_provider_http_error");
    snprintf(message, message_len, "LLM provider returned HTTP %s", detail);
    return FC_ERROR_EXTERNAL_HTTP;
  }
  if (stderr_text && strstr(stderr_text, "llm/http: failed to resolve auth")) {
    snprintf(domain, domain_len, "llm_provider_auth_failed");
    snprintf(message, message_len, "LLM provider authentication failed.");
    return FC_ERROR_EXTERNAL_AUTH;
  }
  if (stderr_text && strstr(stderr_text, "could not extract model text")) {
    snprintf(domain, domain_len, "llm_provider_response_invalid");
    snprintf(message, message_len, "LLM provider response could not be parsed.");
    return FC_ERROR_EXTERNAL_RESPONSE;
  }
  if (stderr_text && strstr(stderr_text, "llm/normalize: model_output_too_large")) {
    snprintf(domain, domain_len, "model_output_too_large");
    snprintf(message, message_len, "LLM model output exceeded the runtime size limit.");
    return FC_ERROR_OUTPUT_LIMIT;
  }
  if (stderr_text && strstr(stderr_text, "llm: json rejected by output contract")) {
    snprintf(domain, domain_len, "agent_failed:llm_json_rejected");
    snprintf(message, message_len, "Agent job failed: LLM JSON was rejected by the output contract.");
    return FC_ERROR_AGENT_FAILED;
  }
  if (stderr_text && *stderr_text) {
    first_line(stderr_text, detail, sizeof(detail));
    snprintf(domain, domain_len, "agent_failed");
    snprintf(message, message_len, "Agent job failed: %s", detail);
    return FC_ERROR_AGENT_FAILED;
  }
  snprintf(domain, domain_len, "agent_failed");
  snprintf(message, message_len, "Agent job failed.");
  return FC_ERROR_AGENT_FAILED;
}
