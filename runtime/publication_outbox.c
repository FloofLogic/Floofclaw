/* Durable publication outbox.
 *
 * A controller wake is never the source of truth. Its source action terminal,
 * operation_result, or terminal work event is. The producer first atomically
 * prepares a self-contained record with a reserved bus identity, then appends
 * that source event synchronously. Reconciliation verifies the exact committed
 * source id/type/payload before publishing the reserved identity.
 *
 * Records survive publication through the claimed result run. Therefore a
 * crash after inbox->processed rename but before runstate persist requeues the
 * same identity, while a pending/active exact claim is never duplicated and
 * its source evidence stays pinned until the claim becomes terminal. */

#include "publication_outbox.h"
#include "run_state.h"

#include "bus/bus.h"
#include "support/fsutil.h"
#include "support/heap_guard.h"
#include "support/json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PUBLICATION_OUTBOX_DIR "workspace/bus/outbox"
#define PUBLICATION_NAME_MAX 64
#define PUBLICATION_SCAN_CAP 1024
#define PUBLICATION_WAKE_PAYLOAD_CAP BUS_PAYLOAD_MAX

typedef struct {
  char wake_id[BUS_ID_MAX];
  char source_run_id[RT_SMALL];
  char source_event_id[RT_SMALL];
  char source_type[RT_SMALL];
  char channel[BUS_CHANNEL_MAX];
  char wake_type[BUS_TYPE_MAX];
  char *source_payload;
  char *wake_payload;
} PublicationRecord;

typedef enum {
  SOURCE_IO = -2,
  SOURCE_CONFLICT = -1,
  SOURCE_ABSENT = 0,
  SOURCE_EXACT = 1
} SourceState;

typedef enum {
  CLAIM_IO = -2,
  CLAIM_CONFLICT = -1,
  CLAIM_NONE = 0,
  CLAIM_PENDING = 1,
  CLAIM_ACTIVE = 2,
  CLAIM_TERMINAL = 3
} ClaimState;

typedef enum {
  CLAIM_INBOUND_IO = -2,
  CLAIM_INBOUND_CONFLICT = -1,
  CLAIM_INBOUND_PENDING = 0,
  CLAIM_INBOUND_EXACT = 1
} ClaimInboundState;

static void record_clear(PublicationRecord *record);
static int record_load(const char *path, PublicationRecord *out);

