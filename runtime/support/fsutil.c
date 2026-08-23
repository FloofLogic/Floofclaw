#include "fsutil.h"

#include "heap_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static long g_readdir_fail_after = -1;

struct dirent *fs_readdir_checked(DIR *dir) {
  if (g_readdir_fail_after == 0) {
    g_readdir_fail_after = -1;
    errno = EIO;
    return NULL;
  }
  if (g_readdir_fail_after > 0) g_readdir_fail_after--;
  /* POSIX readdir() leaves errno untouched at end-of-directory, so any
   * stale errno from work done between entries (stat, open, parse) would
   * otherwise read as a scan failure at EOF. Clear it here so NULL with
   * errno==0 always means clean EOF. */
  errno = 0;
  return readdir(dir);
}

void fs_test_fail_readdir_after(long successful_calls) {
  g_readdir_fail_after = successful_calls < 0 ? 0 : successful_calls;
}

void fs_test_reset_readdir(void) {
  g_readdir_fail_after = -1;
}

int fs_mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  size_t len;
  if (path == NULL || *path == '\0') return -1;
  len = strlen(path);
  if (len >= sizeof(tmp)) return -1;
  memcpy(tmp, path, len + 1);
  if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
  for (char *p = tmp + 1; *p; ++p) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

int fs_file_exists(const char *path) {
  struct stat st;
  if (!path) return 0;
  return stat(path, &st) == 0;
}

int fs_dir_exists(const char *path) {
  struct stat st;
  if (!path) return 0;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int fs_write_text(const char *path, const char *text) {
  FILE *fp = fopen(path, "w");
  if (!fp) return -1;
  if (text && fputs(text, fp) == EOF) { fclose(fp); return -1; }
  if (fclose(fp) != 0) return -1;
  return 0;
}

static int write_all(int fd, const char *text, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, text + off, len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

/* Chaos-only pause points. They are inert unless an explicit test match and
 * marker path are both present in the environment. The marker is written with
 * raw syscalls so instrumentation never recurses through these helpers. */
static int chaos_path_matches(const char *env_name, const char *path) {
  const char *match = getenv(env_name);
  size_t path_len;
  size_t match_len;
  if (!match || !*match || !path) return 0;
  path_len = strlen(path);
  match_len = strlen(match);
  return path_len >= match_len &&
         strcmp(path + path_len - match_len, match) == 0;
}

static void chaos_pause(const char *label) {
  const char *marker = getenv("FCLAW_TEST_CHAOS_MARKER");
  int fd;
  if (!marker || !*marker) return;
  fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return;
  (void)write_all(fd, label, strlen(label));
  (void)close(fd);
  (void)kill(getpid(), SIGSTOP);
}

static int sync_fd(int fd) {
  int rc;
  do {
    rc = fsync(fd);
  } while (rc != 0 && errno == EINTR);
  return rc;
}

int fs_write_text_atomic(const char *path, const char *text) {
  static unsigned long sequence;
  static int chaos_before_fired;
  static int chaos_fired;
  char tmp[PATH_MAX];
  size_t len;
  int fd = -1;
  int saved_errno;
  int preserve_mode = 0;
  mode_t mode = 0666;
  struct stat st;
  if (!path || !*path) { errno = EINVAL; return -1; }
  if (!text) text = "";
  len = strlen(text);
  if (!chaos_before_fired && len > 0 &&
      chaos_path_matches("FCLAW_TEST_CHAOS_ATOMIC_BEFORE_MATCH", path)) {
    chaos_before_fired = 1;
    chaos_pause("atomic_before_write\n");
  }
  if (stat(path, &st) == 0) {
    preserve_mode = 1;
    mode = st.st_mode & 07777;
  }
  for (int attempt = 0; attempt < 100; ++attempt) {
    unsigned long claim = ++sequence;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.%lu",
                 path, (long)getpid(), claim) >= (int)sizeof(tmp)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd >= 0) break;
    if (errno != EEXIST) return -1;
  }
  if (fd < 0) { errno = EEXIST; return -1; }
  if (preserve_mode && fchmod(fd, mode) != 0) {
    saved_errno = errno;
    (void)close(fd);
    (void)unlink(tmp);
    errno = saved_errno;
    return -1;
  }
  if (!chaos_fired && len > 1 &&
      chaos_path_matches("FCLAW_TEST_CHAOS_ATOMIC_MATCH", path)) {
    size_t split = len / 2U;
    if (write_all(fd, text, split) != 0) {
      saved_errno = errno;
      (void)close(fd);
      (void)unlink(tmp);
      errno = saved_errno;
      return -1;
    }
    chaos_fired = 1;
    chaos_pause("atomic_write\n");
    if (write_all(fd, text + split, len - split) != 0) {
      saved_errno = errno;
      (void)close(fd);
      (void)unlink(tmp);
      errno = saved_errno;
      return -1;
    }
  } else if (write_all(fd, text, len) != 0) {
    saved_errno = errno;
    (void)close(fd);
    fd = -1;
    (void)unlink(tmp);
    errno = saved_errno;
    return -1;
  }
  if (sync_fd(fd) != 0) {
    saved_errno = errno;
    (void)close(fd);
    fd = -1;
    (void)unlink(tmp);
    errno = saved_errno;
    return -1;
  }
  if (close(fd) != 0) {
    saved_errno = errno;
    fd = -1;
    (void)unlink(tmp);
    errno = saved_errno;
    return -1;
  }
  fd = -1;
  if (rename(tmp, path) != 0) {
    saved_errno = errno;
    (void)unlink(tmp);
    errno = saved_errno;
    return -1;
  }
  return 0;
}

