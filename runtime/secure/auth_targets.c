#include "auth_targets.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int auth_target_is_known_endpoint(const char *endpoint) {
  if (!endpoint) return 0;
  return strcmp(endpoint, "anthropic_key") == 0 ||
         strcmp(endpoint, "openai_key") == 0 ||
         strcmp(endpoint, "openrouter_key") == 0 ||
         strcmp(endpoint, "gemini_key") == 0 ||
         strcmp(endpoint, "local_ollama") == 0;
}

int auth_target_is_secret_name(const char *name) {
  size_t i;
  if (!name || name[0] == '\0') return 0;
  for (i = 0; name[i]; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (!(isalnum((int)c) || c == '_' || c == '-' || c == '.' || c == ':')) return 0;
  }
  return 1;
}

int auth_target_build_secret_key(const char *name, char *out, size_t out_sz) {
  if (!auth_target_is_secret_name(name) || !out || out_sz == 0) return -1;
  return snprintf(out, out_sz, "endpoint:%s", name) < (int)out_sz ? 0 : -1;
}
