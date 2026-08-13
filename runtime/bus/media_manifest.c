#include "media_manifest.h"

#include "../support/heap_guard.h"
#include "../support/json.h"
#include "../support/sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#define MEDIA_ROOT          "workspace/media"
#define MEDIA_INBOUND_DIR   MEDIA_ROOT "/inbound"
#define MEDIA_FILE_MAX      (256U * 1024U)

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} MediaJson;

static void set_error(char *err, size_t err_cap, const char *fmt, ...) {
  va_list ap;
  if (!err || err_cap == 0U) return;
  va_start(ap, fmt);
  (void)vsnprintf(err, err_cap, fmt, ap);
  va_end(ap);
  err[err_cap - 1U] = '\0';
}

static int add_size(size_t *value, size_t add) {
  if (!value || add > SIZE_MAX - *value) return -1;
  *value += add;
  return 0;
}

static int add_escaped_bound(size_t *value, const char *text) {
  size_t len;
  if (!value || !text) return -1;
  len = strlen(text);
  if (len > (SIZE_MAX - *value) / 6U) return -1;
  *value += len * 6U;
  return 0;
}

static int media_json_puts(MediaJson *json, const char *text) {
  size_t len;
  if (!json || !text) return -1;
  len = strlen(text);
  if (len >= json->cap - json->len) return -1;
  memcpy(json->data + json->len, text, len);
  json->len += len;
  json->data[json->len] = '\0';
  return 0;
}

static int media_json_escape(MediaJson *json, const char *text) {
  size_t len;
  if (!json || !text || json->len >= json->cap ||
      json_escape(text, json->data + json->len,
                  json->cap - json->len) != 0)
    return -1;
  len = strlen(json->data + json->len);
  json->len += len;
  return 0;
}

static int media_json_u64(MediaJson *json, uint64_t value) {
  char number[32];
  if (snprintf(number, sizeof(number), "%" PRIu64, value) >=
      (int)sizeof(number))
    return -1;
  return media_json_puts(json, number);
}

static int ascii_alnum(unsigned char ch) {
  return (ch >= 'a' && ch <= 'z') ||
         (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9');
}

static int valid_id(const char *id) {
  size_t len;
  if (!id || !(len = strlen(id)) || len >= FC_MEDIA_ID_MAX) return 0;
  for (size_t i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)id[i];
    if (!ascii_alnum(ch) &&
        ch != '-' && ch != '_' && ch != '.' && ch != ':')
      return 0;
  }
  return 1;
}

static int valid_filename(const char *filename) {
  size_t len;
  if (!filename || !(len = strlen(filename)) ||
      len >= FC_MEDIA_FILENAME_MAX)
    return 0;
  for (size_t i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)filename[i];
    if (ch < 32U || ch == 127U) return 0;
  }
  return 1;
}

static int mime_token_char(unsigned char ch) {
  if (ascii_alnum(ch)) return 1;
  return strchr("!#$%&'*+-.^_`|~", (int)ch) != NULL;
}

static int valid_media_type(const char *media_type) {
  size_t len;
  size_t slash = SIZE_MAX;
  if (!media_type || !(len = strlen(media_type)) ||
      len >= FC_MEDIA_TYPE_MAX)
    return 0;
  for (size_t i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)media_type[i];
    if (ch == '/') {
      if (slash != SIZE_MAX) return 0;
      slash = i;
    } else if (!mime_token_char(ch)) {
      return 0;
    }
  }
  return slash != SIZE_MAX && slash > 0U && slash + 1U < len;
}

static int valid_test_loopback_url(const char *url) {
  const char *enabled = getenv("FCLAW_MEDIA_TEST_ALLOW_LOOPBACK");
  const char *port;
  unsigned long value = 0;
  if (!enabled || strcmp(enabled, "1") != 0 || !url ||
      strncmp(url, "http://127.0.0.1:", 17U) != 0)
    return 0;
  port = url + 17U;
  if (*port < '0' || *port > '9') return 0;
  while (*port >= '0' && *port <= '9') {
    value = value * 10U + (unsigned long)(*port - '0');
    if (value > 65535U) return 0;
    port++;
  }
  if (value == 0U || *port != '/') return 0;
  for (const char *p = port; *p; ++p) {
    unsigned char ch = (unsigned char)*p;
    if (ch <= 32U || ch == 127U || ch == '\\' || ch == '#') return 0;
  }
  return strlen(url) < FC_MEDIA_SOURCE_URL_MAX;
}

