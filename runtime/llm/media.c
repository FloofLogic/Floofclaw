#include "media.h"

#include "../bus/media_manifest.h"
#include "../support/fsutil.h"
#include "../support/timing.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef FCLAW_HAVE_LIBCURL
#include <curl/curl.h>
#endif

_Static_assert(LLM_MEDIA_ID_MAX >= FC_MEDIA_ID_MAX,
               "LLM media id must hold ingress ids");
_Static_assert(LLM_MEDIA_FILENAME_MAX >= FC_MEDIA_FILENAME_MAX,
               "LLM media filename must hold ingress filenames");
_Static_assert(LLM_MEDIA_TYPE_MAX >= FC_MEDIA_TYPE_MAX,
               "LLM media MIME must hold ingress media types");

static char g_active_media_part[PATH_MAX];
static volatile sig_atomic_t g_active_media_part_ready;

static void cleanup_part_on_term(int signo) {
  if (g_active_media_part_ready)
    (void)unlink(g_active_media_part);
  _exit(128 + signo);
}

static int install_part_cleanup(struct sigaction *prior) {
  struct sigaction action;
  if (!prior) return -1;
  memset(&action, 0, sizeof(action));
  action.sa_handler = cleanup_part_on_term;
  sigemptyset(&action.sa_mask);
  return sigaction(SIGTERM, &action, prior);
}

static void restore_part_cleanup(const struct sigaction *prior) {
  if (prior) (void)sigaction(SIGTERM, prior, NULL);
}

static int media_error(char *err, size_t err_len, const char *format, ...) {
  va_list ap;
  if (err && err_len > 0U) {
    va_start(ap, format);
    (void)vsnprintf(err, err_len, format, ap);
    va_end(ap);
  }
  return -1;
}

static int test_loopback_enabled(void) {
  const char *value = getenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK");
  return value && strcmp(value, "1") == 0;
}

static uint64_t media_download_budget_ms(void) {
  uint64_t budget = LLM_MEDIA_DOWNLOAD_BUDGET_MS;
  const char *value;
  char *end = NULL;
  unsigned long parsed;
  if (!test_loopback_enabled()) return budget;
  value = getenv("FCLAW_MEDIA_TEST_DOWNLOAD_BUDGET_MS");
  if (!value || !*value) return budget;
  errno = 0;
  parsed = strtoul(value, &end, 10);
  if (errno == 0 && end && *end == '\0' && parsed > 0U &&
      parsed <= LLM_MEDIA_DOWNLOAD_BUDGET_MS)
    budget = (uint64_t)parsed;
  return budget;
}

static int prune_stale_partials(const char *media_dir,
                                char *err, size_t err_len) {
  DIR *dir;
  struct dirent *entry;
  int rc = 0;
  dir = opendir(media_dir);
  if (!dir)
    return media_error(err, err_len,
                       "cannot inspect run media directory");
  errno = 0;
  while ((entry = readdir(dir)) != NULL) {
    char path[PATH_MAX];
    struct stat st;
    if (!strstr(entry->d_name, ".part.")) continue;
    if (snprintf(path, sizeof(path), "%s/%s", media_dir, entry->d_name) >=
            (int)sizeof(path) ||
        lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != getuid() || unlink(path) != 0) {
      rc = media_error(err, err_len,
                       "run media directory contains an unsafe partial file");
      break;
    }
    errno = 0;
  }
  if (rc == 0 && errno != 0)
    rc = media_error(err, err_len,
                     "cannot finish inspecting run media directory");
  if (closedir(dir) != 0 && rc == 0)
    rc = media_error(err, err_len,
                     "cannot close run media directory");
  return rc;
}

static int canonical_manifest_arg(const char *arg,
                                  char out[FC_MEDIA_MANIFEST_PATH_SIZE]) {
  static const char relative_prefix[] = "media/inbound/";
  static const char workspace_prefix[] = "workspace/media/inbound/";
  const char *relative = NULL;
  size_t len;
  if (!arg || !*arg || !out) return -1;
  if (strncmp(arg, relative_prefix, sizeof(relative_prefix) - 1U) == 0) {
    relative = arg;
  } else if (strncmp(arg, workspace_prefix,
                     sizeof(workspace_prefix) - 1U) == 0) {
    relative = arg + sizeof("workspace/") - 1U;
  } else if (arg[0] == '/') {
    char workspace[PATH_MAX];
    char prefix[PATH_MAX];
    if (!realpath("workspace", workspace) ||
        snprintf(prefix, sizeof(prefix), "%s/", workspace) >=
            (int)sizeof(prefix) ||
        strncmp(arg, prefix, strlen(prefix)) != 0)
      return -1;
    relative = arg + strlen(prefix);
  } else {
    return -1;
  }
  len = strlen(relative);
  if (len == 0U || len >= FC_MEDIA_MANIFEST_PATH_SIZE ||
      snprintf(out, FC_MEDIA_MANIFEST_PATH_SIZE, "%s", relative) >=
          (int)FC_MEDIA_MANIFEST_PATH_SIZE)
    return -1;
  return 0;
}

