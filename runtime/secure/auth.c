#include "auth.h"

#include "auth_targets.h"
#include "secret_store.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define AUTH_TOKEN_MAX 8192
#define AUTH_KEY_BUF   256

typedef enum {
  AUTH_HEADER_NONE = 0,
  AUTH_HEADER_BEARER,
  AUTH_HEADER_X_API_KEY,
  AUTH_HEADER_X_GOOG_API_KEY,
} AuthHeaderStyle;

typedef struct {
  const char *endpoint;
  AuthHeaderStyle header_style;
} AuthRoute;

static const AuthRoute *find_route(const char *endpoint) {
  static const AuthRoute routes[] = {
      {"local_ollama",  AUTH_HEADER_NONE},
      {"anthropic_key", AUTH_HEADER_X_API_KEY},
      {"openai_key",    AUTH_HEADER_BEARER},
      {"openrouter_key", AUTH_HEADER_BEARER},
      {"gemini_key",    AUTH_HEADER_X_GOOG_API_KEY},
  };
  size_t i;
  if (!endpoint) return NULL;
  for (i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    if (strcmp(routes[i].endpoint, endpoint) == 0) return &routes[i];
  }
  return NULL;
}

int auth_resolve_static_secret(const char *endpoint, char *out, size_t out_sz) {
  char key[AUTH_KEY_BUF];
  if (!out || out_sz == 0) return -1;
  out[0] = '\0';
  if (auth_target_build_secret_key(endpoint, key, sizeof(key)) != 0) return -1;
  return secret_store_get(key, out, out_sz);
}

const char *auth_default_env_name(const char *endpoint) {
  if (!endpoint) return NULL;
  if (strcmp(endpoint, "openrouter_key") == 0) return "OPENROUTER_API_KEY";
  if (strcmp(endpoint, "gemini_key") == 0) return "GEMINI_API_KEY";
  return NULL;
}

int auth_resolve_secret_with_env(const char *endpoint, const char *env_name,
                                 char *out, size_t out_sz) {
  const char *env_value;
  if (!out || out_sz == 0) return -1;
  out[0] = '\0';
  if (env_name && *env_name) {
    env_value = getenv(env_name);
    if (env_value && *env_value) {
      if (snprintf(out, out_sz, "%s", env_value) >= (int)out_sz) {
        out[0] = '\0';
        return -1;
      }
      return 0;
    }
  }
  return auth_resolve_static_secret(endpoint, out, out_sz);
}

int auth_build_header_with_env(const char *endpoint, const char *env_name,
                               char *header_buf, size_t header_buf_sz) {
  const AuthRoute *route;
  char token[AUTH_TOKEN_MAX];
  int rc = -1;
  if (!header_buf || header_buf_sz == 0) return -1;
  header_buf[0] = '\0';
  route = find_route(endpoint);
  if (!route) return -1;
  if (route->header_style == AUTH_HEADER_NONE) return 0;
  if (auth_resolve_secret_with_env(endpoint, env_name, token, sizeof(token)) != 0) {
    secret_store_zero(token, sizeof(token));
    return -1;
  }
  switch (route->header_style) {
  case AUTH_HEADER_BEARER:
    rc = snprintf(header_buf, header_buf_sz, "Authorization: Bearer %s", token) < (int)header_buf_sz ? 0 : -1;
    break;
  case AUTH_HEADER_X_API_KEY:
    rc = snprintf(header_buf, header_buf_sz, "x-api-key: %s", token) < (int)header_buf_sz ? 0 : -1;
    break;
  case AUTH_HEADER_X_GOOG_API_KEY:
    rc = snprintf(header_buf, header_buf_sz, "x-goog-api-key: %s", token) < (int)header_buf_sz ? 0 : -1;
    break;
  default:
    rc = -1;
    break;
  }
  secret_store_zero(token, sizeof(token));
  if (rc != 0) header_buf[0] = '\0';
  return rc;
}

int auth_build_header(const char *endpoint, char *header_buf, size_t header_buf_sz) {
  return auth_build_header_with_env(endpoint, NULL, header_buf, header_buf_sz);
}