static int valid_source_url(const char *url) {
  const char *authority;
  const char *path;
  size_t len;
  if (valid_test_loopback_url(url)) return 1;
  if (!url || strncmp(url, "https://", 8U) != 0) return 0;
  len = strlen(url);
  if (len <= 8U || len >= FC_MEDIA_SOURCE_URL_MAX) return 0;
  authority = url + 8U;
  path = authority;
  while (*path && *path != '/' && *path != '?' && *path != '#') {
    unsigned char ch = (unsigned char)*path;
    if (ch <= 32U || ch == 127U || ch == '\\' || ch == '@') return 0;
    path++;
  }
  if (path == authority || *path != '/') return 0;
  for (const char *p = path; *p; ++p) {
    unsigned char ch = (unsigned char)*p;
    if (ch <= 32U || ch == 127U || ch == '\\' || ch == '#') return 0;
  }
  return 1;
}

int fc_media_manifest_validate_descriptor(const FcMediaDescriptor *item,
                                          char *err, size_t err_cap) {
  if (!item) {
    set_error(err, err_cap, "missing descriptor");
    return -1;
  }
  if (!valid_id(item->id)) {
    set_error(err, err_cap, "invalid media id");
    return -1;
  }
  if (!valid_filename(item->filename)) {
    set_error(err, err_cap, "invalid media filename");
    return -1;
  }
  if (!valid_media_type(item->media_type)) {
    set_error(err, err_cap, "invalid media MIME type");
    return -1;
  }
  if (item->size_bytes == 0U ||
      item->size_bytes > FC_MEDIA_MAX_ITEM_BYTES) {
    set_error(err, err_cap, "media item size must be between 1 and %llu bytes",
              (unsigned long long)FC_MEDIA_MAX_ITEM_BYTES);
    return -1;
  }
  if (!valid_source_url(item->source_url)) {
    set_error(err, err_cap, "invalid HTTPS media source URL");
    return -1;
  }
  return 0;
}

static int build_canonical(const FcMediaDescriptor *items, size_t count,
                           uint64_t total_bytes, char **out, size_t *out_len,
                           char *err, size_t err_cap) {
  MediaJson json;
  size_t cap = 256U;
  if (!items || !count || !out || !out_len) return -1;
  *out = NULL;
  *out_len = 0;
  for (size_t i = 0; i < count; ++i) {
    if (add_size(&cap, 192U) != 0 ||
        add_escaped_bound(&cap, items[i].id) != 0 ||
        add_escaped_bound(&cap, items[i].filename) != 0 ||
        add_escaped_bound(&cap, items[i].media_type) != 0 ||
        add_escaped_bound(&cap, items[i].source_url) != 0) {
      set_error(err, err_cap, "media manifest size overflow");
      return -1;
    }
  }
  if (cap > MEDIA_FILE_MAX) {
    set_error(err, err_cap, "media manifest metadata exceeds limit");
    return -1;
  }
  memset(&json, 0, sizeof(json));
  json.data = (char *)fc_xmalloc(cap);
  if (!json.data) {
    set_error(err, err_cap, "media manifest allocation failed");
    return -1;
  }
  json.cap = cap;
  json.data[0] = '\0';
  if (media_json_puts(&json, "{\"version\":1,\"count\":") != 0 ||
      media_json_u64(&json, (uint64_t)count) != 0 ||
      media_json_puts(&json, ",\"total_bytes\":") != 0 ||
      media_json_u64(&json, total_bytes) != 0 ||
      media_json_puts(&json, ",\"items\":[") != 0)
    goto overflow;
  for (size_t i = 0; i < count; ++i) {
    if ((i && media_json_puts(&json, ",") != 0) ||
        media_json_puts(&json, "{\"id\":\"") != 0 ||
        media_json_escape(&json, items[i].id) != 0 ||
        media_json_puts(&json, "\",\"filename\":\"") != 0 ||
        media_json_escape(&json, items[i].filename) != 0 ||
        media_json_puts(&json, "\",\"media_type\":\"") != 0 ||
        media_json_escape(&json, items[i].media_type) != 0 ||
        media_json_puts(&json, "\",\"size_bytes\":") != 0 ||
        media_json_u64(&json, items[i].size_bytes) != 0 ||
        media_json_puts(&json, ",\"source_url\":\"") != 0 ||
        media_json_escape(&json, items[i].source_url) != 0 ||
        media_json_puts(&json, "\"}") != 0)
      goto overflow;
  }
  if (media_json_puts(&json, "]}\n") != 0) goto overflow;
  *out = json.data;
  *out_len = json.len;
  return 0;

overflow:
  fc_xfree(json.data);
  set_error(err, err_cap, "media manifest serialization overflow");
  return -1;
}

