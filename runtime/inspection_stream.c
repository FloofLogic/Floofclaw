#include "inspection_stream.h"

#include "llm/llm.h"
#include "support/fsutil.h"
#include "support/json.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INSPECTION_USAGE_PATH "workspace/logs/llm_usage.jsonl"
#define INSPECTION_MAX_CACHE_CALLS 10
#define INSPECTION_RAW_MAX (68U * 1024U)

typedef enum {
  INSPECTION_USAGE,
  INSPECTION_CACHEVIEW
} InspectionKind;

typedef struct {
  char run_id[RT_SMALL];
  char meta_path[PATH_MAX];
  char raw_path[PATH_MAX];
  long long order;
  int call_seq;
} InspectionCall;

typedef struct {
  char id[RT_SMALL];
  char profile[RT_SMALL];
} InspectionAgent;

struct RtInspectionStream {
  InspectionKind kind;
  char floop[RT_SMALL];
  char agent[RT_SMALL];
  char run_id[RT_SMALL];
  int call_seq;
  int call_seq_set;
  int scope_pending;
  FILE *usage;
  InspectionCall calls[INSPECTION_MAX_CACHE_CALLS];
  int call_count;
  int call_index;
  InspectionAgent agents[RT_MAX_STEPS];
  size_t agent_count;
};

static int safe_identifier(const char *value) {
  if (!value || !*value) return 0;
  for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
    if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return 0;
  return 1;
}

static int read_bounded(const char *path, char *out, size_t out_len) {
  FILE *file;
  long size;
  if (!path || !out || out_len == 0) return -1;
  file = fopen(path, "rb");
  if (!file) return -1;
  if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
      (size_t)size >= out_len || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return -1;
  }
  if (size > 0 && fread(out, 1, (size_t)size, file) != (size_t)size) {
    fclose(file);
    return -1;
  }
  fclose(file);
  out[size] = '\0';
  return 0;
}

static int appendf(char *out, size_t out_len, size_t *pos,
                   const char *format, ...) {
  va_list args;
  int written;
  if (!out || !pos || *pos >= out_len) return -1;
  va_start(args, format);
  written = vsnprintf(out + *pos, out_len - *pos, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= out_len - *pos) return -1;
  *pos += (size_t)written;
  return 0;
}

static int append_json_string(char *out, size_t out_len, size_t *pos,
                              const char *text, size_t text_len) {
  if (appendf(out, out_len, pos, "\"") != 0) return -1;
  for (size_t i = 0; i < text_len; ++i) {
    unsigned char c = (unsigned char)text[i];
    const char *escaped = NULL;
    char control[7];
    switch (c) {
      case '"': escaped = "\\\""; break;
      case '\\': escaped = "\\\\"; break;
      case '\b': escaped = "\\b"; break;
      case '\f': escaped = "\\f"; break;
      case '\n': escaped = "\\n"; break;
      case '\r': escaped = "\\r"; break;
      case '\t': escaped = "\\t"; break;
      default:
        if (c < 0x20) {
          snprintf(control, sizeof(control), "\\u%04x", c);
          escaped = control;
        }
    }
    if (escaped) {
      if (appendf(out, out_len, pos, "%s", escaped) != 0) return -1;
    } else {
      if (*pos + 1 >= out_len) return -1;
      out[(*pos)++] = (char)c;
      out[*pos] = '\0';
    }
  }
  return appendf(out, out_len, pos, "\"");
}

static int copy_agents(RtInspectionStream *stream,
                       const RtInspectionOptions *options) {
  if (!options->agents) return 0;
  for (size_t i = 0; i < options->agent_count; ++i) {
    int duplicate = 0;
    if (strcmp(options->agents[i].executor, "llm") != 0) continue;
    for (size_t j = 0; j < stream->agent_count; ++j)
      if (strcmp(stream->agents[j].id, options->agents[i].id) == 0)
        duplicate = 1;
    if (duplicate) continue;
    if (stream->agent_count >= RT_MAX_STEPS) return -1;
    snprintf(stream->agents[stream->agent_count].id,
             sizeof(stream->agents[stream->agent_count].id), "%s",
             options->agents[i].id);
    snprintf(stream->agents[stream->agent_count].profile,
             sizeof(stream->agents[stream->agent_count].profile), "%s",
             options->agents[i].model_ref);
    stream->agent_count++;
  }
  return 0;
}

