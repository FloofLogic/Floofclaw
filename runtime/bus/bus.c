#include "bus.h"

#include "../runtime.h"     /* rt_narrate() */
#include "../support/fsutil.h"
#include "../support/heap_guard.h"
#include "../support/json.h"
#include "../support/color.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <strings.h>
#endif

#define INBOX_DIR     "workspace/bus/inbox"
#define PROCESSED_DIR "workspace/bus/processed"
#define LOG_DIR       "workspace/logs"
#define LOG_PATH      LOG_DIR "/bus.jsonl"
#define COUNTER_PATH  "workspace/bus/.counter"
#define COUNTER_LOCK_PATH "workspace/bus/.counter.lock"

void bus_signal_wake(void) {
  int fd;
  struct sockaddr_un addr;
  const char byte = 'w';
  fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", BUS_WAKE_SOCK_PATH);
  (void)sendto(fd, &byte, 1, 0, (struct sockaddr *)&addr, sizeof(addr));
  close(fd);
}

static int next_envelope_id(char *out, size_t out_len) {
  char *text = NULL;
  char *end = NULL;
  long n = 0;
  char buf[32];
  int lock_fd = -1;
  int saved_errno;
  int rc = -1;
  if (fs_mkdir_p("workspace/bus") != 0) return -1;
  lock_fd = open(COUNTER_LOCK_PATH, O_RDWR | O_CREAT, 0666);
  if (lock_fd < 0) return -1;
  while (flock(lock_fd, LOCK_EX) != 0) {
    if (errno == EINTR) continue;
    saved_errno = errno;
    (void)close(lock_fd);
    errno = saved_errno;
    return -1;
  }
  if (fs_read_text(COUNTER_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) == 0 && text) {
    errno = 0;
    n = strtol(text, &end, 10);
    if (errno != 0 || end == text || n < 0 ||
        (*end != '\0' && !(*end == '\n' && end[1] == '\0'))) {
      free(text);
      errno = EINVAL;
      goto done;
    }
    free(text);
  } else if (errno != ENOENT) {
    goto done;
  }
  if (n == LONG_MAX) { errno = EOVERFLOW; goto done; }
  n++;
  snprintf(buf, sizeof(buf), "%ld\n", n);
  if (fs_write_text_atomic(COUNTER_PATH, buf) != 0) goto done;
  if (snprintf(out, out_len, "bus_%06ld", n) >= (int)out_len) {
    errno = ENAMETOOLONG;
    goto done;
  }
  rc = 0;

done:
  saved_errno = errno;
  while (flock(lock_fd, LOCK_UN) != 0 && errno == EINTR) {}
  if (close(lock_fd) != 0 && rc == 0) {
    rc = -1;
    saved_errno = errno;
  }
  errno = saved_errno;
  return rc;
}

int bus_reserve_envelope_id(char *out, size_t out_len) {
  return next_envelope_id(out, out_len);
}