static int safe_id(const char *s, size_t max_len) {
  size_t n;
  if (!s || !*s) return 0;
  n = strlen(s);
  if (n >= max_len) return 0;
  for (size_t i = 0; i < n; ++i)
    if (!((s[i] >= 'a' && s[i] <= 'z') ||
          (s[i] >= 'A' && s[i] <= 'Z') ||
          (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
      return 0;
  return 1;
}

static int outbox_path(const char *wake_id, char *out, size_t out_len) {
  int n;
  if (!safe_id(wake_id, BUS_ID_MAX)) return -1;
  n = snprintf(out, out_len, "%s/%s.json",
               PUBLICATION_OUTBOX_DIR, wake_id);
  return n < 0 || (size_t)n >= out_len ? -1 : 0;
}

static int canonical_record_name(const char *name) {
  char id[BUS_ID_MAX];
  const char *p;
  size_t len;
  if (!name) return 0;
  len = strlen(name);
  if (len <= 5U || len - 5U >= sizeof(id) ||
      strcmp(name + len - 5U, ".json") != 0)
    return 0;
  memcpy(id, name, len - 5U);
  id[len - 5U] = '\0';
  if (!safe_id(id, sizeof(id)) || strncmp(id, "bus_", 4) != 0)
    return 0;
  p = id + 4;
  if (*p < '0' || *p > '9') return 0;
  while (*p >= '0' && *p <= '9') p++;
  return *p == '\0';
}

static int outbox_record_count(size_t *out_count) {
  DIR *dir;
  struct dirent *entry;
  size_t count = 0;
  int saved_errno = 0;
  if (!out_count) return -1;
  *out_count = 0;
  dir = opendir(PUBLICATION_OUTBOX_DIR);
  if (!dir) return errno == ENOENT ? 0 : -1;
  errno = 0;
  while ((entry = fs_readdir_checked(dir)) != NULL)
    if (canonical_record_name(entry->d_name)) count++;
  saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    return -1;
  }
  *out_count = count;
  return 0;
}

static int compact_alloc(const char *json, char **out) {
  size_t cap;
  char *buf;
  if (!json || !out) return -1;
  cap = strlen(json) + 1U;
  buf = (char *)fc_xmalloc(cap);
  if (!buf) return -1;
  if (json_compact(json, buf, cap) != 0) {
    fc_xfree(buf);
    return -1;
  }
  *out = buf;
  return 0;
}

/* 1 exact prepared record reused, 0 no record, -1 conflicting/error. */
static int prepared_source_lookup(const RtContext *ctx,
                                  const char *source_event_id,
                                  const char *source_type,
                                  const char *source_payload,
                                  const char *channel,
                                  const char *wake_type,
                                  const char *wake_payload,
                                  char *wake_id_out,
                                  size_t wake_id_len) {
  DIR *dir = opendir(PUBLICATION_OUTBOX_DIR);
  struct dirent *entry;
  int saved_errno = 0, result = 0;
  if (!dir) return errno == ENOENT ? 0 : -1;
  errno = 0;
  while ((entry = fs_readdir_checked(dir)) != NULL) {
    char path[PATH_MAX];
    PublicationRecord record;
    memset(&record, 0, sizeof(record));
    if (!canonical_record_name(entry->d_name) ||
        snprintf(path, sizeof(path), "%s/%s", PUBLICATION_OUTBOX_DIR,
                 entry->d_name) >= (int)sizeof(path))
      continue;
    if (record_load(path, &record) != 0) {
      saved_errno = EINVAL;
      break;
    }
    if (strcmp(record.source_run_id, ctx->run_id) == 0 &&
        strcmp(record.source_event_id, source_event_id) == 0) {
      if (strcmp(record.source_type, source_type) != 0 ||
          strcmp(record.source_payload, source_payload) != 0 ||
          strcmp(record.channel, channel) != 0 ||
          strcmp(record.wake_type, wake_type) != 0 ||
          strcmp(record.wake_payload, wake_payload) != 0) {
        record_clear(&record);
        saved_errno = EEXIST;
        break;
      }
      if (wake_id_out && wake_id_len)
        snprintf(wake_id_out, wake_id_len, "%s", record.wake_id);
      record_clear(&record);
      result = 1;
      break;
    }
    record_clear(&record);
  }
  if (result == 0 && saved_errno == 0) saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    return -1;
  }
  return result;
}

int rt_publication_outbox_prepare(const RtContext *ctx,
                                  const char *source_event_id,
                                  const char *source_type,
                                  const char *source_payload_json,
                                  const char *channel,
                                  const char *wake_type,
                                  const char *wake_payload_json,
                                  char *wake_id_out,
                                  size_t wake_id_len) {
  char wake_id[BUS_ID_MAX], path[PATH_MAX];
  char erun[RT_MED], esource_id[RT_MED], esource_type[RT_MED];
  char echannel[BUS_CHANNEL_MAX * 2], ewake_type[BUS_TYPE_MAX * 2];
  char *source_payload = NULL, *wake_payload = NULL, *record = NULL;
  int n, rc = -1;
  if (wake_id_out && wake_id_len) wake_id_out[0] = '\0';
  if (!ctx || !safe_id(ctx->run_id, sizeof(ctx->run_id)) ||
      !safe_id(source_event_id, RT_SMALL) || !source_type || !*source_type ||
      !source_payload_json || !channel || !*channel ||
      !wake_type || !*wake_type || !wake_payload_json)
    return -1;
  if (compact_alloc(source_payload_json, &source_payload) != 0 ||
      compact_alloc(wake_payload_json, &wake_payload) != 0 ||
      json_escape(ctx->run_id, erun, sizeof(erun)) != 0 ||
      json_escape(source_event_id, esource_id, sizeof(esource_id)) != 0 ||
      json_escape(source_type, esource_type, sizeof(esource_type)) != 0 ||
      json_escape(channel, echannel, sizeof(echannel)) != 0 ||
      json_escape(wake_type, ewake_type, sizeof(ewake_type)) != 0)
    goto done;
  /* bus_publish_stable owns a fixed BUS_PAYLOAD_MAX canonical boundary.
   * Reject before source truth is appended so reconciliation can never
   * retain an intrinsically unpublishable record. */
  if (strlen(wake_payload) >= PUBLICATION_WAKE_PAYLOAD_CAP)
    goto done;
  {
    int existing = prepared_source_lookup(
        ctx, source_event_id, source_type, source_payload,
        channel, wake_type, wake_payload, wake_id_out, wake_id_len);
    if (existing < 0) goto done;
    if (existing == 1) {
      rc = 0;
      goto done;
    }
  }
  {
    size_t record_count = 0;
    if (outbox_record_count(&record_count) != 0) goto done;
    if (record_count >= PUBLICATION_SCAN_CAP) {
      (void)rt_narrate(
          "publication outbox full: cap=%d; finish or repair pending wakes",
          PUBLICATION_SCAN_CAP);
      errno = ENOSPC;
      goto done;
    }
  }
  if (bus_reserve_envelope_id(wake_id, sizeof(wake_id)) != 0 ||
      outbox_path(wake_id, path, sizeof(path)) != 0)
    goto done;
  n = snprintf(NULL, 0,
               "{\"wake_id\":\"%s\",\"source_run_id\":\"%s\","
               "\"source_event_id\":\"%s\",\"source_type\":\"%s\","
               "\"source_payload\":%s,\"channel\":\"%s\","
               "\"wake_type\":\"%s\",\"wake_payload\":%s}\n",
               wake_id, erun, esource_id, esource_type, source_payload,
               echannel, ewake_type, wake_payload);
  if (n < 0) goto done;
  record = (char *)fc_xmalloc((size_t)n + 1U);
  if (!record) goto done;
  snprintf(record, (size_t)n + 1U,
           "{\"wake_id\":\"%s\",\"source_run_id\":\"%s\","
           "\"source_event_id\":\"%s\",\"source_type\":\"%s\","
           "\"source_payload\":%s,\"channel\":\"%s\","
           "\"wake_type\":\"%s\",\"wake_payload\":%s}\n",
           wake_id, erun, esource_id, esource_type, source_payload,
           echannel, ewake_type, wake_payload);
  if (fs_mkdir_p(PUBLICATION_OUTBOX_DIR) != 0 ||
      fs_write_text_atomic(path, record) != 0)
    goto done;
  if (wake_id_out && wake_id_len)
    snprintf(wake_id_out, wake_id_len, "%s", wake_id);
  rc = 0;
done:
  fc_xfree(record);
  fc_xfree(source_payload);
  fc_xfree(wake_payload);
  return rc;
}

static void record_clear(PublicationRecord *record) {
  if (!record) return;
  fc_xfree(record->source_payload);
  fc_xfree(record->wake_payload);
  memset(record, 0, sizeof(*record));
}

static int record_load(const char *path, PublicationRecord *out) {
  char *text = NULL;
  char expected_name[BUS_ID_MAX + 6];
  const char *basename;
  JsonRef root, source_payload, wake_payload;
  memset(out, 0, sizeof(*out));
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  if (json_ref_top_object(text, &root) != 0 ||
      json_ref_object_get_string(&root, "wake_id", out->wake_id,
                                 sizeof(out->wake_id)) != 0 ||
      json_ref_object_get_string(&root, "source_run_id",
                                 out->source_run_id,
                                 sizeof(out->source_run_id)) != 0 ||
      json_ref_object_get_string(&root, "source_event_id",
                                 out->source_event_id,
                                 sizeof(out->source_event_id)) != 0 ||
      json_ref_object_get_string(&root, "source_type", out->source_type,
                                 sizeof(out->source_type)) != 0 ||
      json_ref_object_get_string(&root, "channel", out->channel,
                                 sizeof(out->channel)) != 0 ||
      json_ref_object_get_string(&root, "wake_type", out->wake_type,
                                 sizeof(out->wake_type)) != 0 ||
      json_ref_object_get(&root, "source_payload", &source_payload) != 0 ||
      json_ref_object_get(&root, "wake_payload", &wake_payload) != 0 ||
      !(out->source_payload = json_ref_value_compact_dup(&source_payload)) ||
      !(out->wake_payload = json_ref_value_compact_dup(&wake_payload)) ||
      !safe_id(out->wake_id, sizeof(out->wake_id)) ||
      !safe_id(out->source_run_id, sizeof(out->source_run_id)) ||
      !safe_id(out->source_event_id, sizeof(out->source_event_id))) {
    fc_xfree(text);
    record_clear(out);
    return -1;
  }
  basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (snprintf(expected_name, sizeof(expected_name), "%s.json",
               out->wake_id) >= (int)sizeof(expected_name) ||
      strcmp(basename, expected_name) != 0) {
    fc_xfree(text);
    record_clear(out);
    return -1;
  }
  fc_xfree(text);
  return 0;
}

/* A publication source is usable only when exactly one complete event line
 * owns the reserved id and its full envelope agrees with the outbox record.
 * Conflicting durable truth is distinct from retryable filesystem I/O. */
static SourceState source_state(const PublicationRecord *record) {
  char path[PATH_MAX], *text = NULL, *line;
  int matches = 0;
  SourceState result = SOURCE_ABSENT;
  if (snprintf(path, sizeof(path), "workspace/runs/%s/event_log.jsonl",
               record->source_run_id) >= (int)sizeof(path))
    return SOURCE_CONFLICT;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? SOURCE_ABSENT : SOURCE_IO;
  line = text;
  while (*line) {
    char *next = strchr(line, '\n');
    JsonRef event, payload;
    char event_id[RT_SMALL] = "", type[RT_SMALL] = "";
    char run_id[RT_SMALL] = "";
    char *payload_compact = NULL;
    /* The newline is the append commit marker. Never bless a
     * complete-looking torn tail that recovery will truncate. */
    if (!next) break;
    *next = '\0';
    if (json_ref_top_object(line, &event) != 0 ||
        json_ref_object_get_string(&event, "event_id", event_id,
                                   sizeof(event_id)) != 0) {
      result = SOURCE_CONFLICT;
      break;
    }
    if (strcmp(event_id, record->source_event_id) == 0) {
      matches++;
      if (matches > 1 ||
          json_ref_object_get_string(&event, "run_id", run_id,
                                     sizeof(run_id)) != 0 ||
          json_ref_object_get_string(&event, "type", type,
                                     sizeof(type)) != 0 ||
          json_ref_object_get(&event, "payload", &payload) != 0 ||
          !(payload_compact = json_ref_value_compact_dup(&payload)) ||
          strcmp(run_id, record->source_run_id) != 0 ||
          strcmp(type, record->source_type) != 0 ||
          strcmp(payload_compact, record->source_payload) != 0) {
        fc_xfree(payload_compact);
        result = SOURCE_CONFLICT;
        break;
      }
      fc_xfree(payload_compact);
      result = SOURCE_EXACT;
    }
    line = next + 1;
  }
  fc_xfree(text);
  if (result == SOURCE_EXACT && matches == 1) {
    int fd = open(path, O_RDWR);
    if (fd < 0 || fsync(fd) != 0) {
      if (fd >= 0) close(fd);
      return SOURCE_IO;
    }
    if (close(fd) != 0) return SOURCE_IO;
  }
  return result;
}

static int apply_exact_source_to_work_state(
    const PublicationRecord *record) {
  RtContext ctx;
  JsonRef payload;
  char context_id[RT_MED] = "";
  if (!record || !rt_work_state_is_event_type(record->source_type) ||
      json_ref_top_object(record->source_payload, &payload) != 0 ||
      json_ref_object_get_string(&payload, "context_id", context_id,
                                 sizeof(context_id)) != 0 ||
      !context_id[0])
    return -1;
  memset(&ctx, 0, sizeof(ctx));
  snprintf(ctx.run_id, sizeof(ctx.run_id), "%s", record->source_run_id);
  snprintf(ctx.context_id, sizeof(ctx.context_id), "%s", context_id);
  return rt_work_state_apply_event(&ctx, record->source_type,
                                   record->source_payload,
                                   record->source_event_id);
}

static ClaimInboundState claimed_inbound_state(
    const char *run_name, const PublicationRecord *record) {
  char path[PATH_MAX], expected_event_id[RT_SMALL];
  char *text = NULL, *next;
  JsonRef event, payload;
  char event_id[RT_SMALL] = "", run_id[RT_SMALL] = "";
  char type[RT_SMALL] = "", source[BUS_CHANNEL_MAX] = "";
  char *compact = NULL;
  ClaimInboundState result = CLAIM_INBOUND_CONFLICT;
  if (snprintf(path, sizeof(path), "workspace/runs/%s/event_log.jsonl",
               run_name) >= (int)sizeof(path) ||
      snprintf(expected_event_id, sizeof(expected_event_id),
               "evt_%s_%06d", run_name, 1) >=
          (int)sizeof(expected_event_id))
    return CLAIM_INBOUND_CONFLICT;
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? CLAIM_INBOUND_PENDING : CLAIM_INBOUND_IO;
  next = strchr(text, '\n');
  if (!next) {
    fc_xfree(text);
    return CLAIM_INBOUND_PENDING;
  }
  *next = '\0';
  if (json_ref_top_object(text, &event) == 0 &&
      json_ref_object_get_string(&event, "event_id", event_id,
                                 sizeof(event_id)) == 0 &&
      json_ref_object_get_string(&event, "run_id", run_id,
                                 sizeof(run_id)) == 0 &&
      json_ref_object_get_string(&event, "type", type,
                                 sizeof(type)) == 0 &&
      json_ref_object_get_string(&event, "source", source,
                                 sizeof(source)) == 0 &&
      json_ref_object_get(&event, "payload", &payload) == 0 &&
      (compact = json_ref_value_compact_dup(&payload)) != NULL &&
      strcmp(event_id, expected_event_id) == 0 &&
      strcmp(run_id, run_name) == 0 &&
      strcmp(type, record->wake_type) == 0 &&
      strcmp(source, record->channel) == 0 &&
      strcmp(compact, record->wake_payload) == 0)
    result = CLAIM_INBOUND_EXACT;
  fc_xfree(compact);
  fc_xfree(text);
  return result;
}

static ClaimState run_claim_state(const PublicationRecord *record) {
  DIR *dir = opendir("workspace/runs");
  struct dirent *entry;
  ClaimState result = CLAIM_NONE;
  int matches = 0, saved_errno = 0;
  if (!dir) return errno == ENOENT ? CLAIM_NONE : CLAIM_IO;
  errno = 0;
  while ((entry = fs_readdir_checked(dir)) != NULL) {
    char path[PATH_MAX], *text = NULL;
    JsonRef root, created;
    char event_id[BUS_ID_MAX] = "";
    char created_type[BUS_TYPE_MAX] = "";
    char run_id[RT_SMALL] = "", status[RT_SMALL] = "";
    ClaimInboundState inbound;
    int run_number = 0;
    if (sscanf(entry->d_name, "run_%d", &run_number) != 1 ||
        run_number < 1 ||
        snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json",
                 entry->d_name) >= (int)sizeof(path))
      continue;
    if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) {
      if (errno == ENOENT) continue;
      result = CLAIM_IO;
      break;
    }
    if (json_ref_top_object(text, &root) != 0 ||
        json_ref_object_get_string(&root, "run_id", run_id,
                                   sizeof(run_id)) != 0 ||
        json_ref_object_get_string(&root, "status", status,
                                   sizeof(status)) != 0 ||
        json_ref_object_get_object(&root, "created_from", &created) != 0 ||
        json_ref_object_get_string(&created, "event_id", event_id,
                                   sizeof(event_id)) != 0 ||
        json_ref_object_get_string(&created, "type", created_type,
                                   sizeof(created_type)) != 0) {
      fc_xfree(text);
      result = CLAIM_CONFLICT;
      break;
    }
    fc_xfree(text);
    if (strcmp(event_id, record->wake_id) != 0)
      continue;
    matches++;
    if (matches > 1 ||
        strcmp(run_id, entry->d_name) != 0 ||
        strcmp(created_type, record->wake_type) != 0 ||
        !rt_run_status_is_valid(status)) {
      result = CLAIM_CONFLICT;
      break;
    }
    inbound = claimed_inbound_state(entry->d_name, record);
    if (inbound == CLAIM_INBOUND_IO) {
      result = CLAIM_IO;
      break;
    }
    if (inbound != CLAIM_INBOUND_EXACT) {
      result = inbound == CLAIM_INBOUND_PENDING
                   ? CLAIM_PENDING : CLAIM_CONFLICT;
      continue;
    }
    result = rt_run_status_is_terminal(status)
                 ? CLAIM_TERMINAL
                 : CLAIM_ACTIVE;
  }
  if (result >= CLAIM_NONE) saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    return CLAIM_IO;
  }
  return result;
}

