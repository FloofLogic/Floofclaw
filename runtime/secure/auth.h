#ifndef FCLAW_SECURE_AUTH_H
#define FCLAW_SECURE_AUTH_H

#include <stddef.h>

/* Resolve the static secret for `endpoint` (e.g. "anthropic_key") into out.
 * Returns 0 and writes the secret on success, -1 on miss/failure. */
int auth_resolve_static_secret(const char *endpoint, char *out, size_t out_sz);

/* Resolve auth for `endpoint` with an optional env-var fallback checked first.
 * Returns 0 and writes the secret on success, -1 on miss/failure. */
int auth_resolve_secret_with_env(const char *endpoint, const char *env_name,
                                 char *out, size_t out_sz);

/* Build the auth header for `endpoint` into `header_buf`. Header line has no
 * trailing newline. Returns 0 on success, -1 if no secret could be resolved.
 * For local endpoints with no auth, returns 0 and writes an empty header. */
int auth_build_header(const char *endpoint, char *header_buf, size_t header_buf_sz);

/* Same as auth_build_header but checks `env_name` first when provided. */
int auth_build_header_with_env(const char *endpoint, const char *env_name,
                               char *header_buf, size_t header_buf_sz);

/* Return the default env-var fallback for an endpoint when one exists. */
const char *auth_default_env_name(const char *endpoint);

#endif
