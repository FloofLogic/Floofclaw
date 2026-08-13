#include "log_rotation.h"

#include "fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Shift slot N -> N+1 for either plain (X.N) or gzipped (X.N.gz). Both
 * variants may exist across cold restarts; renaming whichever is present
 * is safe. */
static void shift_slot(const char *path, int from, int to) {
  char src_plain[512], src_gz[512], dst_plain[512], dst_gz[512];
  snprintf(src_plain, sizeof(src_plain), "%s.%d", path, from);
  snprintf(src_gz,    sizeof(src_gz),    "%s.%d.gz", path, from);
  snprintf(dst_plain, sizeof(dst_plain), "%s.%d", path, to);
  snprintf(dst_gz,    sizeof(dst_gz),    "%s.%d.gz", path, to);
  if (fs_file_exists(src_plain)) (void)rename(src_plain, dst_plain);
  if (fs_file_exists(src_gz))    (void)rename(src_gz,    dst_gz);
}

static void unlink_slot(const char *path, int slot) {
  char plain[512], gz[512];
  snprintf(plain, sizeof(plain), "%s.%d", path, slot);
  snprintf(gz,    sizeof(gz),    "%s.%d.gz", path, slot);
  (void)unlink(plain);
  (void)unlink(gz);
}

/* Non-blocking gzip via double-fork so no zombie. If gzip is missing on
 * PATH the child exits 127 silently — callers do a preflight probe via
 * fc_rotation_gzip_available and turn off compression when it's absent. */
static void spawn_gzip(const char *path) {
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid == 0) {
    pid_t grandchild = fork();
    if (grandchild == 0) {
      execlp("gzip", "gzip", "-q", "-f", "--", path, (char *)NULL);
      _exit(127);
    }
    _exit(0);
  }
  (void)waitpid(pid, NULL, 0);
}

int fc_rotation_gzip_available(void) {
  pid_t pid = fork();
  int status;
  if (pid < 0) return 0;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
    execlp("gzip", "gzip", "--version", (char *)NULL);
    _exit(127);
  }
  if (waitpid(pid, &status, 0) < 0) return 0;
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int fc_rotate_file(const char *path, int keep, int compress,
                   char *out_err_msg, unsigned long out_err_len) {
  int slot;
  char first[512];
  if (out_err_msg && out_err_len) out_err_msg[0] = '\0';
  if (!path || !*path || keep <= 0) {
    if (out_err_msg && out_err_len)
      snprintf(out_err_msg, (size_t)out_err_len, "path or keep invalid");
    return -1;
  }
  /* Refuse ".." in the path — a config or agent mistake shouldn't be
   * able to reach outside the intended workspace. Callers are expected
   * to pass workspace-relative paths; we don't otherwise normalize. */
  if (strstr(path, "..")) {
    if (out_err_msg && out_err_len)
      snprintf(out_err_msg, (size_t)out_err_len, "path contains ..: %s", path);
    return -1;
  }
  if (!fs_file_exists(path)) {
    if (out_err_msg && out_err_len)
      snprintf(out_err_msg, (size_t)out_err_len, "file does not exist: %s", path);
    return -1;
  }
  unlink_slot(path, keep);
  for (slot = keep - 1; slot >= 1; --slot) shift_slot(path, slot, slot + 1);
  snprintf(first, sizeof(first), "%s.1", path);
  if (rename(path, first) != 0) {
    if (out_err_msg && out_err_len)
      snprintf(out_err_msg, (size_t)out_err_len,
               "rename %s -> %s failed: %s", path, first, strerror(errno));
    return -1;
  }
  if (compress) spawn_gzip(first);
  return 0;
}
