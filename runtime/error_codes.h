#ifndef FCLAW_ERROR_CODES_H
#define FCLAW_ERROR_CODES_H

/* Common runtime error classes.
 *
 * These are intentionally small, two-digit buckets for UI/tests/control
 * flow. The domain-specific error string stays intact in runstate/event
 * payloads as `code` (for example: llm_provider_connection_failed).
 */
typedef enum {
  FC_ERROR_NONE = 0,

  FC_ERROR_BAD_REQUEST  = 20,
  FC_ERROR_INVALID_ARGS = 21,
  FC_ERROR_MALFORMED    = 22,

  FC_ERROR_NOT_FOUND   = 40,
  FC_ERROR_NOT_ALLOWED = 41,

  FC_ERROR_RUNTIME      = 50,
  FC_ERROR_AGENT_FAILED = 51,
  FC_ERROR_TIMEOUT      = 52,
  FC_ERROR_OUTPUT_LIMIT = 53,

  FC_ERROR_EXTERNAL_CONNECTION = 60,
  FC_ERROR_EXTERNAL_REQUEST    = 61,
  FC_ERROR_EXTERNAL_HTTP       = 62,
  FC_ERROR_EXTERNAL_AUTH       = 63,
  FC_ERROR_EXTERNAL_RESPONSE   = 64
} FcErrorCode;

static inline const char *fc_error_default_code(FcErrorCode code) {
  switch (code) {
    case FC_ERROR_NONE:                return "ok";
    case FC_ERROR_BAD_REQUEST:         return "bad_request";
    case FC_ERROR_INVALID_ARGS:        return "invalid_args";
    case FC_ERROR_MALFORMED:           return "malformed";
    case FC_ERROR_NOT_FOUND:           return "not_found";
    case FC_ERROR_NOT_ALLOWED:         return "not_allowed";
    case FC_ERROR_RUNTIME:             return "runtime_error";
    case FC_ERROR_AGENT_FAILED:        return "agent_failed";
    case FC_ERROR_TIMEOUT:             return "timeout";
    case FC_ERROR_OUTPUT_LIMIT:        return "output_limit";
    case FC_ERROR_EXTERNAL_CONNECTION: return "external_connection_failed";
    case FC_ERROR_EXTERNAL_REQUEST:    return "external_request_failed";
    case FC_ERROR_EXTERNAL_HTTP:       return "external_http_error";
    case FC_ERROR_EXTERNAL_AUTH:       return "external_auth_failed";
    case FC_ERROR_EXTERNAL_RESPONSE:   return "external_response_error";
  }
  return "runtime_error";
}

#endif