static int render_scope(const RtInspectionStream *stream,
                        char *out, size_t out_len) {
  char floop[RT_MED];
  size_t pos = 0;
  const char *kind = stream->kind == INSPECTION_USAGE
                         ? "current_log_segment"
                         : "provider_call_artifacts";
  if (json_escape(stream->floop, floop, sizeof(floop)) != 0) return -1;
  if (appendf(out, out_len, &pos,
              "{\"schema_version\":1,\"type\":\"scope\","
              "\"scope\":{\"kind\":\"%s\",\"floop\":\"%s\","
              "\"agents\":[", kind, floop) != 0)
    return -1;
  for (size_t i = 0; i < stream->agent_count; ++i) {
    char id[RT_MED], profile[RT_MED];
    if (json_escape(stream->agents[i].id, id, sizeof(id)) != 0 ||
        json_escape(stream->agents[i].profile, profile, sizeof(profile)) != 0)
      return -1;
    if (appendf(out, out_len, &pos,
                "%s{\"id\":\"%s\",\"profile\":\"%s\"}",
                i ? "," : "", id, profile) != 0)
      return -1;
  }
  return appendf(out, out_len, &pos, "]}}\n");
}

static void keep_latest(InspectionCall *calls, int *count, int limit,
                        const InspectionCall *candidate) {
  int slot = *count;
  if (*count < limit) {
    (*count)++;
  } else {
    long long oldest = calls[0].order;
    slot = 0;
    for (int i = 1; i < *count; ++i) {
      if (calls[i].order < oldest) {
        oldest = calls[i].order;
        slot = i;
      }
    }
    if (candidate->order <= oldest) return;
  }
  calls[slot] = *candidate;
}

static void sort_latest(InspectionCall *calls, int count) {
  for (int i = 0; i < count; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (calls[j].order > calls[i].order) {
        InspectionCall swap = calls[i];
        calls[i] = calls[j];
        calls[j] = swap;
      }
    }
  }
}

static int scan_cache_calls(RtInspectionStream *stream, int recent) {
  DIR *runs = opendir("workspace/runs");
  struct dirent *entry;
  if (!runs) return 0;
  while ((entry = readdir(runs)) != NULL) {
    int run_number = 0;
    if (sscanf(entry->d_name, "run_%d", &run_number) != 1) continue;
    if (stream->run_id[0] && strcmp(stream->run_id, entry->d_name) != 0)
      continue;
    for (int seq = 1; seq < 999; ++seq) {
      InspectionCall call;
      char meta[RT_LARGE];
      char meta_agent[RT_SMALL] = "";
      char meta_floop[RT_SMALL] = "";
      char profile[RT_SMALL] = "";
      char provider[RT_SMALL] = "";
      char model[RT_SMALL] = "";
      char url[RT_LARGE] = "";
      JsonRef root, cached;
      long long cached_tokens, input_tokens, input_media_count;
      memset(&call, 0, sizeof(call));
      snprintf(call.meta_path, sizeof(call.meta_path),
               "workspace/runs/%s/provider_calls/%03d_meta.json",
               entry->d_name, seq);
      if (!fs_file_exists(call.meta_path)) {
        if (seq > 1) break;
        continue;
      }
      if (stream->call_seq_set && stream->call_seq != seq) continue;
      if (read_bounded(call.meta_path, meta, sizeof(meta)) != 0 ||
          json_ref_top_object(meta, &root) != 0 ||
          json_ref_object_get_string(&root, "agent", meta_agent,
                                     sizeof(meta_agent)) != 0 ||
          strcmp(meta_agent, stream->agent) != 0 ||
          json_ref_object_get_string(&root, "floop", meta_floop,
                                     sizeof(meta_floop)) != 0 ||
          strcmp(meta_floop, stream->floop) != 0 ||
          json_ref_object_get_string(&root, "profile", profile,
                                     sizeof(profile)) != 0 ||
          !profile[0] ||
          json_ref_object_get_string(&root, "provider", provider,
                                     sizeof(provider)) != 0 ||
          !provider[0] ||
          json_ref_object_get_string(&root, "model", model,
                                     sizeof(model)) != 0 ||
          !model[0] ||
          json_ref_object_get_string(&root, "url", url, sizeof(url)) != 0 ||
          !url[0] ||
          json_ref_object_get_long(&root, "input_tokens", &input_tokens) != 0 ||
          input_tokens < 0 ||
          json_ref_object_get_long(&root, "input_media_count",
                                   &input_media_count) != 0 ||
          input_media_count != 0 ||
          json_ref_object_get(&root, "provider_cached_input_tokens",
                              &cached) != 0 ||
          (!json_ref_is_null(&cached) &&
           (json_ref_get_long(&cached, &cached_tokens) != 0 ||
            cached_tokens < 0)))
        continue;
      snprintf(call.raw_path, sizeof(call.raw_path),
               "workspace/runs/%s/provider_calls/%03d_raw_request.json",
               entry->d_name, seq);
      if (!fs_file_exists(call.raw_path)) continue;
      snprintf(call.run_id, sizeof(call.run_id), "%s", entry->d_name);
      call.call_seq = seq;
      call.order = (long long)run_number * 1000LL + seq;
      keep_latest(stream->calls, &stream->call_count, recent, &call);
    }
  }
  closedir(runs);
  sort_latest(stream->calls, stream->call_count);
  return 0;
}