static int ensure_directory(const char *path, mode_t create_mode) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    if (errno != ENOENT || mkdir(path, create_mode) != 0) return -1;
    if (lstat(path, &st) != 0) return -1;
  }
  if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }
  return 0;
}

static int ensure_manifest_directories(void) {
  if (ensure_directory("workspace", 0755) != 0 ||
      ensure_directory(MEDIA_ROOT, 0700) != 0 ||
      ensure_directory(MEDIA_INBOUND_DIR, 0700) != 0)
    return -1;
  return 0;
}

static int private_regular_stat(const struct stat *st) {
  return st && S_ISREG(st->st_mode) && !S_ISLNK(st->st_mode) &&
         st->st_uid == getuid() && (st->st_mode & 0777) == 0600;
}

static int existing_manifest_matches(const char *path,
                                     const char *expected, size_t expected_len) {
  unsigned char buf[4096];
  struct stat before;
  struct stat opened;
  size_t off = 0;
  int fd;
  if (lstat(path, &before) != 0) return -1;
  if (!private_regular_stat(&before) ||
      before.st_size < 0 || (uint64_t)before.st_size != expected_len) {
    errno = EPERM;
    return -1;
  }
  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return -1;
  if (fstat(fd, &opened) != 0 || !private_regular_stat(&opened) ||
      opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
      opened.st_size != before.st_size) {
    int saved_errno = errno ? errno : EPERM;
    (void)close(fd);
    errno = saved_errno;
    return -1;
  }
  while (off < expected_len) {
    size_t want = expected_len - off;
    ssize_t n;
    if (want > sizeof(buf)) want = sizeof(buf);
    do {
      n = read(fd, buf, want);
    } while (n < 0 && errno == EINTR);
    if (n <= 0 || memcmp(buf, expected + off, (size_t)n) != 0) {
      int saved_errno = n < 0 ? errno : EINVAL;
      (void)close(fd);
      errno = saved_errno;
      return -1;
    }
    off += (size_t)n;
  }
  {
    unsigned char extra;
    ssize_t n;
    do {
      n = read(fd, &extra, 1U);
    } while (n < 0 && errno == EINTR);
    if (n != 0) {
      int saved_errno = n < 0 ? errno : EINVAL;
      (void)close(fd);
      errno = saved_errno;
      return -1;
    }
  }
  if (close(fd) != 0) return -1;
  return 0;
}

static int write_all(int fd, const char *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return -1;
    off += (size_t)n;
  }
  return 0;
}

static int sync_fd(int fd) {
  int rc;
  do {
    rc = fsync(fd);
  } while (rc != 0 && errno == EINTR);
  return rc;
}