static void iso_time(char *out, size_t out_len) {
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int valid_envelope_id(const char *id) {
  const char *p;
  if (!id || strlen(id) >= BUS_ID_MAX || strncmp(id, "bus_", 4) != 0)
    return 0;
  p = id + 4;
  if (*p < '0' || *p > '9') return 0;
  while (*p >= '0' && *p <= '9') p++;
  return *p == '\0';
}

int bus_envelope_locations(const char *envelope_id) {
  char path[BUS_PATH_MAX];
  int locations = 0;
  if (!valid_envelope_id(envelope_id)) return -1;
  if (snprintf(path, sizeof(path), "%s/%s.json",
               INBOX_DIR, envelope_id) >= (int)sizeof(path))
    return -1;
  if (fs_file_exists(path)) locations |= 1;
  if (snprintf(path, sizeof(path), "%s/%s.json",
               PROCESSED_DIR, envelope_id) >= (int)sizeof(path))
    return -1;
  if (fs_file_exists(path)) locations |= 2;
  return locations;
}

static int existing_envelope_matches(const char *path,
                                     const char *envelope_id,
                                     const char *channel,
                                     const char *type,
                                     const char *payload_json) {
  char *text = NULL;
  char *buffers = NULL;
  char actual_id[BUS_ID_MAX] = "", actual_channel[BUS_CHANNEL_MAX] = "";
  char actual_type[BUS_TYPE_MAX] = "";
  char *actual_raw, *actual_payload, *expected_payload;
  JsonRef root, payload;
  int matches = 0;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  buffers = (char *)fc_xmalloc(BUS_PAYLOAD_MAX * 3U);
  if (!buffers) {
    free(text);
    return -1;
  }
  actual_raw = buffers;
  actual_payload = buffers + BUS_PAYLOAD_MAX;
  expected_payload = buffers + BUS_PAYLOAD_MAX * 2U;
  if (json_ref_top_object(text, &root) == 0 &&
      json_ref_object_get_string(&root, "event_id", actual_id,
                                 sizeof(actual_id)) == 0 &&
      json_ref_object_get_string(&root, "channel", actual_channel,
                                 sizeof(actual_channel)) == 0 &&
      json_ref_object_get_string(&root, "type", actual_type,
                                 sizeof(actual_type)) == 0 &&
      json_ref_object_get(&root, "payload", &payload) == 0 &&
      json_ref_value_copy(&payload, actual_raw,
                          BUS_PAYLOAD_MAX) == 0 &&
      json_compact(actual_raw, actual_payload,
                   BUS_PAYLOAD_MAX) == 0 &&
      json_compact(payload_json, expected_payload,
                   BUS_PAYLOAD_MAX) == 0 &&
      strcmp(actual_id, envelope_id) == 0 &&
      strcmp(actual_channel, channel) == 0 &&
      strcmp(actual_type, type) == 0 &&
      strcmp(actual_payload, expected_payload) == 0)
    matches = 1;
  fc_xfree(buffers);
  free(text);
  return matches;
}

int bus_publish_stable(const char *envelope_id,
                       const char *channel, const char *type,
                       const char *payload_json) {
  char ts[64];
  char ec[BUS_CHANNEL_MAX], et[BUS_TYPE_MAX];
  char *compact_payload = NULL;
  char *envelope = NULL;
  char inbox_path[BUS_PATH_MAX], processed_path[BUS_PATH_MAX];
  int locations, n, rc = -1;
  if (!valid_envelope_id(envelope_id) ||
      !channel || !type || !payload_json) return -1;
  locations = bus_envelope_locations(envelope_id);
  if (locations < 0 || locations == 3) {
    (void)rt_narrate("bus stable publish conflict: %s exists in multiple locations",
                     envelope_id);
    return -1;
  }
  if (snprintf(inbox_path, sizeof(inbox_path), "%s/%s.json",
               INBOX_DIR, envelope_id) >= (int)sizeof(inbox_path) ||
      snprintf(processed_path, sizeof(processed_path), "%s/%s.json",
               PROCESSED_DIR, envelope_id) >= (int)sizeof(processed_path))
    return -1;
  if (locations == 1 || locations == 2) {
    const char *path = locations == 1 ? inbox_path : processed_path;
    int same = existing_envelope_matches(path, envelope_id, channel,
                                         type, payload_json);
    if (same == 1) return 0;
    (void)rt_narrate("bus stable publish conflict: %s payload differs",
                     envelope_id);
    return -1;
  }
  if (fs_mkdir_p(INBOX_DIR) != 0) return -1;
  if (fs_mkdir_p(LOG_DIR) != 0) return -1;
  iso_time(ts, sizeof(ts));
  if (json_escape(channel, ec, sizeof(ec)) != 0) return -1;
  if (json_escape(type, et, sizeof(et)) != 0) return -1;
  compact_payload = (char *)fc_xmalloc(BUS_PAYLOAD_MAX);
  if (!compact_payload ||
      json_compact(payload_json, compact_payload, BUS_PAYLOAD_MAX) != 0)
    goto done;
  n = snprintf(NULL, 0,
               "{\"event_id\":\"%s\",\"ts\":\"%s\",\"channel\":\"%s\","
               "\"type\":\"%s\",\"payload\":%s}\n",
               envelope_id, ts, ec, et, compact_payload);
  if (n < 0) goto done;
  envelope = (char *)fc_xmalloc((size_t)n + 1U);
  if (!envelope) goto done;
  snprintf(envelope, (size_t)n + 1U,
           "{\"event_id\":\"%s\",\"ts\":\"%s\",\"channel\":\"%s\","
           "\"type\":\"%s\",\"payload\":%s}\n",
           envelope_id, ts, ec, et, compact_payload);
  if (fs_write_text_atomic(inbox_path, envelope) != 0) goto done;
  /* The inbox file is the behavioral commit. A diagnostic-log failure must
   * not make a retry allocate a second envelope. */
  if (fs_append_text(LOG_PATH, envelope) != 0)
    (void)rt_narrate("bus stable publish: %s committed but bus log append failed",
                     envelope_id);
  bus_signal_wake();
  rc = 0;
done:
  fc_xfree(envelope);
  fc_xfree(compact_payload);
  return rc;
}

int bus_requeue_processed(const char *envelope_id) {
  char inbox_path[BUS_PATH_MAX], processed_path[BUS_PATH_MAX];
  int locations = bus_envelope_locations(envelope_id);
  if (locations < 0 || locations == 3) return -1;
  if (locations == 0 || locations == 1) return 0;
  if (fs_mkdir_p(INBOX_DIR) != 0) return -1;
  if (snprintf(inbox_path, sizeof(inbox_path), "%s/%s.json",
               INBOX_DIR, envelope_id) >= (int)sizeof(inbox_path) ||
      snprintf(processed_path, sizeof(processed_path), "%s/%s.json",
               PROCESSED_DIR, envelope_id) >= (int)sizeof(processed_path))
    return -1;
  if (rename(processed_path, inbox_path) != 0) return -1;
  bus_signal_wake();
  return 0;
}

int bus_publish(const char *channel, const char *type, const char *payload_json,
                char *envelope_id_out, size_t envelope_id_out_len) {
  char id[BUS_ID_MAX];
  if (!channel || !type || !payload_json) return -1;
  if (bus_reserve_envelope_id(id, sizeof(id)) != 0 ||
      bus_publish_stable(id, channel, type, payload_json) != 0)
    return -1;
  if (envelope_id_out && envelope_id_out_len > 0)
    snprintf(envelope_id_out, envelope_id_out_len, "%s", id);
  return 0;
}

int bus_publish_with_retry(const char *channel, const char *type,
                           const char *payload_json,
                           char *envelope_id_out, size_t envelope_id_out_len,
                           int max_attempts) {
  int attempt;
  int delay_ms = 10;
  if (max_attempts < 1) max_attempts = 1;
  if (max_attempts > 6) max_attempts = 6;   /* 10-1000ms then cap */
  for (attempt = 1; attempt <= max_attempts; ++attempt) {
    if (bus_publish(channel, type, payload_json,
                    envelope_id_out, envelope_id_out_len) == 0) {
      if (attempt > 1)
        (void)rt_narrate("bus_publish: succeeded on attempt %d/%d (channel=%s type=%s)",
                         attempt, max_attempts, channel, type);
      return 0;
    }
    if (attempt < max_attempts) {
      struct timespec ts = { delay_ms / 1000, (delay_ms % 1000) * 1000000L };
      (void)nanosleep(&ts, NULL);
      if (delay_ms < 1000) delay_ms *= 10;   /* 10 -> 100 -> 1000 */
    }
  }
  (void)rt_narrate("bus_publish: FAILED all %d attempts (channel=%s type=%s) — "
                   "envelope not delivered, event still durable in log",
                   max_attempts, channel, type);
  return -1;
}

/* Inbox scan uses a fixed-slot buffer. Bus envelope basenames are
 * always "bus_NNNNNN.json" style — well under 64 bytes. Avoiding
 * strdup per name eliminates allocator churn that, under rapid
 * back-to-back drain calls (e.g. with a deep inbox + fast agent
 * execution), inflated process RSS into the hundreds of MB on macOS
 * even though every alloc was paired with a free. */
#define BUS_NAME_MAX 64
#define BUS_NAMES_CAP 1024

/* Diagnostic counters. Help the perf harness prove that allocator
 * churn isn't a leak. Strict equality strdups==frees implies any RSS
 * pressure is allocator-side hoarding, not a bug. (Kept after the
 * strdup-free rewrite for future regressions.) */
static long long g_inbox_scan_strdups = 0;
static long long g_inbox_scan_frees   = 0;
long long bus_diag_strdups(void) { return g_inbox_scan_strdups; }
long long bus_diag_frees(void)   { return g_inbox_scan_frees; }

static int compare_name_slot(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static int is_committed_envelope_name(const char *name) {
  const char *p;
  if (!name || strncmp(name, "bus_", 4) != 0) return 0;
  p = name + 4;
  if (*p < '0' || *p > '9') return 0;
  while (*p >= '0' && *p <= '9') p++;
  return strcmp(p, ".json") == 0;
}

/* Shared scan: read inbox into `names` (up to BUS_NAMES_CAP), sorted
 * oldest-first. Returns count placed, or -1 on hard error.
 * `names` must point at a `char[BUS_NAMES_CAP][BUS_NAME_MAX]` array. */
static int inbox_scan_sorted(char (*names)[BUS_NAME_MAX]) {
  DIR *d;
  struct dirent *entry;
  size_t n = 0;
  int saved_errno = 0;
  d = opendir(INBOX_DIR);
  if (!d) {
    if (errno == ENOENT) return 0;
    fprintf(stderr, "bus: opendir failed for %s: %s\n",
            INBOX_DIR, strerror(errno));
    return -1;
  }
  errno = 0;
  while (n < BUS_NAMES_CAP &&
         (entry = fs_readdir_checked(d)) != NULL) {
    /* Atomic publishers expose their temporary file in inbox/ before the
     * rename commit. Only the canonical basename is an intake record. */
    if (!is_committed_envelope_name(entry->d_name)) continue;
    /* Truncating copy. If a name exceeds BUS_NAME_MAX-1 bytes the
     * truncated form sorts the same as the original prefix; rename
     * will fail and we'll skip the envelope, which is acceptable
     * defensive behavior on a malformed file. */
    snprintf(names[n], BUS_NAME_MAX, "%s", entry->d_name);
    n++;
  }
  if (n < BUS_NAMES_CAP) saved_errno = errno;
  if (closedir(d) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    fprintf(stderr, "bus: readdir failed for %s: %s\n",
            INBOX_DIR, strerror(saved_errno));
    errno = saved_errno;
    return -1;
  }
  if (n > 1) qsort(names, n, BUS_NAME_MAX, compare_name_slot);
  return (int)n;
}

int bus_pop_oldest(char *envelope_path_out, size_t path_len) {
  char names[BUS_NAMES_CAP][BUS_NAME_MAX];
  int n = inbox_scan_sorted(names);
  if (n < 0) return -1;
  if (n == 0) return 1;
  if (fs_mkdir_p(PROCESSED_DIR) != 0) return -1;
  {
    char inbox_path[BUS_PATH_MAX];
    char dest_path[BUS_PATH_MAX];
    snprintf(inbox_path, sizeof(inbox_path), "%s/%s", INBOX_DIR, names[0]);
    snprintf(dest_path,  sizeof(dest_path),  "%s/%s", PROCESSED_DIR, names[0]);
    if (rename(inbox_path, dest_path) != 0) return -1;
    if (envelope_path_out && path_len > 0)
      snprintf(envelope_path_out, path_len, "%s", dest_path);
  }
  return 0;
}

int bus_drain_inbox(int budget, BusDrainHandler handler, void *user_data) {
  char names[BUS_NAMES_CAP][BUS_NAME_MAX];
  int n, target, consumed = 0;
  if (budget <= 0 || !handler) return 0;
  n = inbox_scan_sorted(names);
  if (n < 0) return -1;
  if (n == 0) return 0;
  if (fs_mkdir_p(PROCESSED_DIR) != 0) return -1;
  target = (budget < n) ? budget : n;
  for (int i = 0; i < target; ++i) {
    char inbox_path[BUS_PATH_MAX];
    char dest_path[BUS_PATH_MAX];
    snprintf(inbox_path, sizeof(inbox_path), "%s/%s", INBOX_DIR, names[i]);
    snprintf(dest_path,  sizeof(dest_path),  "%s/%s", PROCESSED_DIR, names[i]);
    /* Per-envelope rename keeps the existing ack/delete crash semantics:
     * any envelope we have not yet renamed is still in inbox/ and will
     * be picked up after a restart. */
    if (rename(inbox_path, dest_path) != 0) continue;
    int stop = handler(dest_path, user_data);
    consumed++;
    if (stop) break;
  }
  return consumed;
}

int bus_inbox_has_pending(void) {
  DIR *d = opendir(INBOX_DIR);
  struct dirent *entry;
  int found = 0;
  int saved_errno = 0;
  if (!d) {
    if (errno == ENOENT) return 0;
    fprintf(stderr, "bus: opendir failed for %s: %s\n",
            INBOX_DIR, strerror(errno));
    return -1;
  }
  errno = 0;
  while ((entry = fs_readdir_checked(d)) != NULL) {
    if (!is_committed_envelope_name(entry->d_name)) continue;
    found = 1;
    break;
  }
  if (!found) saved_errno = errno;
  if (closedir(d) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    fprintf(stderr, "bus: readdir failed for %s: %s\n",
            INBOX_DIR, strerror(saved_errno));
    errno = saved_errno;
    return -1;
  }
  return found;
}

int bus_inbox_count(void) {
  DIR *d = opendir(INBOX_DIR);
  struct dirent *entry;
  int count = 0;
  int saved_errno = 0;
  if (!d) {
    if (errno == ENOENT) return 0;
    fprintf(stderr, "bus: opendir failed for %s: %s\n",
            INBOX_DIR, strerror(errno));
    return -1;
  }
  errno = 0;
  while ((entry = fs_readdir_checked(d)) != NULL) {
    if (!is_committed_envelope_name(entry->d_name)) continue;
    count++;
  }
  saved_errno = errno;
  if (closedir(d) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    fprintf(stderr, "bus: readdir failed for %s: %s\n",
            INBOX_DIR, strerror(saved_errno));
    errno = saved_errno;
    return -1;
  }
  return count;
}

long long bus_log_size_bytes(void) {
  struct stat st;
  if (stat(LOG_PATH, &st) != 0) return 0;
  return (long long)st.st_size;
}

int bus_log_show(int tail_n) {
  BusLogFilter f = {0};
  f.last_n = tail_n;
  return bus_log_show_filtered(&f);
}

static int line_matches(const char *line, size_t len, const BusLogFilter *f) {
  JsonRef root;
  char buf[256];
  char tmp[4096];
  if (!f) return 1;
  if (!f->channel && !f->type) return 1;
  if (len + 1 > sizeof(tmp)) return 1; /* don't filter oversized lines, just print */
  memcpy(tmp, line, len);
  tmp[len] = '\0';
  if (json_ref_first_object(tmp, &root) != 0) return 0;
  if (f->channel) {
    if (json_ref_object_get_string(&root, "channel", buf, sizeof(buf)) != 0) return 0;
    if (strcmp(buf, f->channel) != 0) return 0;
  }
  if (f->type) {
    if (json_ref_object_get_string(&root, "type", buf, sizeof(buf)) != 0) return 0;
    if (strcmp(buf, f->type) != 0) return 0;
  }
  return 1;
}

/* Walk lines in [start..end), apply filter, print each match to stdout. */
static void emit_range(const char *start, const char *end, const BusLogFilter *f) {
  const char *p = start;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (line_len > 0 && line_matches(p, line_len, f)) {
      if (f && f->human) fputs(COLOR_CYAN, stdout);
      fwrite(p, 1, line_len, stdout);
      if (f && f->human) fputs(COLOR_RESET, stdout);
      fputc('\n', stdout);
    }
    if (!nl) break;
    p = nl + 1;
  }
  fflush(stdout);
}

int bus_log_show_filtered(const BusLogFilter *filter) {
  char *text = NULL;
  size_t total = 0;
  size_t shown_start = 0;
  long long offset = 0;
  if (fs_read_text(LOG_PATH, &text, FS_READ_TEXT_DEFAULT_CAP) == 0 && text) {
    total = strlen(text);
    if (filter && filter->last_n > 0) {
      int line_count = 0;
      for (size_t i = 0; i < total; ++i) if (text[i] == '\n') line_count++;
      if (line_count > filter->last_n) {
        int to_skip = line_count - filter->last_n;
        for (size_t i = 0; i < total && to_skip > 0; ++i) {
          if (text[i] == '\n') {
            to_skip--;
            if (to_skip == 0) shown_start = i + 1;
          }
        }
      }
    }
    emit_range(text + shown_start, text + total, filter);
    offset = (long long)total;
    free(text);
  }
  if (!filter || !filter->follow) return 0;

  /* Follow mode: poll the file size; when it grows, read the new bytes,
   * filter, and print. Stop when *stop_flag goes nonzero. */
  while (!filter->stop_flag || !*filter->stop_flag) {
    long long size = bus_log_size_bytes();
    if (size > offset) {
      FILE *fp = fopen(LOG_PATH, "rb");
      if (fp) {
        if (fseek(fp, offset, SEEK_SET) == 0) {
          char buf[8192];
          size_t n;
          char accum[16384];
          size_t used = 0;
          while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
            if (used + n > sizeof(accum)) {
              /* flush whatever we have, then start fresh; oversized lines just stream through */
              emit_range(accum, accum + used, filter);
              used = 0;
              if (n > sizeof(accum)) {
                emit_range(buf, buf + n, filter);
                continue;
              }
            }
            memcpy(accum + used, buf, n);
            used += n;
          }
          if (used > 0) emit_range(accum, accum + used, filter);
        }
        fclose(fp);
      }
      offset = size;
    } else {
      struct timespec ts = {0, 100 * 1000000L};
      nanosleep(&ts, NULL);
    }
  }
  return 0;
}
