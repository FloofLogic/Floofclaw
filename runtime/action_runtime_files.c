/* Workspace-scoped file runtime intrinsics and their shared path helpers. */

#include "action_runtime_internal.h"
#include "support/fsutil.h"
#include "support/heap_guard.h"
#include "support/json.h"
#include "support/log_rotation.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int fc_list_directory(RtRun *r, const char *rid, const RtActionDef *def,
                             const char *args, char *result, size_t result_len,
                             char *error, size_t error_len) {
  char path[PATH_MAX] = ".";
  char abs[PATH_MAX];
  char epath[PATH_MAX * 2];
  char listing[1600] = "";
  char elisting[3400];
  size_t listing_pos = 0;
  DIR *d;
  struct dirent *ent;
  size_t pos = 0;
  int truncated = 0;
  (void)rid; (void)def;
  (void)rt_json_get_string(args && *args ? args : "{}", "path", path, sizeof(path));
  if (!path[0]) snprintf(path, sizeof(path), ".");
  if (rt_action_resolve_workspace_path(r, path, abs, sizeof(abs), error, error_len) != 0) {
    snprintf(result, result_len, "{}");
    return -1;
  }
  d = opendir(abs);
  if (!d) {
    char msg[RT_MED], emsg[RT_MED];
    snprintf(msg, sizeof(msg), "could not open directory: %s", path);
    json_escape(msg, emsg, sizeof(emsg));
    snprintf(error, error_len, "{\"message\":\"%s\"}", emsg);
    snprintf(result, result_len, "{}");
    return -1;
  }
  json_escape(path, epath, sizeof(epath));
  pos += (size_t)snprintf(result + pos, result_len - pos,
                          "{\"path\":\"%s\",\"entries\":[", epath);
  while ((ent = readdir(d)) != NULL) {
    char full[PATH_MAX], ename[RT_MED * 2], item[RT_LARGE];
    struct stat st;
    int is_dir_entry = 0;
    long long size = 0;
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    snprintf(full, sizeof(full), "%s/%s", abs, ent->d_name);
    if (stat(full, &st) == 0) {
      is_dir_entry = S_ISDIR(st.st_mode);
      size = (long long)st.st_size;
    }
    json_escape(ent->d_name, ename, sizeof(ename));
    snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"is_dir\":%s,\"size\":%lld}",
             pos > strlen("{\"path\":\"\",\"entries\":[") + strlen(epath) ? "," : "",
             ename, is_dir_entry ? "true" : "false", size);
    if (pos + strlen(item) + 32 >= result_len) { truncated = 1; break; }
    memcpy(result + pos, item, strlen(item));
    pos += strlen(item);
    result[pos] = '\0';
    /* Human-readable line for the "text" the operation driver forwards
     * to the later operation_result turn; entry list past the bound is
     * still complete in "entries". */
    if (listing_pos + strlen(ent->d_name) + 3 < sizeof(listing)) {
      listing_pos += (size_t)snprintf(listing + listing_pos,
                                      sizeof(listing) - listing_pos, "%s%s\n",
                                      ent->d_name, is_dir_entry ? "/" : "");
    }
  }
  closedir(d);
  if (json_escape(listing, elisting, sizeof(elisting)) != 0) elisting[0] = '\0';
  snprintf(result + pos, result_len - pos, "],\"text\":\"%s\",\"truncated\":%s}",
           elisting, truncated ? "true" : "false");
  snprintf(error, error_len, "null");
  return 0;
}

/* Resolve `path` (workspace-root-relative) into an absolute path that stays
 * under the run's workspace root. Returns 0 on success, sets out_err to a JSON
 * {"message":...} error object on failure. */