static int install_manifest(const char *path, const char *data, size_t len,
                            char *err, size_t err_cap) {
  static unsigned long sequence;
  char tmp[FC_MEDIA_MANIFEST_PATH_SIZE + 64U];
  int fd = -1;
  int saved_errno = 0;
  if (lstat(path, &(struct stat){0}) == 0) {
    if (existing_manifest_matches(path, data, len) == 0) return 0;
    set_error(err, err_cap, "existing media manifest is not identical and private");
    return -1;
  }
  if (errno != ENOENT) {
    set_error(err, err_cap, "cannot inspect media manifest destination");
    return -1;
  }
  for (int attempt = 0; attempt < 100; ++attempt) {
    unsigned long claim = ++sequence;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.%lu",
                 path, (long)getpid(), claim) >= (int)sizeof(tmp)) {
      errno = ENAMETOOLONG;
      break;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) break;
    if (errno != EEXIST) break;
  }
  if (fd < 0) {
    set_error(err, err_cap, "cannot create private media manifest");
    return -1;
  }
  if (fchmod(fd, 0600) != 0 || write_all(fd, data, len) != 0 ||
      sync_fd(fd) != 0) {
    saved_errno = errno;
    (void)close(fd);
    (void)unlink(tmp);
    errno = saved_errno;
    set_error(err, err_cap, "cannot commit private media manifest");
    return -1;
  }
  if (close(fd) != 0) {
    saved_errno = errno;
    (void)unlink(tmp);
    errno = saved_errno;
    set_error(err, err_cap, "cannot commit private media manifest");
    return -1;
  }
  /* link(2) gives an atomic no-clobber install. If another writer won with
   * the same content address, validate its bytes and permissions instead. */
  if (link(tmp, path) != 0) {
    saved_errno = errno;
    (void)unlink(tmp);
    if (saved_errno == EEXIST &&
        existing_manifest_matches(path, data, len) == 0)
      return 0;
    errno = saved_errno;
    set_error(err, err_cap, "cannot install media manifest atomically");
    return -1;
  }
  if (unlink(tmp) != 0) {
    saved_errno = errno;
    set_error(err, err_cap, "media manifest installed but temp cleanup failed");
    errno = saved_errno;
    return -1;
  }
  {
    int dir_fd = open(MEDIA_INBOUND_DIR, O_RDONLY);
    if (dir_fd < 0) {
      set_error(err, err_cap, "media manifest directory sync failed");
      return -1;
    }
    if (sync_fd(dir_fd) != 0) {
      saved_errno = errno;
      (void)close(dir_fd);
      errno = saved_errno;
      set_error(err, err_cap, "media manifest directory sync failed");
      return -1;
    }
    if (close(dir_fd) != 0) {
      set_error(err, err_cap, "media manifest directory sync failed");
      return -1;
    }
  }
  return 0;
}

