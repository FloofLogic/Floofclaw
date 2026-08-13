#ifndef FCLAW_SECURE_AUTH_TARGETS_H
#define FCLAW_SECURE_AUTH_TARGETS_H

#include <stddef.h>

int auth_target_is_known_endpoint(const char *endpoint);
int auth_target_is_secret_name(const char *name);
int auth_target_build_secret_key(const char *name, char *out, size_t out_sz);

#endif