int rt_action_resolve_workspace_path(const RtRun *r, const char *path,
                                  char *abs, size_t abs_len,
                                  char *out_err, size_t err_len) {
  const char *runs;
  char root[PATH_MAX];
  char ws_abs[PATH_MAX];
  char joined[PATH_MAX];
  size_t ws_len;
  if (!path || !abs || abs_len < 2) {
    snprintf(out_err, err_len, "{\"message\":\"missing path\"}");
    return -1;
  }
  if (!r || !r->ctx.run_dir[0] || !(runs = strstr(r->ctx.run_dir, "/runs/")) ||
      runs == r->ctx.run_dir) {
    snprintf(out_err, err_len, "{\"message\":\"workspace root unavailable\"}");
    return -1;
  }
  if ((size_t)(runs - r->ctx.run_dir) >= sizeof(root)) {
    snprintf(out_err, err_len, "{\"message\":\"workspace root too long\"}");
    return -1;
  }
  memcpy(root, r->ctx.run_dir, (size_t)(runs - r->ctx.run_dir));
  root[runs - r->ctx.run_dir] = '\0';
  if (!realpath(root, ws_abs)) {
    if (mkdir(root, 0755) != 0 && errno != EEXIST) {
      snprintf(out_err, err_len, "{\"message\":\"workspace unavailable\"}");
      return -1;
    }
    if (!realpath(root, ws_abs)) {
      snprintf(out_err, err_len, "{\"message\":\"workspace unavailable\"}");
      return -1;
    }
  }
  ws_len = strlen(ws_abs);
  if (snprintf(joined, sizeof(joined), "%s/%s", ws_abs, path) >= (int)sizeof(joined)) {
    snprintf(out_err, err_len, "{\"message\":\"path too long\"}");
    return -1;
  }
  /* Use realpath when the target exists; otherwise resolve the parent
   * and append the leaf, so we can write files that don't exist yet. */
  if (!realpath(joined, abs)) {
    char parent[PATH_MAX];
    char parent_abs[PATH_MAX];
    const char *slash;
    snprintf(parent, sizeof(parent), "%s", joined);
    slash = strrchr(parent, '/');
    if (!slash) {
      snprintf(out_err, err_len, "{\"message\":\"invalid path\"}");
      return -1;
    }
    *(char *)slash = '\0';
    if (!realpath(parent[0] ? parent : "/", parent_abs)) {
      snprintf(out_err, err_len, "{\"message\":\"path parent missing: %s\"}", path);
      return -1;
    }
    if (snprintf(abs, abs_len, "%s/%s", parent_abs, slash + 1) >= (int)abs_len) {
      snprintf(out_err, err_len, "{\"message\":\"path too long\"}");
      return -1;
    }
  }
  if (strncmp(abs, ws_abs, ws_len) != 0 || (abs[ws_len] != '\0' && abs[ws_len] != '/')) {
    snprintf(out_err, err_len, "{\"message\":\"path escapes workspace\"}");
    return -1;
  }
  return 0;
}

static int append_escaped_prefix(char *out, size_t out_len, size_t *pos,
                                 const char *src, size_t reserve,
                                 size_t *consumed) {
  if (!out || !pos || !consumed || *pos >= out_len) return -1;
  *consumed = 0;
  for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; ++p) {
    const char *escaped = NULL;
    char buf[8];
    size_t add;
    switch (*p) {
    case '"':  escaped = "\\\""; break;
    case '\\': escaped = "\\\\"; break;
    case '\b': escaped = "\\b"; break;
    case '\f': escaped = "\\f"; break;
    case '\n': escaped = "\\n"; break;
    case '\r': escaped = "\\r"; break;
    case '\t': escaped = "\\t"; break;
    default: break;
    }
    if (escaped) {
      add = strlen(escaped);
      if (*pos + add + reserve >= out_len) return 0;
      memcpy(out + *pos, escaped, add);
      *pos += add;
    } else if (*p < 0x20U) {
      int n = snprintf(buf, sizeof(buf), "\\u%04x", *p);
      if (n < 0) return -1;
      add = (size_t)n;
      if (*pos + add + reserve >= out_len) return 0;
      memcpy(out + *pos, buf, add);
      *pos += add;
    } else {
      if (*pos + 1U + reserve >= out_len) return 0;
      out[(*pos)++] = (char)*p;
    }
    out[*pos] = '\0';
    (*consumed)++;
  }
  return 0;
}