static int compare_name(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

int rt_publication_outbox_reconcile(void) {
  char names[PUBLICATION_SCAN_CAP][PUBLICATION_NAME_MAX];
  DIR *dir;
  struct dirent *entry;
  size_t count = 0;
  int saved_errno = 0, failed = 0, overflow = 0;
  dir = opendir(PUBLICATION_OUTBOX_DIR);
  if (!dir) return errno == ENOENT ? 0 : -1;
  errno = 0;
  while ((entry = fs_readdir_checked(dir)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (!canonical_record_name(entry->d_name))
      continue;
    if (len >= PUBLICATION_NAME_MAX) {
      failed = 1;
      continue;
    }
    if (count >= PUBLICATION_SCAN_CAP) {
      overflow = 1;
      continue;
    }
    snprintf(names[count++], sizeof(names[0]), "%s", entry->d_name);
  }
  saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    return -1;
  }
  if (overflow)
    (void)rt_narrate(
        "publication outbox exceeds reconcile batch cap=%d; processing one batch",
        PUBLICATION_SCAN_CAP);
  if (count > 1)
    qsort(names, count, sizeof(names[0]), compare_name);
  /* Scheduler startup rebuilds the ledger before its first reconciliation.
   * Live producers synchronously apply source truth to the ledger before
   * calling here. Do not reread every retained run on this hot path. */
  for (size_t i = 0; i < count; ++i) {
    char path[PATH_MAX];
    PublicationRecord record;
    SourceState source;
    ClaimState claim;
    int locations;
    if (snprintf(path, sizeof(path), "%s/%s",
                 PUBLICATION_OUTBOX_DIR, names[i]) >= (int)sizeof(path) ||
        record_load(path, &record) != 0) {
      failed = 1;
      continue;
    }
    source = source_state(&record);
    if (source == SOURCE_CONFLICT) {
      (void)rt_narrate("publication outbox conflict: source %s is not exact",
                       record.source_event_id);
      failed = 1;
      record_clear(&record);
      continue;
    }
    if (source == SOURCE_IO) {
      failed = 1;
      record_clear(&record);
      continue;
    }
    if (source == SOURCE_ABSENT) {
      record_clear(&record);
      continue;
    }
    /* A full source line can survive a producer-side fsync error that
     * prevented the ordinary reducers from running. Reapply only this exact
     * committed source before publication; startup's full rebuild covers
     * crash recovery, while this keeps live reconciliation O(outbox). */
    if (apply_exact_source_to_work_state(&record) != 0) {
      failed = 1;
      record_clear(&record);
      continue;
    }
    claim = run_claim_state(&record);
    if (claim == CLAIM_CONFLICT) {
      (void)rt_narrate(
          "publication outbox conflict: wake %s has an invalid or duplicate run claim",
          record.wake_id);
      failed = 1;
      record_clear(&record);
      continue;
    }
    if (claim == CLAIM_IO) {
      failed = 1;
      record_clear(&record);
      continue;
    }
    if (claim == CLAIM_TERMINAL) {
      if (unlink(path) != 0 && errno != ENOENT) failed = 1;
      record_clear(&record);
      continue;
    }
    if (claim == CLAIM_PENDING || claim == CLAIM_ACTIVE) {
      /* The processed envelope already belongs to one exact run. A pending
       * first append is retried by ports under that same run id; an active
       * result run retains this record (and therefore its evidence pin) until
       * its terminal runstate is durable. */
      record_clear(&record);
      continue;
    }
    locations = bus_envelope_locations(record.wake_id);
    if (locations == 1 || locations == 2) {
      /* Existing files are not proof of this publication. Verify their
       * complete identity and payload before leaving or requeueing them. */
      if (bus_publish_stable(record.wake_id, record.channel,
                             record.wake_type,
                             record.wake_payload) != 0) {
        failed = 1;
      } else if (locations == 2 &&
                 bus_requeue_processed(record.wake_id) != 0) {
        failed = 1;
      }
    } else if (locations == 0) {
      if (bus_publish_stable(record.wake_id, record.channel,
                             record.wake_type,
                             record.wake_payload) != 0)
        failed = 1;
    } else if (locations < 0 || locations == 3) {
      (void)rt_narrate("publication outbox conflict: wake %s has invalid location",
                       record.wake_id);
      failed = 1;
    }
    record_clear(&record);
  }
  return failed ? -1 : 0;
}

int rt_publication_outbox_validate(const char *wake_id,
                                   const char *channel,
                                   const char *wake_type,
                                   const char *wake_payload_json) {
  char path[PATH_MAX];
  PublicationRecord record;
  char *compact = NULL;
  int result = -1;
  memset(&record, 0, sizeof(record));
  if (outbox_path(wake_id, path, sizeof(path)) != 0 ||
      !channel || !*channel || !wake_type || !*wake_type ||
      !wake_payload_json)
    return -1;
  if (!fs_file_exists(path)) return 0;
  if (record_load(path, &record) != 0 ||
      compact_alloc(wake_payload_json, &compact) != 0)
    goto done;
  if (strcmp(record.wake_id, wake_id) != 0 ||
      strcmp(record.channel, channel) != 0 ||
      strcmp(record.wake_type, wake_type) != 0 ||
      strcmp(record.wake_payload, compact) != 0 ||
      source_state(&record) != SOURCE_EXACT)
    goto done;
  result = 1;
done:
  fc_xfree(compact);
  record_clear(&record);
  return result;
}

int rt_publication_outbox_source_pending(const char *source_event_id) {
  DIR *dir;
  struct dirent *entry;
  int saved_errno = 0, found = 0;
  if (!safe_id(source_event_id, RT_SMALL)) return -1;
  dir = opendir(PUBLICATION_OUTBOX_DIR);
  if (!dir) return errno == ENOENT ? 0 : -1;
  errno = 0;
  while ((entry = fs_readdir_checked(dir)) != NULL) {
    char path[PATH_MAX];
    PublicationRecord record;
    memset(&record, 0, sizeof(record));
    if (!canonical_record_name(entry->d_name) ||
        snprintf(path, sizeof(path), "%s/%s", PUBLICATION_OUTBOX_DIR,
                 entry->d_name) >= (int)sizeof(path))
      continue;
    if (record_load(path, &record) != 0) {
      saved_errno = EINVAL;
      break;
    }
    if (strcmp(record.source_event_id, source_event_id) == 0)
      found = 1;
    record_clear(&record);
    if (found) break;
  }
  if (!found && saved_errno == 0) saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    return -1;
  }
  return found;
}