int fc_media_manifest_write(const FcMediaDescriptor *items, size_t count,
                            FcMediaManifestRef *out,
                            char *err, size_t err_cap) {
  unsigned char digest[FC_SHA256_DIGEST_SIZE];
  char *canonical = NULL;
  char path[FC_MEDIA_MANIFEST_PATH_SIZE + 16U];
  size_t canonical_len = 0;
  uint64_t total = 0;
  if (err && err_cap) err[0] = '\0';
  if (!out) {
    set_error(err, err_cap, "missing media manifest output");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  if (!items || count == 0U || count > FC_MEDIA_MAX_ITEMS) {
    set_error(err, err_cap, "media attachment count must be between 1 and %u",
              FC_MEDIA_MAX_ITEMS);
    return -1;
  }
  for (size_t i = 0; i < count; ++i) {
    char item_error[128];
    if (fc_media_manifest_validate_descriptor(&items[i], item_error,
                                               sizeof(item_error)) != 0) {
      set_error(err, err_cap, "media item %zu: %s", i, item_error);
      return -1;
    }
    if (items[i].size_bytes > UINT64_MAX - total) {
      set_error(err, err_cap, "media total size overflow");
      return -1;
    }
    total += items[i].size_bytes;
    if (total > FC_MEDIA_MAX_TOTAL_BYTES) {
      set_error(err, err_cap, "media total exceeds %llu bytes",
                (unsigned long long)FC_MEDIA_MAX_TOTAL_BYTES);
      return -1;
    }
  }
  if (build_canonical(items, count, total, &canonical, &canonical_len,
                      err, err_cap) != 0)
    return -1;
  fc_sha256(canonical, canonical_len, digest);
  fc_sha256_hex(digest, out->sha256);
  if (snprintf(out->manifest, sizeof(out->manifest),
               "media/inbound/%s.json", out->sha256) >=
      (int)sizeof(out->manifest) ||
      snprintf(path, sizeof(path), "workspace/%s", out->manifest) >=
      (int)sizeof(path)) {
    fc_xfree(canonical);
    memset(out, 0, sizeof(*out));
    set_error(err, err_cap, "media manifest path overflow");
    return -1;
  }
  if (ensure_manifest_directories() != 0 ||
      install_manifest(path, canonical, canonical_len, err, err_cap) != 0) {
    if (err && err_cap && !err[0])
      set_error(err, err_cap, "cannot prepare private media manifest storage");
    fc_xfree(canonical);
    memset(out, 0, sizeof(*out));
    return -1;
  }
  fc_xfree(canonical);
  out->count = count;
  out->total_bytes = total;
  return 0;
}

static int lowercase_digest(const char *digest) {
  if (!digest || strlen(digest) != FC_MEDIA_SHA256_HEX_SIZE - 1U) return 0;
  for (size_t i = 0; i < FC_MEDIA_SHA256_HEX_SIZE - 1U; ++i)
    if (!((digest[i] >= '0' && digest[i] <= '9') ||
          (digest[i] >= 'a' && digest[i] <= 'f')))
      return 0;
  return 1;
}

static int valid_reference_path(const FcMediaManifestRef *ref) {
  char expected[FC_MEDIA_MANIFEST_PATH_SIZE];
  if (!ref || !lowercase_digest(ref->sha256)) return 0;
  if (snprintf(expected, sizeof(expected), "media/inbound/%s.json",
               ref->sha256) >= (int)sizeof(expected))
    return 0;
  return strcmp(ref->manifest, expected) == 0;
}

static int valid_reference(const FcMediaManifestRef *ref) {
  return valid_reference_path(ref) && ref->count > 0U &&
         ref->count <= FC_MEDIA_MAX_ITEMS && ref->total_bytes > 0U &&
         ref->total_bytes <= FC_MEDIA_MAX_TOTAL_BYTES;
}

int fc_media_manifest_ref_json(const FcMediaManifestRef *ref,
                               char *out, size_t out_cap) {
  int n;
  if (!out || out_cap == 0U) return -1;
  out[0] = '\0';
  if (!valid_reference(ref)) return -1;
  n = snprintf(out, out_cap,
               "{\"version\":1,\"manifest\":\"%s\",\"sha256\":\"%s\","
               "\"count\":%zu,\"total_bytes\":%" PRIu64 "}",
               ref->manifest, ref->sha256, ref->count, ref->total_bytes);
  return n >= 0 && (size_t)n < out_cap ? 0 : -1;
}

void fc_media_manifest_dispose(FcMediaManifest *manifest) {
  if (!manifest) return;
  for (size_t i = 0; i < manifest->count; ++i) {
    fc_xfree((void *)manifest->items[i].id);
    fc_xfree((void *)manifest->items[i].filename);
    fc_xfree((void *)manifest->items[i].media_type);
    fc_xfree((void *)manifest->items[i].source_url);
  }
  fc_xfree(manifest->items);
  memset(manifest, 0, sizeof(*manifest));
}

static char *object_string_dup(const JsonRef *object, const char *key) {
  JsonRef value;
  if (json_ref_object_get(object, key, &value) != 0 ||
      value.type != JSON_REF_STRING)
    return NULL;
  return json_ref_string_dup(&value);
}

static int parse_canonical(const char *text, size_t text_len,
                           const FcMediaManifestRef *ref,
                           int require_ref_aggregates,
                           FcMediaManifest *out,
                           char *err, size_t err_cap) {
  JsonRef root;
  JsonRef items_ref;
  long long version;
  long long declared_count;
  long long declared_total;
  uint64_t total = 0;
  char *rebuilt = NULL;
  size_t rebuilt_len = 0;
  if (json_ref_from_text(text, &root) != 0 || root.type != JSON_REF_OBJECT ||
      json_ref_object_get_long(&root, "version", &version) != 0 ||
      version != 1 ||
      json_ref_object_get_long(&root, "count", &declared_count) != 0 ||
      declared_count <= 0 ||
      (uint64_t)declared_count > FC_MEDIA_MAX_ITEMS ||
      json_ref_object_get_long(&root, "total_bytes", &declared_total) != 0 ||
      declared_total <= 0 ||
      (uint64_t)declared_total > FC_MEDIA_MAX_TOTAL_BYTES ||
      json_ref_object_get_array(&root, "items", &items_ref) != 0 ||
      json_ref_array_size(&items_ref) != (size_t)declared_count) {
    set_error(err, err_cap, "media manifest schema is invalid");
    return -1;
  }
  out->items = (FcMediaDescriptor *)fc_xcalloc((size_t)declared_count,
                                               sizeof(*out->items));
  if (!out->items) {
    set_error(err, err_cap, "media manifest allocation failed");
    return -1;
  }
  out->count = (size_t)declared_count;
  for (size_t i = 0; i < out->count; ++i) {
    JsonRef item;
    long long size;
    char item_error[128];
    if (json_ref_array_get(&items_ref, i, &item) != 0 ||
        item.type != JSON_REF_OBJECT ||
        !(out->items[i].id = object_string_dup(&item, "id")) ||
        !(out->items[i].filename = object_string_dup(&item, "filename")) ||
        !(out->items[i].media_type =
              object_string_dup(&item, "media_type")) ||
        !(out->items[i].source_url =
              object_string_dup(&item, "source_url")) ||
        json_ref_object_get_long(&item, "size_bytes", &size) != 0 ||
        size < 0) {
      set_error(err, err_cap, "media manifest item %zu has invalid schema", i);
      goto fail;
    }
    out->items[i].size_bytes = (uint64_t)size;
    if (fc_media_manifest_validate_descriptor(&out->items[i], item_error,
                                               sizeof(item_error)) != 0) {
      set_error(err, err_cap, "media manifest item %zu: %s", i, item_error);
      goto fail;
    }
    if (out->items[i].size_bytes > UINT64_MAX - total) {
      set_error(err, err_cap, "media manifest total size overflow");
      goto fail;
    }
    total += out->items[i].size_bytes;
    if (total > FC_MEDIA_MAX_TOTAL_BYTES) {
      set_error(err, err_cap, "media manifest total exceeds limit");
      goto fail;
    }
  }
  if (total != (uint64_t)declared_total ||
      (require_ref_aggregates &&
       (out->count != ref->count || total != ref->total_bytes))) {
    set_error(err, err_cap, "media manifest aggregate does not match reference");
    goto fail;
  }
  out->total_bytes = total;
  if (build_canonical(out->items, out->count, total, &rebuilt, &rebuilt_len,
                      err, err_cap) != 0)
    goto fail;
  if (rebuilt_len != text_len || memcmp(rebuilt, text, text_len) != 0) {
    set_error(err, err_cap, "media manifest JSON is not canonical");
    fc_xfree(rebuilt);
    goto fail;
  }
  fc_xfree(rebuilt);
  return 0;

fail:
  fc_media_manifest_dispose(out);
  return -1;
}

static int read_private_manifest(const char *path, char **out,
                                 size_t *out_len,
                                 char *err, size_t err_cap) {
  struct stat before;
  struct stat opened;
  char *text = NULL;
  size_t off = 0;
  int fd = -1;
  if (lstat(path, &before) != 0 || !private_regular_stat(&before) ||
      before.st_size <= 0 || (uint64_t)before.st_size > MEDIA_FILE_MAX) {
    set_error(err, err_cap, "media manifest is missing, unsafe, or too large");
    return -1;
  }
  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0 || fstat(fd, &opened) != 0 ||
      !private_regular_stat(&opened) ||
      opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
      opened.st_size != before.st_size) {
    int saved_errno = errno;
    if (fd >= 0) (void)close(fd);
    errno = saved_errno;
    set_error(err, err_cap, "media manifest changed or is unsafe");
    return -1;
  }
  text = (char *)fc_xmalloc((size_t)opened.st_size + 1U);
  if (!text) {
    (void)close(fd);
    set_error(err, err_cap, "media manifest allocation failed");
    return -1;
  }
  while (off < (size_t)opened.st_size) {
    ssize_t n;
    do {
      n = read(fd, text + off, (size_t)opened.st_size - off);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
      int saved_errno = n < 0 ? errno : EIO;
      fc_xfree(text);
      (void)close(fd);
      errno = saved_errno;
      set_error(err, err_cap, "cannot read complete media manifest");
      return -1;
    }
    off += (size_t)n;
  }
  if (close(fd) != 0) {
    int saved_errno = errno;
    fc_xfree(text);
    errno = saved_errno;
    set_error(err, err_cap, "cannot close media manifest");
    return -1;
  }
  text[off] = '\0';
  *out = text;
  *out_len = off;
  return 0;
}

