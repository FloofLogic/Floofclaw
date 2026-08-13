#include "llm.h"

#include "../secure/auth.h"
#include "../secure/secret_store.h"
#include "../support/fsutil.h"
#include "../support/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *profile_path(void) {
  const char *p = getenv("FCLAW_MODEL_PROFILES");
  return (p && *p) ? p : "config/model_profiles.json";
}

static const char *registry_path(void) {
  const char *p = getenv("FCLAW_LLM_REGISTRY");
  return (p && *p) ? p : "config/llm_registry.json";
}

static int find_in_array(const char *path, const char *array_key, const char *match_id,
                         JsonRef *root_out, JsonRef *match_out, char **text_out) {
  JsonRef arr;
  size_t i, n;
  if (fs_read_text(path, text_out, FS_READ_TEXT_DEFAULT_CAP) != 0) return -1;
  if (json_ref_first_object(*text_out, root_out) != 0) { free(*text_out); *text_out = NULL; return -1; }
  if (json_ref_object_get_array(root_out, array_key, &arr) != 0) { free(*text_out); *text_out = NULL; return -1; }
  n = json_ref_array_size(&arr);
  for (i = 0; i < n; ++i) {
    JsonRef item;
    char id[LLM_ID_MAX];
    if (json_ref_array_get(&arr, i, &item) != 0) continue;
    if (json_ref_object_get_string(&item, "id", id, sizeof(id)) != 0) continue;
    if (strcmp(id, match_id) == 0) { *match_out = item; return 0; }
  }
  free(*text_out);
  *text_out = NULL;
  return -1;
}

int llm_load_profile(const char *profile_id, LlmProfile *out) {
  JsonRef root, match;
  char *text = NULL;
  if (!profile_id || !out) return -1;
  memset(out, 0, sizeof(*out));
  if (find_in_array(profile_path(), "profiles", profile_id, &root, &match, &text) != 0) return -1;
  if (json_ref_object_get_string(&match, "id", out->id, sizeof(out->id)) != 0 ||
      json_ref_object_get_string(&match, "provider", out->provider, sizeof(out->provider)) != 0 ||
      json_ref_object_get_string(&match, "model", out->model, sizeof(out->model)) != 0) {
    free(text);
    return -1;
  }
  /* Optional: absent leaves the zeroed field, which keeps each provider's
   * legacy request shape. */
  (void)json_ref_object_get_string(&match, "effort", out->effort, sizeof(out->effort));
  free(text);
  return 0;
}

int llm_load_provider(const char *provider_id, LlmProvider *out) {
  JsonRef root, match;
  JsonRef limits;
  long long limit = 0;
  char *text = NULL;
  if (!provider_id || !out) return -1;
  memset(out, 0, sizeof(*out));
  out->rpm_limit = -1;
  out->daily_limit = -1;
  if (find_in_array(registry_path(), "providers", provider_id, &root, &match, &text) != 0) return -1;
  if (json_ref_object_get_string(&match, "id", out->id, sizeof(out->id)) != 0 ||
      json_ref_object_get_string(&match, "kind", out->kind, sizeof(out->kind)) != 0) {
    free(text);
    return -1;
  }
  (void)json_ref_object_get_string(&match, "url", out->url, sizeof(out->url));
  (void)json_ref_object_get_string(&match, "auth_endpoint", out->auth_endpoint, sizeof(out->auth_endpoint));
  (void)json_ref_object_get_string(&match, "auth_env", out->auth_env, sizeof(out->auth_env));
  if (json_ref_object_get_object(&match, "limits", &limits) == 0) {
    if (json_ref_object_get_long(&limits, "rpm", &limit) == 0) out->rpm_limit = (int)limit;
    if (json_ref_object_get_long(&limits, "daily", &limit) == 0) out->daily_limit = (int)limit;
  }
  free(text);
  return 0;
}

static int supported_provider_kind(const char *kind) {
  return kind && (strcmp(kind, "mock") == 0 ||
                  strcmp(kind, "http") == 0 ||
                  strcmp(kind, "openai_compat") == 0 ||
                  strcmp(kind, "openai_responses") == 0 ||
                  strcmp(kind, "gemini_key") == 0);
}

int llm_validate_profile_startup(const char *profile_id,
                                 char *err, size_t err_len) {
  LlmProfile profile;
  LlmProvider provider;
  char secret[8192];
  if (llm_load_profile(profile_id, &profile) != 0) {
    if (err)
      snprintf(err, err_len,
               "profile '%s' not found; fix: add it to config/model_profiles.json or change model.ref",
               profile_id ? profile_id : "");
    return -1;
  }
  if (llm_load_provider(profile.provider, &provider) != 0) {
    if (err)
      snprintf(err, err_len,
               "profile '%s' references unknown provider '%s'; fix: add it to config/llm_registry.json or change the profile's provider",
               profile.id, profile.provider);
    return -1;
  }
  if (!supported_provider_kind(provider.kind)) {
    if (err)
      snprintf(err, err_len,
               "profile '%s' resolves to unimplemented provider kind '%s'; fix: use a provider whose kind is mock, http, openai_compat, openai_responses, or gemini_key",
               profile.id, provider.kind);
    return -1;
  }
  if (!provider.auth_endpoint[0]) return 0;
  memset(secret, 0, sizeof(secret));
  if (auth_resolve_secret_with_env(provider.auth_endpoint, provider.auth_env,
                                   secret, sizeof(secret)) != 0) {
    secret_store_zero(secret, sizeof(secret));
    if (err) {
      if (provider.auth_env[0])
        snprintf(err, err_len,
                 "missing key '%s'; fix: run ./bin/fclaw auth set-stdin %s or set %s",
                 provider.auth_endpoint, provider.auth_endpoint,
                 provider.auth_env);
      else
        snprintf(err, err_len,
                 "missing key '%s'; fix: run ./bin/fclaw auth set-stdin %s",
                 provider.auth_endpoint, provider.auth_endpoint);
    }
    return -1;
  }
  secret_store_zero(secret, sizeof(secret));
  return 0;
}