int fs_append_text(const char *path, const char *text) {
  static int chaos_fired;
  if (!chaos_fired && text && strlen(text) > 1 &&
      chaos_path_matches("FCLAW_TEST_CHAOS_APPEND_MATCH", path)) {
    size_t len = strlen(text);
    size_t split = len / 2U;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    int saved_errno;
    if (fd < 0) return -1;
    if (write_all(fd, text, split) != 0) {
      saved_errno = errno;
      (void)close(fd);
      errno = saved_errno;
      return -1;
    }
    chaos_fired = 1;
    chaos_pause("event_append\n");
    if (write_all(fd, text + split, len - split) != 0) {
      saved_errno = errno;
      (void)close(fd);
      errno = saved_errno;
      return -1;
    }
    if (close(fd) == 0) return 0;
    saved_errno = errno;
    errno = saved_errno;
    return -1;
  }
  FILE *fp = fopen(path, "a");
  if (!fp) return -1;
  if (text && fputs(text, fp) == EOF) { fclose(fp); return -1; }
  if (fclose(fp) != 0) return -1;
  return 0;
}

/* Test instrumentation is deliberately narrow: it can fail only the sync
 * step of the next synchronous append. Production callers never arm it. */
static int g_fail_next_append_sync;
static unsigned long g_append_sync_attempts;

void fs_test_fail_next_append_sync(void) {
  g_fail_next_append_sync = 1;
}

unsigned long fs_test_append_sync_attempts(void) {
  return g_append_sync_attempts;
}

void fs_test_reset_append_sync(void) {
  g_fail_next_append_sync = 0;
  g_append_sync_attempts = 0;
}

int fs_append_text_sync(const char *path, const char *text) {
  int fd;
  int saved_errno;
  size_t len;
  if (!path || !*path) { errno = EINVAL; return -1; }
  if (!text) text = "";
  len = strlen(text);
  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0) return -1;
  if (write_all(fd, text, len) != 0) {
    saved_errno = errno;
    (void)close(fd);
    errno = saved_errno;
    return -1;
  }
  g_append_sync_attempts++;
  if (g_fail_next_append_sync) {
    g_fail_next_append_sync = 0;
    errno = EIO;
  } else if (sync_fd(fd) == 0) {
    if (close(fd) == 0) return 0;
    return -1;
  }
  saved_errno = errno;
  (void)close(fd);
  errno = saved_errno;
  return -1;
}

int fs_truncate_incomplete_line(const char *path) {
  char block[4096];
  struct stat st;
  off_t cursor;
  off_t keep = 0;
  int fd;
  int saved_errno;
  if (!path || !*path) { errno = EINVAL; return -1; }
  fd = open(path, O_RDWR);
  if (fd < 0) return errno == ENOENT ? 0 : -1;
  if (fstat(fd, &st) != 0) goto fail;
  if (st.st_size == 0) {
    if (close(fd) == 0) return 0;
    return -1;
  }
  {
    char last;
    ssize_t n;
    do {
      n = pread(fd, &last, 1, st.st_size - 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) goto fail;
    if (last == '\n') {
      if (close(fd) == 0) return 0;
      return -1;
    }
  }
  cursor = st.st_size;
  while (cursor > 0) {
    size_t want = cursor > (off_t)sizeof(block) ? sizeof(block) : (size_t)cursor;
    off_t start = cursor - (off_t)want;
    ssize_t n;
    do {
      n = pread(fd, block, want, start);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)want) goto fail;
    for (size_t i = want; i > 0; --i) {
      if (block[i - 1] == '\n') {
        keep = start + (off_t)i;
        cursor = 0;
        break;
      }
    }
    if (cursor > 0) cursor = start;
  }
  while (ftruncate(fd, keep) != 0) {
    if (errno == EINTR) continue;
    goto fail;
  }
  if (sync_fd(fd) != 0) goto fail;
  if (close(fd) == 0) return 0;
  return -1;

fail:
  saved_errno = errno;
  (void)close(fd);
  errno = saved_errno;
  return -1;
}