static int read_reference(const FcMediaManifestRef *ref,
                          int require_ref_aggregates,
                          FcMediaManifest *out,
                          char *err, size_t err_cap) {
  unsigned char digest[FC_SHA256_DIGEST_SIZE];
  char actual_sha[FC_SHA256_HEX_SIZE];
  char path[FC_MEDIA_MANIFEST_PATH_SIZE + 16U];
  char *text = NULL;
  size_t text_len = 0;
  if (err && err_cap) err[0] = '\0';
  if (!out) {
    set_error(err, err_cap, "missing media manifest output");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  if ((!require_ref_aggregates && !valid_reference_path(ref)) ||
      (require_ref_aggregates && !valid_reference(ref)) ||
      snprintf(path, sizeof(path), "workspace/%s", ref->manifest) >=
          (int)sizeof(path)) {
    set_error(err, err_cap, "media manifest reference is invalid");
    return -1;
  }
  if (read_private_manifest(path, &text, &text_len, err, err_cap) != 0)
    return -1;
  fc_sha256(text, text_len, digest);
  fc_sha256_hex(digest, actual_sha);
  if (strcmp(actual_sha, ref->sha256) != 0) {
    fc_xfree(text);
    set_error(err, err_cap, "media manifest digest mismatch");
    return -1;
  }
  if (parse_canonical(text, text_len, ref, require_ref_aggregates,
                      out, err, err_cap) != 0) {
    fc_xfree(text);
    return -1;
  }
  fc_xfree(text);
  return 0;
}

int fc_media_manifest_read(const FcMediaManifestRef *ref,
                           FcMediaManifest *out,
                           char *err, size_t err_cap) {
  return read_reference(ref, 1, out, err, err_cap);
}

int fc_media_manifest_read_path(const char *manifest,
                                FcMediaManifest *out,
                                FcMediaManifestRef *verified_ref,
                                char *err, size_t err_cap) {
  static const char prefix[] = "media/inbound/";
  static const char suffix[] = ".json";
  FcMediaManifestRef ref;
  size_t manifest_len;
  size_t expected_len = sizeof(prefix) - 1U +
                        FC_MEDIA_SHA256_HEX_SIZE - 1U +
                        sizeof(suffix) - 1U;
  if (!manifest || !out || (manifest_len = strlen(manifest)) != expected_len ||
      strncmp(manifest, prefix, sizeof(prefix) - 1U) != 0 ||
      strcmp(manifest + manifest_len - (sizeof(suffix) - 1U), suffix) != 0) {
    if (out) memset(out, 0, sizeof(*out));
    set_error(err, err_cap, "media manifest path is invalid");
    return -1;
  }
  memset(&ref, 0, sizeof(ref));
  memcpy(ref.manifest, manifest, manifest_len + 1U);
  memcpy(ref.sha256, manifest + sizeof(prefix) - 1U,
         FC_MEDIA_SHA256_HEX_SIZE - 1U);
  ref.sha256[FC_MEDIA_SHA256_HEX_SIZE - 1U] = '\0';
  if (!valid_reference_path(&ref) ||
      read_reference(&ref, 0, out, err, err_cap) != 0)
    return -1;
  ref.count = out->count;
  ref.total_bytes = out->total_bytes;
  if (verified_ref) *verified_ref = ref;
  return 0;
}