static RtInspectionStream *stream_open(const RtInspectionOptions *options,
                                       InspectionKind kind) {
  RtInspectionStream *stream;
  int recent;
  if (!options || !safe_identifier(options->floop)) return NULL;
  if (options->agent && *options->agent && !safe_identifier(options->agent))
    return NULL;
  if (options->run_id && *options->run_id &&
      !safe_identifier(options->run_id))
    return NULL;
  stream = (RtInspectionStream *)calloc(1, sizeof(*stream));
  if (!stream) return NULL;
  stream->kind = kind;
  stream->scope_pending = 1;
  snprintf(stream->floop, sizeof(stream->floop), "%s", options->floop);
  if (options->agent)
    snprintf(stream->agent, sizeof(stream->agent), "%s", options->agent);
  if (options->run_id)
    snprintf(stream->run_id, sizeof(stream->run_id), "%s", options->run_id);
  stream->call_seq = options->call_seq;
  stream->call_seq_set = options->call_seq_set;
  if (copy_agents(stream, options) != 0) {
    free(stream);
    return NULL;
  }
  if (kind == INSPECTION_USAGE) {
    stream->usage = fopen(INSPECTION_USAGE_PATH, "rb");
    return stream;
  }
  if (!stream->agent[0]) {
    free(stream);
    return NULL;
  }
  recent = options->recent > 0 ? options->recent : 10;
  if (recent > INSPECTION_MAX_CACHE_CALLS) recent = INSPECTION_MAX_CACHE_CALLS;
  if (scan_cache_calls(stream, recent) != 0) {
    free(stream);
    return NULL;
  }
  return stream;
}

RtInspectionStream *rt_usage_record_stream_open(
    const RtInspectionOptions *options) {
  return stream_open(options, INSPECTION_USAGE);
}

RtInspectionStream *rt_cacheview_record_stream_open(
    const RtInspectionOptions *options) {
  return stream_open(options, INSPECTION_CACHEVIEW);
}

static int next_usage(RtInspectionStream *stream, char *out, size_t out_len) {
  char line[RT_LARGE];
  if (!stream->usage) return 0;
  while (fgets(line, sizeof(line), stream->usage)) {
    LlmUsageRecord record;
    size_t len = strlen(line);
    int written;
    if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
      int c;
      while ((c = fgetc(stream->usage)) != EOF && c != '\n') {}
      continue;
    }
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (llm_usage_record_parse(line, &record) != 0) continue;
    if (strcmp(record.floop, stream->floop) != 0)
      continue;
    if (stream->agent[0] && strcmp(record.agent_id, stream->agent) != 0)
      continue;
    written = snprintf(out, out_len,
                       "{\"schema_version\":1,\"type\":\"usage_record\","
                       "\"record\":%s}\n", line);
    return written < 0 || (size_t)written >= out_len ? -1 : 1;
  }
  return 0;
}