static size_t read_cap(size_t requested) {
  if (requested == 0 || requested > FS_READ_TEXT_DEFAULT_CAP)
    return FS_READ_TEXT_DEFAULT_CAP;
  return requested;
}

static int seek_file_size(FILE *fp, off_t *out_size) {
  off_t size;
  if (!fp || !out_size) return -1;
  if (fseeko(fp, 0, SEEK_END) != 0) return -1;
  size = ftello(fp);
  if (size < 0) return -1;
  *out_size = size;
  return 0;
}

int fs_read_text(const char *path, char **out_text, size_t max_bytes) {
  FILE *fp;
  off_t size;
  size_t cap;
  char *buffer;
  size_t read_n;
  if (!path || !out_text) return -1;
  *out_text = NULL;
  fp = fopen(path, "rb");
  if (!fp) return -1;
  if (seek_file_size(fp, &size) != 0) { fclose(fp); return -1; }
  cap = read_cap(max_bytes);
  if ((uintmax_t)size > (uintmax_t)cap) {
    fprintf(stderr,
            "fs_read_text: %s is %" PRIuMAX
            " bytes, exceeds %zu-byte cap\n",
            path, (uintmax_t)size, cap);
    fclose(fp);
    errno = EFBIG;
    return FS_READ_TOO_LARGE;
  }
  if (fseeko(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
  buffer = (char *)fc_xmalloc((size_t)size + 1U);
  if (!buffer) { fclose(fp); return -1; }
  read_n = fread(buffer, 1U, (size_t)size, fp);
  if (read_n != (size_t)size) { fc_xfree(buffer); fclose(fp); return -1; }
  buffer[size] = '\0';
  if (fclose(fp) != 0) { fc_xfree(buffer); return -1; }
  *out_text = buffer;
  return 0;
}

int fs_read_tail(const char *path, char **out_text, size_t max_bytes) {
  FILE *fp;
  off_t size;
  off_t start;
  size_t cap;
  size_t wanted;
  size_t used;
  char *buffer;
  int starts_at_record = 1;
  if (!path || !out_text) return -1;
  *out_text = NULL;
  fp = fopen(path, "rb");
  if (!fp) return -1;
  if (seek_file_size(fp, &size) != 0) { fclose(fp); return -1; }
  cap = read_cap(max_bytes);
  wanted = (uintmax_t)size > (uintmax_t)cap ? cap : (size_t)size;
  start = size - (off_t)wanted;
  if (start > 0) {
    if (fseeko(fp, start - 1, SEEK_SET) != 0) { fclose(fp); return -1; }
    starts_at_record = fgetc(fp) == '\n';
  }
  if (fseeko(fp, start, SEEK_SET) != 0) { fclose(fp); return -1; }
  buffer = (char *)fc_xmalloc(wanted + 1U);
  if (!buffer) { fclose(fp); return -1; }
  used = fread(buffer, 1U, wanted, fp);
  if (used != wanted) { fc_xfree(buffer); fclose(fp); return -1; }
  if (fclose(fp) != 0) { fc_xfree(buffer); return -1; }
  if (start > 0 && !starts_at_record) {
    char *newline = (char *)memchr(buffer, '\n', used);
    if (newline) {
      size_t discard = (size_t)(newline - buffer) + 1U;
      used -= discard;
      memmove(buffer, newline + 1, used);
    } else {
      used = 0;
    }
  }
  buffer[used] = '\0';
  *out_text = buffer;
  return 0;
}

int fs_join(char *out, size_t out_len, const char *left, const char *right) {
  size_t left_len;
  int needs_sep;
  int written;
  if (!out || out_len == 0 || !left || !right) return -1;
  left_len = strlen(left);
  needs_sep = left_len > 0 && left[left_len - 1] != '/';
  written = snprintf(out, out_len, "%s%s%s", left, needs_sep ? "/" : "", right);
  if (written < 0 || (size_t)written >= out_len) return -1;
  return 0;
}

void fs_trim_newline(char *text) {
  size_t len;
  if (!text) return;
  len = strlen(text);
  while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
    text[len - 1] = '\0';
    len--;
  }
}

long long fs_file_size(const char *path) {
  struct stat st;
  if (!path || stat(path, &st) != 0) return -1;
  return (long long)st.st_size;
}