static int append_escaped_bytes(char *out, size_t out_len, size_t *pos,
                                const char *src, size_t src_len,
                                size_t reserve) {
  size_t consumed = 0;
  if (!out || !pos || !src) return -1;
  for (size_t i = 0; i < src_len; ++i) {
    char tmp[2] = { src[i], '\0' };
    size_t before = *pos;
    if (append_escaped_prefix(out, out_len, pos, tmp, reserve, &consumed) != 0)
      return -1;
    if (*pos == before) return -1;
  }
  return 0;
}

int fc_read_file(RtRun *r, const char *rid, const RtActionDef *def,
                        const char *args, char *result, size_t result_len,
                        char *error, size_t error_len) {
  enum { READ_CAP = 32768 };
  char path[PATH_MAX] = "";
  char abs[PATH_MAX];
  char epath[PATH_MAX * 2];
  char header[RT_MED], ebase[RT_MED], tool[RT_SMALL], *buf = NULL;
  const char *base;
  FILE *fp = NULL;
  long total = 0, offset = 0, length = READ_CAP, nread = 0, read_end = 0;
  size_t pos = 0;
  JsonRef root;
  long long v;
  int n, has_more = 0;
  (void)rid;
  snprintf(tool, sizeof(tool), "%s", def && def->id[0] ? def->id : "tool");
  (void)rt_json_get_string(args && *args ? args : "{}", "path", path, sizeof(path));
  if (args && json_ref_first_object(args, &root) == 0) {
    if (json_ref_object_get_long(&root, "offset", &v) == 0 && v > 0) offset = (long)v;
    if (json_ref_object_get_long(&root, "length", &v) == 0 && v > 0) length = (long)v;
  }
  if (length > READ_CAP) length = READ_CAP;
  if (!path[0]) {
    snprintf(error, error_len, "{\"message\":\"path is required\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_action_resolve_workspace_path(r, path, abs, sizeof(abs), error, error_len) != 0) {
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    return -1;
  }
  fp = fopen(abs, "rb");
  if (!fp) {
    snprintf(error, error_len, "{\"message\":\"read failed: %s\"}", path);
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) == 0) total = ftell(fp);
  if (total < 0) total = 0;
  if (offset > total) offset = total;
  if (fseek(fp, offset, SEEK_SET) != 0) {
    snprintf(error, error_len, "{\"message\":\"seek failed: %s\"}", path);
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    fclose(fp);
    return -1;
  }
  buf = malloc((size_t)length + 1U);
  if (!buf) {
    snprintf(error, error_len, "{\"message\":\"read allocation failed\"}");
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    fclose(fp);
    return -1;
  }
  nread = (long)fread(buf, 1, (size_t)length, fp);
  buf[nread > 0 ? nread : 0] = '\0';
  has_more = offset + nread < total;
  read_end = offset + nread;
  if (json_escape(path, epath, sizeof(epath)) != 0) {
    snprintf(error, error_len, "{\"message\":\"path escape failed\"}");
    snprintf(result, result_len, "{\"path\":\"\"}");
    free(buf); fclose(fp);
    return -1;
  }
  base = strrchr(path, '/');
  base = base ? base + 1 : path;
  if (json_escape(base, ebase, sizeof(ebase)) != 0) ebase[0] = '\0';
  /* "text" is the one field the operation driver forwards to the later
   * operation_result turn; READ_CAP bounds it. */
  n = snprintf(result, result_len, "{\"path\":\"%s\",\"text\":\"", epath);
  if (n < 0 || (size_t)n >= result_len) {
    snprintf(error, error_len, "{\"message\":\"read result envelope too large: %s\"}", path);
    snprintf(result, result_len, "{}");
    free(buf); fclose(fp);
    return -1;
  }
  pos = (size_t)n;
  if (nread == 0) {
    snprintf(header, sizeof(header), "[END OF FILE - no content at this offset]");
  } else if (has_more) {
    snprintf(header, sizeof(header),
             "[file: %s | total: %ld bytes | read: bytes %ld-%ld]\n"
             "[TRUNCATED - file has more content. Call %s again with offset=%ld to continue.]",
             ebase, total, offset, read_end - 1, tool, read_end);
  } else {
    snprintf(header, sizeof(header),
             "[file: %s | total: %ld bytes | read: bytes %ld-%ld]\n"
             "[END OF FILE - no further content.]",
             ebase, total, offset, read_end - 1);
  }
  if (append_escaped_bytes(result, result_len, &pos, header, strlen(header), 384) != 0 ||
      (nread > 0 && append_escaped_bytes(result, result_len, &pos, "\n\n", 2, 384) != 0) ||
      (nread > 0 && append_escaped_bytes(result, result_len, &pos, buf, (size_t)nread, 384) != 0)) {
    snprintf(error, error_len, "{\"message\":\"read content escape failed: %s\"}", path);
    snprintf(result, result_len, "{}");
    free(buf); fclose(fp);
    return -1;
  }
  n = snprintf(result + pos, result_len - pos,
               "\",\"truncated\":%s,\"bytes\":%ld,\"content_bytes\":%ld,"
               "\"offset\":%ld,\"next_offset\":%ld}",
               has_more ? "true" : "false", total, nread, offset, read_end);
  if (n < 0 || (size_t)n >= result_len - pos) {
    snprintf(error, error_len, "{\"message\":\"read result envelope too large: %s\"}", path);
    snprintf(result, result_len, "{}");
    free(buf); fclose(fp);
    return -1;
  }
  snprintf(error, error_len, "null");
  free(buf);
  fclose(fp);
  return 0;
}