/* Channel adapters own transport-specific source authority. By the time this
 * child sees a canonical private manifest, its descriptors have passed that
 * adapter policy and the generic manifest reader's HTTPS envelope checks.
 * The materializer knows only which curl protocol to permit. */
static int source_protocol(const char *url, int *loopback_out) {
  static const char https_prefix[] = "https://";
  static const char test_prefix[] = "http://127.0.0.1:";
  if (loopback_out) *loopback_out = 0;
  if (!url) return 0;
  if (strncmp(url, https_prefix, sizeof(https_prefix) - 1U) == 0) return 1;
  if (test_loopback_enabled() &&
      strncmp(url, test_prefix, sizeof(test_prefix) - 1U) == 0) {
    const char *p = url + sizeof(test_prefix) - 1U;
    unsigned port = 0U;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
      if (port > 6553U || (port == 6553U && (unsigned)(*p - '0') > 5U))
        return 0;
      port = port * 10U + (unsigned)(*p - '0');
      p++;
      digits++;
    }
    if (!digits || port == 0U || *p != '/') return 0;
    if (loopback_out) *loopback_out = 1;
    return 1;
  }
  return 0;
}

static int private_regular_file(const char *path, uint64_t expected_size) {
  struct stat st;
  if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
  if (!S_ISREG(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & 077U) != 0U ||
      st.st_size < 0 || (uint64_t)st.st_size != expected_size)
    return -1;
  return 1;
}

#ifdef FCLAW_HAVE_LIBCURL

typedef struct {
  int fd;
  uint64_t expected;
  uint64_t written;
  int failed;
} MediaDownload;

static size_t download_write(void *ptr, size_t size, size_t nmemb,
                             void *userdata) {
  MediaDownload *download = (MediaDownload *)userdata;
  size_t want;
  size_t off = 0U;
  if (!download || (size != 0U && nmemb > SIZE_MAX / size)) return 0U;
  want = size * nmemb;
  if ((uint64_t)want > UINT64_MAX - download->written ||
      download->written + (uint64_t)want > download->expected) {
    download->failed = 1;
    return 0U;
  }
  while (off < want) {
    ssize_t n;
    do {
      n = write(download->fd, (const char *)ptr + off, want - off);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
      download->failed = 1;
      return 0U;
    }
    off += (size_t)n;
  }
  download->written += (uint64_t)want;
  return want;
}

static int content_type_matches(const char *actual, const char *declared) {
  char normalized[FC_MEDIA_TYPE_MAX];
  const char *start;
  const char *end;
  size_t len;
  if (!actual || !*actual) return 1;
  start = actual;
  while (*start && isspace((unsigned char)*start)) start++;
  end = strchr(start, ';');
  if (!end) end = start + strlen(start);
  while (end > start && isspace((unsigned char)end[-1])) end--;
  len = (size_t)(end - start);
  if (len == 0U || len >= sizeof(normalized)) return 0;
  memcpy(normalized, start, len);
  normalized[len] = '\0';
  return strcasecmp(normalized, declared) == 0;
}

static int create_part_file(const char *target, char *part, size_t part_len) {
  for (unsigned attempt = 0U; attempt < 16U; ++attempt) {
    int n = snprintf(part, part_len, "%s.part.%ld.%u",
                     target, (long)getpid(), attempt);
    int flags = O_WRONLY | O_CREAT | O_EXCL;
    int fd;
    if (n < 0 || (size_t)n >= part_len) return -1;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(part, flags, 0600);
    if (fd >= 0) return fd;
    if (errno != EEXIST) return -1;
  }
  return -1;
}

static int create_active_part_file(const char *target,
                                   char *part, size_t part_len) {
  sigset_t blocked;
  sigset_t prior;
  int fd;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &blocked, &prior) != 0) return -1;
  fd = create_part_file(target, part, part_len);
  if (fd >= 0) {
    if (snprintf(g_active_media_part, sizeof(g_active_media_part),
                 "%s", part) >= (int)sizeof(g_active_media_part)) {
      (void)close(fd);
      (void)unlink(part);
      fd = -1;
    } else {
      g_active_media_part_ready = 1;
    }
  }
  (void)sigprocmask(SIG_SETMASK, &prior, NULL);
  return fd;
}

