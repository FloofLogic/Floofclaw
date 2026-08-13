/* File-backed secret store.
 *
 * Stores small secrets at <FCLAW_HOME>/secrets/<sanitized-key>, file mode
 * 0600, directory mode 0700. FCLAW_HOME defaults to the working directory,
 * so secrets live in a visible, gitignored ./secrets/ alongside workspace/
 * and .fclaw/ — one floofclaw is one self-contained directory. Set
 * FCLAW_HOME to relocate the store. The sanitizer rejects empty keys and
 * any character outside [A-Za-z0-9_.:-].
 *
 * The store never writes secrets to the run dir, the event log, or trace
 * artifacts. Callers must zeroize their own buffers; secret_store_zero is
 * provided for that.
 */

#include "secret_store.h"

#include "../support/fsutil.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SS_PATH_MAX  PATH_MAX
#define SS_KEY_MAX   256

void secret_store_zero(void *ptr, size_t len) {
  volatile unsigned char *p = (volatile unsigned char *)ptr;
  while (p && len-- > 0U) *p++ = 0U;
}

static int valid_key_char(unsigned char c) {
  return isalnum((int)c) || c == '_' || c == '-' || c == '.' || c == ':';
}

static int sanitize_key(const char *key, char *out, size_t out_len) {
  size_t i;
  if (!key || !out || out_len == 0) return -1;
  if (key[0] == '\0') return -1;
  for (i = 0; key[i]; ++i) {
    if (!valid_key_char((unsigned char)key[i])) return -1;
    if (i + 1 >= out_len) return -1;
    out[i] = key[i];
  }
  out[i] = '\0';
  return 0;
}

static int resolve_home(char *out, size_t out_len) {
  const char *home = getenv("FCLAW_HOME");
  if (home && *home) return snprintf(out, out_len, "%s", home) < (int)out_len ? 0 : -1;
  /* Default to the working directory: secrets live in ./secrets/, visible
   * and gitignored, part of the self-contained deployment directory. */
  return snprintf(out, out_len, "%s", ".") < (int)out_len ? 0 : -1;
}

static int secret_path(const char *key, char *out, size_t out_len, char *secrets_dir, size_t dir_len) {
  char home[SS_PATH_MAX];
  char sanitized[SS_KEY_MAX];
  if (resolve_home(home, sizeof(home)) != 0) return -1;
  if (sanitize_key(key, sanitized, sizeof(sanitized)) != 0) return -1;
  if (snprintf(secrets_dir, dir_len, "%s/secrets", home) >= (int)dir_len) return -1;
  if (snprintf(out, out_len, "%s/%s", secrets_dir, sanitized) >= (int)out_len) return -1;
  return 0;
}

static int ensure_secret_dir(const char *home, const char *secrets_dir) {
  if (fs_mkdir_p(home) != 0) return -1;
  if (chmod(home, 0700) != 0 && errno != ENOENT) {
    /* Best-effort tighten; ignore failure when cwd already locked down. */
  }
  if (fs_mkdir_p(secrets_dir) != 0) return -1;
  if (chmod(secrets_dir, 0700) != 0) {
    /* same: not fatal */
  }
  return 0;
}

int secret_store_set(const char *key, const char *value) {
  char home[SS_PATH_MAX];
  char secrets_dir[SS_PATH_MAX];
  char path[SS_PATH_MAX];
  char temp_path[SS_PATH_MAX];
  char sanitized[SS_KEY_MAX];
  int fd = -1;
  int dir_fd = -1;
  size_t value_len;
  if (!key || !value) return -1;
  if (sanitize_key(key, sanitized, sizeof(sanitized)) != 0) return -1;
  if (resolve_home(home, sizeof(home)) != 0) return -1;
  if (snprintf(secrets_dir, sizeof(secrets_dir), "%s/secrets", home) >= (int)sizeof(secrets_dir)) return -1;
  if (snprintf(path, sizeof(path), "%s/%s", secrets_dir, sanitized) >= (int)sizeof(path)) return -1;
  if (ensure_secret_dir(home, secrets_dir) != 0) return -1;
  if (snprintf(temp_path, sizeof(temp_path), "%s/.%s.tmp.XXXXXX",
               secrets_dir, sanitized) >= (int)sizeof(temp_path))
    return -1;
  fd = mkstemp(temp_path);
  if (fd < 0) return -1;
  value_len = strlen(value);
  {
    size_t written = 0;
    while (written < value_len) {
      ssize_t n = write(fd, value + written, value_len - written);
      if (n > 0) {
        written += (size_t)n;
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      goto fail;
    }
  }
  if (fchmod(fd, 0600) != 0 || fsync(fd) != 0) goto fail;
  if (close(fd) != 0) { fd = -1; goto fail; }
  fd = -1;
  if (rename(temp_path, path) != 0) goto fail;
  dir_fd = open(secrets_dir, O_RDONLY);
  if (dir_fd >= 0) {
    if (fsync(dir_fd) != 0) { close(dir_fd); return -1; }
    close(dir_fd);
  }
  return 0;

fail:
  if (fd >= 0) close(fd);
  (void)unlink(temp_path);
  return -1;
}

int secret_store_get(const char *key, char *out, size_t out_sz) {
  char secrets_dir[SS_PATH_MAX];
  char path[SS_PATH_MAX];
  int fd;
  ssize_t n;
  if (!out || out_sz == 0) return -1;
  out[0] = '\0';
  if (secret_path(key, path, sizeof(path), secrets_dir, sizeof(secrets_dir)) != 0) return -1;
  fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  n = read(fd, out, out_sz - 1);
  if (n < 0) { close(fd); out[0] = '\0'; return -1; }
  out[n] = '\0';
  close(fd);
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
    out[--n] = '\0';
  }
  return out[0] ? 0 : -1;
}

int secret_store_list_prefix(const char *prefix,
                             int (*fn)(const char *suffix, void *user),
                             void *user) {
  char home[SS_PATH_MAX];
  char secrets_dir[SS_PATH_MAX];
  char sanitized[SS_KEY_MAX];
  DIR *d;
  struct dirent *ent;
  size_t plen;
  int rc = 0;
  if (!prefix || !fn) return -1;
  /* The prefix is a key fragment and must survive the same charset check
   * as a whole key, so a caller can never walk out of the store. */
  if (sanitize_key(prefix, sanitized, sizeof(sanitized)) != 0) return -1;
  if (resolve_home(home, sizeof(home)) != 0) return -1;
  if (snprintf(secrets_dir, sizeof(secrets_dir), "%s/secrets", home) >=
      (int)sizeof(secrets_dir))
    return -1;
  d = opendir(secrets_dir);
  if (!d) return 0; /* no store yet is not an error */
  plen = strlen(sanitized);
  while ((ent = readdir(d)) != NULL) {
    if (strncmp(ent->d_name, sanitized, plen) != 0) continue;
    if (ent->d_name[plen] == '\0') continue;
    if (fn(ent->d_name + plen, user) != 0) { rc = -1; break; }
  }
  closedir(d);
  return rc;
}

int secret_store_delete(const char *key) {
  char secrets_dir[SS_PATH_MAX];
  char path[SS_PATH_MAX];
  if (secret_path(key, path, sizeof(path), secrets_dir, sizeof(secrets_dir)) != 0) return -1;
  if (unlink(path) != 0 && errno != ENOENT) return -1;
  return 0;
}