static int next_cacheview(RtInspectionStream *stream,
                          char *out, size_t out_len) {
  InspectionCall *call;
  char meta[RT_LARGE];
  char *raw;
  char run_id[RT_MED], profile[RT_MED], provider[RT_MED], model[RT_MED];
  char url[RT_LARGE];
  long long input_tokens = 0, cached_tokens = 0;
  int cache_reported;
  JsonRef meta_root, raw_root, cached;
  size_t raw_len, pos = 0;
  int rc = -1;
  if (stream->call_index >= stream->call_count) return 0;
  call = &stream->calls[stream->call_index++];
  raw = (char *)malloc(INSPECTION_RAW_MAX);
  if (!raw) return -1;
  if (read_bounded(call->meta_path, meta, sizeof(meta)) != 0 ||
      read_bounded(call->raw_path, raw, INSPECTION_RAW_MAX) != 0 ||
      json_ref_top_object(meta, &meta_root) != 0 ||
      json_ref_from_text(raw, &raw_root) != 0 ||
      json_escape(call->run_id, run_id, sizeof(run_id)) != 0 ||
      json_ref_object_get_string(&meta_root, "profile", profile,
                                 sizeof(profile)) != 0 ||
      !profile[0] ||
      json_ref_object_get_string(&meta_root, "provider", provider,
                                 sizeof(provider)) != 0 ||
      !provider[0] ||
      json_ref_object_get_string(&meta_root, "model", model,
                                 sizeof(model)) != 0 ||
      !model[0] ||
      json_ref_object_get_string(&meta_root, "url", url, sizeof(url)) != 0 ||
      !url[0] ||
      json_ref_object_get_long(&meta_root, "input_tokens", &input_tokens) != 0 ||
      input_tokens < 0)
    goto done;
  if (json_ref_object_get(&meta_root, "provider_cached_input_tokens",
                          &cached) != 0)
    goto done;
  cache_reported = !json_ref_is_null(&cached);
  if (cache_reported &&
      (json_ref_get_long(&cached, &cached_tokens) != 0 || cached_tokens < 0))
    goto done;
  raw_len = strlen(raw);
  while (raw_len > 0 && (raw[raw_len - 1] == '\n' || raw[raw_len - 1] == '\r'))
    raw[--raw_len] = '\0';
  if (appendf(out, out_len, &pos,
              "{\"schema_version\":1,\"type\":\"cacheview_call\","
              "\"run_id\":\"%s\",\"call_seq\":%d,\"profile\":",
              run_id, call->call_seq) != 0 ||
      append_json_string(out, out_len, &pos, profile, strlen(profile)) != 0 ||
      appendf(out, out_len, &pos, ",\"provider\":") != 0 ||
      append_json_string(out, out_len, &pos, provider, strlen(provider)) != 0 ||
      appendf(out, out_len, &pos, ",\"model\":") != 0 ||
      append_json_string(out, out_len, &pos, model, strlen(model)) != 0 ||
      appendf(out, out_len, &pos, ",\"url\":") != 0 ||
      append_json_string(out, out_len, &pos, url, strlen(url)) != 0 ||
      appendf(out, out_len, &pos,
              ",\"bytes\":%zu,\"input_tokens\":%lld,"
              "\"provider_cached_input_tokens\":",
              raw_len, input_tokens) != 0)
    goto done;
  if (cache_reported) {
    if (appendf(out, out_len, &pos, "%lld", cached_tokens) != 0) goto done;
  } else if (appendf(out, out_len, &pos, "null") != 0) {
    goto done;
  }
  if (appendf(out, out_len, &pos, ",\"raw_request\":") != 0 ||
      append_json_string(out, out_len, &pos, raw, raw_len) != 0 ||
      appendf(out, out_len, &pos, "}\n") != 0)
    goto done;
  rc = 1;
done:
  free(raw);
  return rc;
}

int rt_inspection_stream_next(RtInspectionStream *stream,
                              char *out, size_t out_len) {
  if (!stream || !out || out_len == 0) return -1;
  out[0] = '\0';
  if (stream->scope_pending) {
    stream->scope_pending = 0;
    return render_scope(stream, out, out_len) == 0 ? 1 : -1;
  }
  return stream->kind == INSPECTION_USAGE
             ? next_usage(stream, out, out_len)
             : next_cacheview(stream, out, out_len);
}

void rt_inspection_stream_close(RtInspectionStream *stream) {
  if (!stream) return;
  if (stream->usage) fclose(stream->usage);
  free(stream);
}