static int claimed_wake_for_run(const char *run_id,
                                char *wake_id, size_t wake_id_len) {
  char path[PATH_MAX], *text = NULL;
  char stored_run_id[RT_SMALL] = "";
  JsonRef root, created;
  int result = -1;
  if (!run_id || !*run_id || !wake_id || wake_id_len == 0 ||
      snprintf(path, sizeof(path), "workspace/runs/%s/runstate.json",
               run_id) >= (int)sizeof(path))
    return -1;
  wake_id[0] = '\0';
  if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return errno == ENOENT ? 0 : -1;
  if (json_ref_top_object(text, &root) == 0 &&
      json_ref_object_get_string(&root, "run_id", stored_run_id,
                                 sizeof(stored_run_id)) == 0 &&
      strcmp(stored_run_id, run_id) == 0 &&
      json_ref_object_get_object(&root, "created_from", &created) == 0 &&
      json_ref_object_get_string(&created, "event_id",
                                 wake_id, wake_id_len) == 0)
    result = wake_id[0] ? 1 : 0;
  fc_xfree(text);
  return result;
}

int rt_publication_outbox_run_pending(const char *run_id) {
  DIR *dir;
  struct dirent *entry;
  char claimed_wake[BUS_ID_MAX] = "";
  int claimed;
  int saved_errno = 0, found = 0;
  if (!safe_id(run_id, RT_SMALL)) return -1;
  dir = opendir(PUBLICATION_OUTBOX_DIR);
  if (!dir) return errno == ENOENT ? 0 : -1;
  claimed = claimed_wake_for_run(run_id, claimed_wake,
                                 sizeof(claimed_wake));
  /* With live publication records, an unreadable claim is not permission
   * for retention to discard a possibly claimed result run. */
  if (claimed < 0) {
    (void)closedir(dir);
    return -1;
  }
  errno = 0;
  while ((entry = fs_readdir_checked(dir)) != NULL) {
    char path[PATH_MAX];
    PublicationRecord record;
    memset(&record, 0, sizeof(record));
    if (!canonical_record_name(entry->d_name) ||
        snprintf(path, sizeof(path), "%s/%s", PUBLICATION_OUTBOX_DIR,
                 entry->d_name) >= (int)sizeof(path))
      continue;
    if (record_load(path, &record) != 0) {
      saved_errno = EINVAL;
      break;
    }
    if (strcmp(record.source_run_id, run_id) == 0 ||
        (claimed == 1 &&
         strcmp(record.wake_id, claimed_wake) == 0))
      found = 1;
    record_clear(&record);
    if (found) break;
  }
  if (!found && saved_errno == 0) saved_errno = errno;
  if (closedir(dir) != 0 && saved_errno == 0) saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    return -1;
  }
  return found;
}

int rt_publication_outbox_wake_pending(const char *wake_id) {
  char path[PATH_MAX];
  PublicationRecord record;
  int result;
  memset(&record, 0, sizeof(record));
  if (outbox_path(wake_id, path, sizeof(path)) != 0) return -1;
  if (!fs_file_exists(path)) return 0;
  result = record_load(path, &record) == 0 ? 1 : -1;
  record_clear(&record);
  return result;
}