static void clear_active_part(void) {
  sigset_t blocked;
  sigset_t prior;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &blocked, &prior) != 0) {
    g_active_media_part_ready = 0;
    g_active_media_part[0] = '\0';
    return;
  }
  g_active_media_part_ready = 0;
  g_active_media_part[0] = '\0';
  (void)sigprocmask(SIG_SETMASK, &prior, NULL);
}

static int sync_directory(const char *path) {
  int flags = O_RDONLY;
  int fd;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  fd = open(path, flags);
  if (fd < 0) return -1;
  if (fsync(fd) != 0) {
    close(fd);
    return -1;
  }
  return close(fd);
}

static int download_item(const FcMediaDescriptor *item, size_t index,
                         const char *media_dir, const char *target,
                         int loopback, long timeout_ms,
                         char *err, size_t err_len) {
  MediaDownload download;
  CURL *curl = NULL;
  CURLcode curl_rc;
  curl_off_t content_length = -1;
  char part[PATH_MAX];
  char *content_type = NULL;
  long status = 0;
  int fd = -1;
  int cached;
  int rc = -1;

  cached = private_regular_file(target, item->size_bytes);
  if (cached > 0) return 0;
  if (cached < 0)
    return media_error(err, err_len,
                       "media item %zu cache is unsafe or has wrong size",
                       index);
  if (timeout_ms <= 0)
    return media_error(err, err_len,
                       "media download budget exhausted before item %zu",
                       index);

  part[0] = '\0';
  fd = create_active_part_file(target, part, sizeof(part));
  if (fd < 0)
    return media_error(err, err_len,
                       "media item %zu cannot create partial file", index);
  memset(&download, 0, sizeof(download));
  download.fd = fd;
  download.expected = item->size_bytes;

  curl = curl_easy_init();
  if (!curl) {
    (void)media_error(err, err_len,
                      "media item %zu cannot initialize downloader", index);
    goto done;
  }
  curl_easy_setopt(curl, CURLOPT_URL, item->source_url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                   timeout_ms < 5000L ? timeout_ms : 5000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                   (curl_off_t)item->size_bytes);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, loopback ? "http" : "https");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "floofclaw-media/1");
  curl_rc = curl_easy_perform(curl);
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  (void)curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                          &content_length);
  (void)curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
  if (curl_rc != CURLE_OK || status < 200 || status >= 300) {
    (void)media_error(err, err_len,
                      "media item %zu download failed", index);
    goto done;
  }
  if (download.failed || download.written != item->size_bytes ||
      (content_length >= 0 &&
       (uint64_t)content_length != item->size_bytes)) {
    (void)media_error(err, err_len,
                      "media item %zu downloaded size does not match manifest",
                      index);
    goto done;
  }
  if (!content_type_matches(content_type, item->media_type)) {
    (void)media_error(err, err_len,
                      "media item %zu Content-Type does not match manifest",
                      index);
    goto done;
  }
  {
    int sync_rc = fsync(fd);
    int close_rc = close(fd);
    fd = -1;
    if (sync_rc != 0 || close_rc != 0) {
      (void)media_error(err, err_len,
                        "media item %zu cannot commit partial file", index);
      goto done;
    }
  }
  if (private_regular_file(part, item->size_bytes) <= 0 ||
      rename(part, target) != 0 ||
      sync_directory(media_dir) != 0) {
    (void)media_error(err, err_len,
                      "media item %zu cannot install run-local file", index);
    goto done;
  }
  part[0] = '\0';
  rc = 0;

done:
  if (curl) curl_easy_cleanup(curl);
  if (fd >= 0) (void)close(fd);
  if (part[0]) (void)unlink(part);
  clear_active_part();
  return rc;
}

#else

static int download_item(const FcMediaDescriptor *item, size_t index,
                         const char *media_dir, const char *target,
                         int loopback, long timeout_ms,
                         char *err, size_t err_len) {
  (void)item;
  (void)media_dir;
  (void)target;
  (void)loopback;
  (void)timeout_ms;
  return media_error(err, err_len,
                     "media item %zu requires libcurl support", index);
}

#endif

