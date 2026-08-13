#ifndef FCLAW_SECURE_SECRET_STORE_H
#define FCLAW_SECURE_SECRET_STORE_H

#include <stddef.h>

int secret_store_get(const char *key, char *out, size_t out_sz);
int secret_store_set(const char *key, const char *value);
int secret_store_delete(const char *key);

/* Invoke fn for every stored key beginning with prefix, passing only the
 * part AFTER the prefix. Never yields values. Used to enumerate one
 * action's namespace without letting the caller name another's. */
int secret_store_list_prefix(const char *prefix,
                             int (*fn)(const char *suffix, void *user),
                             void *user);

void secret_store_zero(void *ptr, size_t len);

#endif