int fc_write_file(RtRun *r, const char *rid, const RtActionDef *def,
                         const char *args, char *result, size_t result_len,
                         char *error, size_t error_len) {
  char path[PATH_MAX] = "";
  char abs[PATH_MAX];
  char epath[PATH_MAX * 2];
  char dir[PATH_MAX];
  const char *slash;
  FILE *fp;
  JsonRef root, content_ref;
  char *content = NULL;
  const char *content_text = "";
  (void)rid; (void)def;
  (void)rt_json_get_string(args && *args ? args : "{}", "path", path, sizeof(path));
  if (args && json_ref_first_object(args, &root) == 0 &&
      json_ref_object_get(&root, "content", &content_ref) == 0) {
    if (content_ref.type != JSON_REF_STRING) {
      snprintf(error, error_len, "{\"message\":\"content must be a string\"}");
      snprintf(result, result_len, "{\"path\":\"%s\"}", path);
      return -1;
    }
    content = json_ref_string_dup(&content_ref);
    if (!content) {
      snprintf(error, error_len, "{\"message\":\"content decode failed\"}");
      snprintf(result, result_len, "{\"path\":\"%s\"}", path);
      return -1;
    }
    content_text = content;
  }
  if (!path[0]) {
    fc_xfree(content);
    snprintf(error, error_len, "{\"message\":\"path is required\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_action_resolve_workspace_path(r, path, abs, sizeof(abs), error, error_len) != 0) {
    fc_xfree(content);
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    return -1;
  }
  snprintf(dir, sizeof(dir), "%s", abs);
  slash = strrchr(dir, '/');
  if (slash) {
    *(char *)slash = '\0';
    if (fs_mkdir_p(dir) != 0) {
      fc_xfree(content);
      snprintf(error, error_len, "{\"message\":\"mkdir parents failed: %s\"}", path);
      snprintf(result, result_len, "{\"path\":\"%s\"}", path);
      return -1;
    }
  }
  fp = fopen(abs, "wb");
  if (!fp) {
    fc_xfree(content);
    snprintf(error, error_len, "{\"message\":\"open for write failed: %s\"}", path);
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    return -1;
  }
  if (fwrite(content_text, 1, strlen(content_text), fp) != strlen(content_text)) {
    fclose(fp);
    fc_xfree(content);
    snprintf(error, error_len, "{\"message\":\"write failed: %s\"}", path);
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    return -1;
  }
  fclose(fp);
  fc_xfree(content);
  if (json_escape(path, epath, sizeof(epath)) != 0) epath[0] = '\0';
  snprintf(result, result_len,
           "{\"path\":\"%s\",\"text\":\"wrote %s\"}", epath, epath);
  snprintf(error, error_len, "null");
  return 0;
}

/* rotate_file — runtime intrinsic wrapping fc_rotate_file() from
 * support/log_rotation.c. Pure mechanism: caller (agent, memory
 * compactor, whatever) supplies path/keep/compress and we rotate.
 * The engine has zero opinion about which files should rotate or when.
 *
 * Args: { path (required, workspace-relative), keep (required, >0),
 *         compress: "gzip"|"none" (default "gzip") }
 *
 * Returns success with { path, keep, compress, action:"rotated" } or
 * a plain "not_present" if the file doesn't exist yet (which is a
 * no-op success, not a failure — a not-yet-created file trivially
 * "needs no rotation"). */
int fc_rotate_file_intrinsic(RtRun *r, const char *rid,
                                    const RtActionDef *def, const char *args,
                                    char *result, size_t result_len,
                                    char *error, size_t error_len) {
  char path[PATH_MAX] = "";
  char abs[PATH_MAX];
  char epath[PATH_MAX * 2];
  char compress_str[16] = "gzip";
  char err_msg[128];
  JsonRef root;
  long long v;
  int keep = 0;
  int compress = 1;
  (void)rid; (void)def;
  if (!args || json_ref_first_object(args, &root) != 0) {
    snprintf(error, error_len, "{\"message\":\"args must be an object\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  (void)json_ref_object_get_string(&root, "path", path, sizeof(path));
  if (json_ref_object_get_long(&root, "keep", &v) == 0) keep = (int)v;
  (void)json_ref_object_get_string(&root, "compress", compress_str, sizeof(compress_str));
  compress = (strcmp(compress_str, "gzip") == 0);
  if (!path[0] || keep <= 0) {
    snprintf(error, error_len,
             "{\"message\":\"path and keep>0 are required\"}");
    snprintf(result, result_len, "{}");
    return -1;
  }
  if (rt_action_resolve_workspace_path(r, path, abs, sizeof(abs), error, error_len) != 0) {
    snprintf(result, result_len, "{\"path\":\"%s\"}", path);
    return -1;
  }
  if (!fs_file_exists(abs)) {
    if (json_escape(path, epath, sizeof(epath)) != 0) epath[0] = '\0';
    snprintf(result, result_len,
             "{\"path\":\"%s\",\"action\":\"not_present\"}", epath);
    snprintf(error, error_len, "null");
    return 0;
  }
  if (compress && !fc_rotation_gzip_available()) compress = 0;
  if (fc_rotate_file(abs, keep, compress, err_msg, sizeof(err_msg)) != 0) {
    if (json_escape(path, epath, sizeof(epath)) != 0) epath[0] = '\0';
    snprintf(error, error_len, "{\"message\":\"rotate failed: %s\"}",
             err_msg[0] ? err_msg : "unknown");
    snprintf(result, result_len, "{\"path\":\"%s\"}", epath);
    return -1;
  }
  if (json_escape(path, epath, sizeof(epath)) != 0) epath[0] = '\0';
  snprintf(result, result_len,
           "{\"path\":\"%s\",\"keep\":%d,\"compress\":\"%s\",\"action\":\"rotated\"}",
           epath, keep, compress ? "gzip" : "none");
  snprintf(error, error_len, "null");
  return 0;
}