int llm_media_load(const char *run_dir, const char *manifest_path,
                   LlmMedia **media_out, size_t *media_count_out,
                   uint64_t *media_bytes_out,
                   char *err, size_t err_len) {
  FcMediaManifest manifest;
  FcMediaManifestRef verified_ref;
  LlmMedia *media = NULL;
  char canonical[FC_MEDIA_MANIFEST_PATH_SIZE];
  char media_dir[PATH_MAX];
  struct stat media_dir_stat;
  struct sigaction prior_term;
  uint64_t download_deadline_ms = 0U;
  int cleanup_installed = 0;
  int rc = -1;

  if (err && err_len > 0U) err[0] = '\0';
  if (media_out) *media_out = NULL;
  if (media_count_out) *media_count_out = 0U;
  if (media_bytes_out) *media_bytes_out = 0U;
  if (!run_dir || !*run_dir || !manifest_path || !media_out ||
      !media_count_out || !media_bytes_out)
    return media_error(err, err_len, "invalid media loader arguments");
  if (canonical_manifest_arg(manifest_path, canonical) != 0)
    return media_error(err, err_len, "media manifest path is invalid");

  memset(&manifest, 0, sizeof(manifest));
  memset(&verified_ref, 0, sizeof(verified_ref));
  if (fc_media_manifest_read_path(canonical, &manifest, &verified_ref,
                                  err, err_len) != 0)
    return -1;
  if (snprintf(media_dir, sizeof(media_dir), "%s/media", run_dir) >=
          (int)sizeof(media_dir) ||
      fs_mkdir_p(media_dir) != 0 ||
      lstat(media_dir, &media_dir_stat) != 0 ||
      !S_ISDIR(media_dir_stat.st_mode) ||
      media_dir_stat.st_uid != getuid() ||
      chmod(media_dir, 0700) != 0) {
    (void)media_error(err, err_len, "cannot create run media directory");
    goto done;
  }
  if (prune_stale_partials(media_dir, err, err_len) != 0)
    goto done;
  media = (LlmMedia *)calloc(manifest.count, sizeof(*media));
  if (!media) {
    (void)media_error(err, err_len, "media descriptor allocation failed");
    goto done;
  }
  clear_active_part();
  if (install_part_cleanup(&prior_term) != 0) {
    (void)media_error(err, err_len,
                      "cannot install media partial-file cleanup");
    goto done;
  }
  cleanup_installed = 1;
  {
    uint64_t now = timing_monotonic_ms();
    uint64_t budget = media_download_budget_ms();
    download_deadline_ms =
        now > UINT64_MAX - budget ? UINT64_MAX : now + budget;
  }

  for (size_t i = 0; i < manifest.count; ++i) {
    const FcMediaDescriptor *item = &manifest.items[i];
    char target[PATH_MAX];
    uint64_t now;
    uint64_t remaining;
    long timeout_ms;
    int loopback = 0;
    if (!source_protocol(item->source_url, &loopback)) {
      (void)media_error(err, err_len,
                        "media item %zu source is not an approved ingress URL",
                        i);
      goto done;
    }
    if (snprintf(target, sizeof(target), "%s/%s_%02zu.bin",
                 media_dir, verified_ref.sha256, i) >= (int)sizeof(target) ||
        snprintf(media[i].id, sizeof(media[i].id), "%s", item->id) >=
            (int)sizeof(media[i].id) ||
        snprintf(media[i].filename, sizeof(media[i].filename), "%s",
                 item->filename) >= (int)sizeof(media[i].filename) ||
        snprintf(media[i].media_type, sizeof(media[i].media_type), "%s",
                 item->media_type) >= (int)sizeof(media[i].media_type) ||
        snprintf(media[i].path, sizeof(media[i].path), "%s", target) >=
            (int)sizeof(media[i].path)) {
      (void)media_error(err, err_len,
                        "media item %zu metadata exceeds local boundary", i);
      goto done;
    }
    media[i].size_bytes = item->size_bytes;
    now = timing_monotonic_ms();
    remaining = now < download_deadline_ms
                    ? download_deadline_ms - now : 0U;
    timeout_ms = remaining > (uint64_t)LONG_MAX
                     ? LONG_MAX : (long)remaining;
    if (download_item(item, i, media_dir, target, loopback, timeout_ms,
                      err, err_len) != 0)
      goto done;
  }

  *media_out = media;
  *media_count_out = manifest.count;
  *media_bytes_out = manifest.total_bytes;
  media = NULL;
  rc = 0;

done:
  if (cleanup_installed) {
    clear_active_part();
    restore_part_cleanup(&prior_term);
  }
  free(media);
  fc_media_manifest_dispose(&manifest);
  return rc;
}

void llm_media_dispose(LlmMedia *media) {
  free(media);
}
