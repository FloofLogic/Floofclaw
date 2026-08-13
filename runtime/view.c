#include "runtime.h"
#include "support/cli_output.h"
#include "support/color.h"

#include <dirent.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  int colorful;
  int full;
  int agent_readable;
  int debug;    /* -d: dump failure payloads and correlated action stderr */
} ViewOpts;

static int read_file_or_warn(const char *path, char **out, int required) {
  if (fs_read_text(path, out, FS_READ_TEXT_DEFAULT_CAP) != 0) {
    if (required) {
      fprintf(stderr, "view: failed to read %s\n", path);
      return -1;
    }
    *out = NULL;
  }
  return 0;
}

static const char *type_color(const char *type) {
  if (strcmp(type, "user_message") == 0) return COLOR_CYAN;
  if (strcmp(type, "action_request") == 0) return COLOR_YELLOW;
  if (strcmp(type, "action_started") == 0) return COLOR_BLUE_BRIGHT;
  if (strcmp(type, "action_succeeded") == 0) return COLOR_GREEN_BRIGHT;
  if (strcmp(type, "action_failed") == 0) return COLOR_RED_BRIGHT;
  if (strcmp(type, "action_rejected") == 0) return COLOR_ORANGE;
  if (strcmp(type, "user") == 0) return COLOR_MAGENTA;
  if (strcmp(type, "asst") == 0) return COLOR_MAGENTA;
  if (strcmp(type, "fact") == 0) return COLOR_MAGENTA;
  if (strcmp(type, "summ") == 0) return COLOR_MAGENTA;
  if (strncmp(type, "task_", 5) == 0) return COLOR_MAGENTA;
  if (strcmp(type, "trace_note") == 0) return COLOR_BRIGHT_BLACK;
  if (strcmp(type, "error") == 0) return COLOR_RED_BRIGHT;
  if (strcmp(type, "run_started") == 0) return COLOR_GREEN;
  if (strcmp(type, "run_done") == 0) return COLOR_GREEN_BRIGHT;
  if (strcmp(type, "run_failed") == 0) return COLOR_RED;
  if (strcmp(type, "run_canceled") == 0) return COLOR_ORANGE;
  return COLOR_WHITE;
}

static void truncate_inline(char *s, size_t max) {
  if (!s || strlen(s) <= max) return;
  s[max] = '\0';
  if (max >= 3) { s[max - 1] = '.'; s[max - 2] = '.'; s[max - 3] = '.'; }
}

static int parse_iso_utc_seconds(const char *ts, long long *out) {
  int year, mon, day, hour, min, sec;
  struct tm tmv;
  if (!ts || !out) return -1;
  if (sscanf(ts, "%d-%d-%dT%d:%d:%dZ", &year, &mon, &day, &hour, &min, &sec) != 6) return -1;
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = year - 1900;
  tmv.tm_mon = mon - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = min;
  tmv.tm_sec = sec;
  *out = (long long)timegm(&tmv);
  return 0;
}

static void print_indent(int indent) {
  for (int i = 0; i < indent; ++i) fputs("  ", stdout);
}

static int starts_jsonish(const char *text) {
  const char *p = text ? text : "";
  while (*p && isspace((unsigned char)*p)) p++;
  return *p == '{' || *p == '[';
}

static void render_jsonish(const ViewOpts *opts, const char *text) {
  int indent = 0;
  int in_string = 0;
  int escaped = 0;
  int at_line_start = 1;
  const char *string_on = opts->colorful ? COLOR_CYAN : "";
  const char *number_on = opts->colorful ? COLOR_YELLOW : "";
  const char *literal_on = opts->colorful ? COLOR_GREEN_BRIGHT : "";
  const char *off = opts->colorful ? COLOR_RESET : "";
  const char *p = text ? text : "";
  if (!starts_jsonish(p)) {
    fputs(p, stdout);
    if (*p && p[strlen(p) - 1] != '\n') fputc('\n', stdout);
    return;
  }
  while (*p) {
    char c = *p;
    if (in_string) {
      fputc(c, stdout);
      if (escaped) {
        escaped = 0;
      } else if (c == '\\') {
        escaped = 1;
      } else if (c == '"') {
        fputs(off, stdout);
        in_string = 0;
      }
      p++;
      continue;
    }
    if (isspace((unsigned char)c)) {
      p++;
      continue;
    }
    if (at_line_start) {
      print_indent(indent);
      at_line_start = 0;
    }
    if (c == '"') {
      fputs(string_on, stdout);
      fputc(c, stdout);
      in_string = 1;
      p++;
    } else if (c == '{' || c == '[') {
      fputc(c, stdout);
      fputc('\n', stdout);
      indent++;
      at_line_start = 1;
      p++;
    } else if (c == '}' || c == ']') {
      fputc('\n', stdout);
      if (indent > 0) indent--;
      print_indent(indent);
      fputc(c, stdout);
      at_line_start = 0;
      p++;
    } else if (c == ',') {
      fputc(c, stdout);
      fputc('\n', stdout);
      at_line_start = 1;
      p++;
    } else if (c == ':') {
      fputs(": ", stdout);
      p++;
    } else if (isdigit((unsigned char)c) || c == '-') {
      fputs(number_on, stdout);
      while (*p && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) {
        fputc(*p++, stdout);
      }
      fputs(off, stdout);
    } else if (strncmp(p, "true", 4) == 0 || strncmp(p, "null", 4) == 0) {
      fputs(literal_on, stdout);
      fwrite(p, 1, 4, stdout);
      fputs(off, stdout);
      p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
      fputs(literal_on, stdout);
      fwrite(p, 1, 5, stdout);
      fputs(off, stdout);
      p += 5;
    } else {
      fputc(c, stdout);
      p++;
    }
  }
  fputc('\n', stdout);
}

static int next_line(const char **cursor, char *line, size_t line_len) {
  const char *p = *cursor;
  size_t n = 0;
  if (!p || !*p) return 0;
  while (p[n] && p[n] != '\n' && n + 1 < line_len) n++;
  memcpy(line, p, n);
  line[n] = '\0';
  *cursor = p + n + (p[n] == '\n' ? 1 : 0);
  return n > 0 ? 1 : (*p == '\n' ? 1 : 0);
}

static void render_event_pretty(const ViewOpts *opts, const JsonRef *ev) {
  char id[RT_SMALL] = "";
  char type[RT_SMALL] = "";
  char source[RT_SMALL] = "";
  char detail[RT_LARGE] = "";
  JsonRef payload;
  const char *col_on = "";
  const char *col_off = "";
  json_ref_object_get_string(ev, "event_id", id, sizeof(id));
  json_ref_object_get_string(ev, "type", type, sizeof(type));
  json_ref_object_get_string(ev, "source", source, sizeof(source));
  if (opts->colorful) {
    col_on = type_color(type);
    col_off = COLOR_RESET;
  }
  if (json_ref_object_get_object(ev, "payload", &payload) == 0) {
    char action[RT_SMALL] = "";
    char request_id[RT_SMALL] = "";
    char text[RT_LARGE] = "";
    if (strcmp(type, "user_message") == 0 &&
        json_ref_object_get_string(&payload, "text", text, sizeof(text)) == 0) {
      snprintf(detail, sizeof(detail), "text=%s", text);
    } else if (strcmp(type, "action_request") == 0 ||
               strcmp(type, "action_started") == 0) {
      json_ref_object_get_string(&payload, "action", action, sizeof(action));
      json_ref_object_get_string(&payload, "request_id", request_id, sizeof(request_id));
      snprintf(detail, sizeof(detail), "action=%s req=%s", action, request_id);
    } else if (strcmp(type, "action_succeeded") == 0 ||
               strcmp(type, "action_failed") == 0 ||
               strcmp(type, "action_rejected") == 0) {
      JsonRef result;
      char message[RT_LARGE] = "";
      char reason[RT_SMALL] = "";
      json_ref_object_get_string(&payload, "action", action, sizeof(action));
      json_ref_object_get_string(&payload, "request_id", request_id, sizeof(request_id));
      json_ref_object_get_string(&payload, "reason", reason, sizeof(reason));
      if (json_ref_object_get_object(&payload, "result", &result) == 0) {
        if (json_ref_object_get_string(&result, "message", message, sizeof(message)) != 0)
          json_ref_object_get_string(&result, "path", message, sizeof(message));
      }
      snprintf(detail, sizeof(detail), "action=%s req=%s%s%s%s%s",
               action, request_id,
               reason[0] ? " reason=" : "", reason,
               message[0] ? " result=" : "", message);
    } else if (strcmp(type, "user") == 0 || strcmp(type, "asst") == 0 ||
               strcmp(type, "fact") == 0 || strcmp(type, "summ") == 0) {
      char text_buf[RT_LARGE] = "";
      json_ref_object_get_string(&payload, "text", text_buf, sizeof(text_buf));
      snprintf(detail, sizeof(detail), "%s", text_buf);
    } else if (strcmp(type, "error") == 0) {
      json_ref_object_get_string(&payload, "message", text, sizeof(text));
      snprintf(detail, sizeof(detail), "%s", text);
    }
  }
  truncate_inline(detail, sizeof(detail) - 1);
  printf("%s%-12s%s %-13s %-14s %s\n", col_on, id, col_off, type, source, detail);
}

static void render_event_log(const ViewOpts *opts, const char *event_text) {
  char line[RT_XL];
  const char *cursor = event_text ? event_text : "";
  while (next_line(&cursor, line, sizeof(line))) {
    JsonRef ev;
    if (json_ref_first_object(line, &ev) != 0) continue;
    render_event_pretty(opts, &ev);
  }
}

static void render_trace(const ViewOpts *opts, const char *trace_text) {
  char line[RT_XL];
  const char *cursor = trace_text ? trace_text : "";
  const char *col_on = opts->colorful ? COLOR_BRIGHT_BLACK : "";
  const char *col_off = opts->colorful ? COLOR_RESET : "";
  if (!trace_text || !*trace_text) return;
  printf("\ntrace.jsonl (diagnostics)\n");
  while (next_line(&cursor, line, sizeof(line))) {
    JsonRef ev;
    char type[RT_SMALL] = "";
    char phase[RT_SMALL] = "";
    char detail[RT_SMALL] = "";
    char decision[RT_SMALL] = "";
    char reason[RT_SMALL] = "";
    long long pass = 0;
    if (json_ref_first_object(line, &ev) != 0) continue;
    json_ref_object_get_string(&ev, "type", type, sizeof(type));
    json_ref_object_get_string(&ev, "phase", phase, sizeof(phase));
    json_ref_object_get_string(&ev, "detail", detail, sizeof(detail));
    json_ref_object_get_string(&ev, "decision", decision, sizeof(decision));
    json_ref_object_get_string(&ev, "reason", reason, sizeof(reason));
    (void)json_ref_object_get_long(&ev, "pass", &pass);
    if (strcmp(type, "loop_decision") == 0) {
      printf("%s  loop_decision pass=%lld decision=%s reason=%s%s\n",
             col_on, pass, decision, reason, col_off);
    } else if (strcmp(type, "phase_skipped") == 0) {
      printf("%s  phase_skipped pass=%lld phase=%s reason=%s%s\n",
             col_on, pass, phase, reason, col_off);
    } else {
      printf("%s  phase=%-22s detail=%s%s\n", col_on, phase, detail, col_off);
    }
  }
}

static void list_dir(const char *path, const char *label) {
  DIR *d = opendir(path);
  struct dirent *e;
  int empty = 1;
  if (!d) return;
  printf("\n%s\n", label);
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    printf("  %s/%s\n", path, e->d_name);
    empty = 0;
  }
  closedir(d);
  if (empty) printf("  (empty)\n");
}

static long long sum_provider_meta_long(const char *run_dir, const char *key) {
  long long total = 0;
  for (int seq = 1; seq < 999; ++seq) {
    char path[PATH_MAX];
    char *text = NULL;
    JsonRef root;
    long long value = 0;
    snprintf(path, sizeof(path), "%s/provider_calls/%03d_meta.json", run_dir, seq);
    if (!fs_file_exists(path)) {
      if (seq == 1) continue;
      break;
    }
    if (fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) == 0 && text &&
        json_ref_first_object(text, &root) == 0 &&
        json_ref_object_get_long(&root, key, &value) == 0) {
      total += value;
    }
    free(text);
  }
  return total;
}

static void render_timing_summary(const ViewOpts *opts, const char *run_dir, const char *event_text) {
  char line[RT_XL];
  const char *cursor = event_text ? event_text : "";
  long long first_ts = 0;
  long long last_ts = 0;
  long long started_ms = 0;
  long long finished_ms = 0;
  int have_first = 0;
  int have_started_ms = 0;
  int have_finished_ms = 0;
  long long llm_ms = sum_provider_meta_long(run_dir, "duration_ms");
  long long input_tokens = sum_provider_meta_long(run_dir, "input_tokens");
  long long output_tokens = sum_provider_meta_long(run_dir, "output_tokens");
  long long total_tokens = sum_provider_meta_long(run_dir, "total_tokens");
  const char *col_on = opts->colorful ? COLOR_SECTION_HEADER : "";
  const char *col_off = opts->colorful ? COLOR_RESET : "";
  while (next_line(&cursor, line, sizeof(line))) {
    JsonRef ev;
    JsonRef payload;
    char ts[RT_SMALL] = "";
    char type[RT_SMALL] = "";
    long long parsed = 0;
    if (json_ref_first_object(line, &ev) != 0) continue;
    (void)json_ref_object_get_string(&ev, "type", type, sizeof(type));
    if (json_ref_object_get_object(&ev, "payload", &payload) == 0) {
      long long mono = 0;
      if (json_ref_object_get_long(&payload, "monotonic_ms", &mono) == 0) {
        if (strcmp(type, "run_started") == 0) {
          started_ms = mono;
          have_started_ms = 1;
        } else if (strcmp(type, "run_done") == 0 ||
                   strcmp(type, "run_failed") == 0 ||
                   strcmp(type, "run_canceled") == 0) {
          finished_ms = mono;
          have_finished_ms = 1;
        }
      }
    }
    if (json_ref_object_get_string(&ev, "ts", ts, sizeof(ts)) != 0) continue;
    if (parse_iso_utc_seconds(ts, &parsed) != 0) continue;
    if (!have_first) { first_ts = parsed; have_first = 1; }
    last_ts = parsed;
  }
  printf("\n%stiming summary%s\n", col_on, col_off);
  if (have_started_ms && have_finished_ms && finished_ms >= started_ms) {
    printf("  whole path:        %lld ms\n", finished_ms - started_ms);
  } else if (have_first) {
    printf("  whole path:        %lld ms (event timestamp resolution: 1s)\n", (last_ts - first_ts) * 1000LL);
  } else {
    printf("  whole path:        unavailable\n");
  }
  printf("  llm calls:         %lld ms total\n", llm_ms);
  if (input_tokens || output_tokens || total_tokens) {
    printf("  tokens:            input=%lld output=%lld total=%lld\n",
           input_tokens, output_tokens, total_tokens);
  } else {
    printf("  tokens:            unavailable from provider response\n");
  }
}

static void render_one_provider_call(const ViewOpts *opts, const char *run_dir, int seq) {
  char meta_path[PATH_MAX], raw_req_path[PATH_MAX], raw_resp_path[PATH_MAX], req_path[PATH_MAX], resp_path[PATH_MAX];
  char *meta = NULL, *raw_req = NULL, *raw_resp = NULL;
  JsonRef root;
  char agent[RT_SMALL] = "";
  char provider[RT_SMALL] = "";
  char model[RT_SMALL] = "";
  long long duration_ms = 0, input_tokens = 0, output_tokens = 0, total_tokens = 0;
  long long input_media_count = 0, input_media_bytes = 0;
  const char *hdr_on = opts->colorful ? COLOR_SECTION_HEADER : "";
  const char *off = opts->colorful ? COLOR_RESET : "";
  snprintf(meta_path, sizeof(meta_path), "%s/provider_calls/%03d_meta.json", run_dir, seq);
  snprintf(raw_req_path, sizeof(raw_req_path), "%s/provider_calls/%03d_raw_request.json", run_dir, seq);
  snprintf(raw_resp_path, sizeof(raw_resp_path), "%s/provider_calls/%03d_raw_response.json", run_dir, seq);
  snprintf(req_path, sizeof(req_path), "%s/provider_calls/%03d_request.json", run_dir, seq);
  snprintf(resp_path, sizeof(resp_path), "%s/provider_calls/%03d_response.json", run_dir, seq);
  if (fs_read_text(meta_path, &meta, FS_READ_TEXT_DEFAULT_CAP) == 0 && meta &&
      json_ref_first_object(meta, &root) == 0) {
    (void)json_ref_object_get_string(&root, "agent", agent, sizeof(agent));
    (void)json_ref_object_get_string(&root, "provider", provider, sizeof(provider));
    (void)json_ref_object_get_string(&root, "model", model, sizeof(model));
    (void)json_ref_object_get_long(&root, "duration_ms", &duration_ms);
    (void)json_ref_object_get_long(&root, "input_tokens", &input_tokens);
    (void)json_ref_object_get_long(&root, "output_tokens", &output_tokens);
    (void)json_ref_object_get_long(&root, "total_tokens", &total_tokens);
    (void)json_ref_object_get_long(&root, "input_media_count",
                                   &input_media_count);
    (void)json_ref_object_get_long(&root, "input_media_bytes",
                                   &input_media_bytes);
  }
  printf("\n%sprovider call %03d%s agent=%s provider=%s model=%s duration=%lldms",
         hdr_on, seq, off, agent[0] ? agent : "?", provider[0] ? provider : "?",
         model[0] ? model : "?", duration_ms);
  if (input_tokens || output_tokens || total_tokens) {
    printf(" tokens=input:%lld output:%lld total:%lld", input_tokens, output_tokens, total_tokens);
  }
  if (input_media_count > 0) {
    printf(" media=count:%lld bytes:%lld",
           input_media_count, input_media_bytes);
  }
  printf("\n");

  if (input_media_count > 0) {
    if (fs_read_text(req_path, &raw_req, FS_READ_TEXT_DEFAULT_CAP) == 0 &&
        raw_req) {
      printf("  %slogical model input%s (%s)\n",
             hdr_on, off, req_path);
      render_jsonish(opts, raw_req);
      free(raw_req);
      raw_req = NULL;
    }
    if (fs_file_exists(raw_req_path)) {
      printf("  %sraw request to server%s (%s; base64 media withheld, "
             "inspect the artifact explicitly)\n",
             hdr_on, off, raw_req_path);
    }
  } else {
    if (fs_read_text(raw_req_path, &raw_req, FS_READ_TEXT_DEFAULT_CAP) != 0 ||
        !raw_req)
      (void)fs_read_text(req_path, &raw_req, FS_READ_TEXT_DEFAULT_CAP);
    if (raw_req) {
      printf("  %sraw request to server%s (%s)\n", hdr_on, off,
             fs_file_exists(raw_req_path) ? raw_req_path : req_path);
      render_jsonish(opts, raw_req);
    }
  }
  if (fs_read_text(raw_resp_path, &raw_resp, FS_READ_TEXT_DEFAULT_CAP) != 0 || !raw_resp)
    (void)fs_read_text(resp_path, &raw_resp, FS_READ_TEXT_DEFAULT_CAP);
  if (raw_resp) {
    printf("  %sraw response from llm%s (%s)\n", hdr_on, off, fs_file_exists(raw_resp_path) ? raw_resp_path : resp_path);
    render_jsonish(opts, raw_resp);
  }
  free(meta);
  free(raw_req);
  free(raw_resp);
}

static void render_provider_calls(const ViewOpts *opts, const char *run_dir) {
  char dir[PATH_MAX];
  int any = 0;
  snprintf(dir, sizeof(dir), "%s/provider_calls", run_dir);
  if (!fs_file_exists(dir)) return;
  for (int seq = 1; seq < 999; ++seq) {
    char meta_path[PATH_MAX], req_path[PATH_MAX], resp_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/%03d_meta.json", dir, seq);
    snprintf(req_path, sizeof(req_path), "%s/%03d_request.json", dir, seq);
    snprintf(resp_path, sizeof(resp_path), "%s/%03d_response.json", dir, seq);
    if (!fs_file_exists(meta_path) && !fs_file_exists(req_path) && !fs_file_exists(resp_path)) {
      if (seq == 1) continue;
      break;
    }
    if (!any) {
      printf("\nprovider_calls/ (raw llm traffic)\n");
      any = 1;
    }
    render_one_provider_call(opts, run_dir, seq);
  }
}

static int safe_artifact_id(const char *value) {
  const unsigned char *p = (const unsigned char *)(value ? value : "");
  if (!*p) return 0;
  for (; *p; ++p) {
    if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return 0;
  }
  return 1;
}

/* A recovered action may relaunch the same request at a higher call sequence.
 * Only the final launch can emit its terminal, so its stderr is the highest
 * numbered artifact whose input envelope names that exact request. */
static int read_action_stderr(const char *run_dir, const char *request_id,
                              char *relative_path, size_t relative_path_len,
                              char **out) {
  char latest[PATH_MAX] = "";
  char latest_relative[PATH_MAX] = "";
  if (!run_dir || !safe_artifact_id(request_id) || !relative_path ||
      relative_path_len == 0 || !out) return 0;
  *out = NULL;
  relative_path[0] = '\0';
  for (int seq = 1; seq < 1000; ++seq) {
    char input_path[PATH_MAX];
    char stderr_path[PATH_MAX];
    char *input = NULL;
    JsonRef envelope;
    char artifact_request_id[RT_MED] = "";
    snprintf(input_path, sizeof(input_path),
             "%s/action_calls/%03d_%s.input.json", run_dir, seq, request_id);
    if (!fs_file_exists(input_path)) continue;
    if (fs_read_text(input_path, &input, FS_READ_TEXT_DEFAULT_CAP) != 0 ||
        !input || json_ref_first_object(input, &envelope) != 0 ||
        json_ref_object_get_string(&envelope, "request_id", artifact_request_id,
                                   sizeof(artifact_request_id)) != 0 ||
        strcmp(artifact_request_id, request_id) != 0) {
      free(input);
      continue;
    }
    free(input);
    snprintf(stderr_path, sizeof(stderr_path),
             "%s/action_calls/%03d_%s.stderr", run_dir, seq, request_id);
    if (!fs_file_exists(stderr_path)) continue;
    snprintf(latest, sizeof(latest), "%s", stderr_path);
    snprintf(latest_relative, sizeof(latest_relative),
             "action_calls/%03d_%s.stderr", seq, request_id);
  }
  if (!latest[0] || fs_read_text(latest, out, FS_READ_TEXT_DEFAULT_CAP) != 0 ||
      !*out || !(*out)[0]) {
    free(*out);
    *out = NULL;
    return 0;
  }
  snprintf(relative_path, relative_path_len, "%s", latest_relative);
  return 1;
}

/* Walk the event log; show the complete payload for failure-like terminals.
 * A subprocess action failure is correlated to its request-qualified stderr.
 * Rejections and runtime-intrinsic failures legitimately have no artifact. */
static void render_debug(const char *run_dir, const char *event_text) {
  char line[RT_XL];
  const char *cursor = event_text ? event_text : "";
  int errors_found = 0;
  while (next_line(&cursor, line, sizeof(line))) {
    JsonRef ev, payload;
    char type[RT_SMALL] = "";
    char source[RT_SMALL] = "";
    if (json_ref_first_object(line, &ev) != 0) continue;
    json_ref_object_get_string(&ev, "type", type, sizeof(type));
    json_ref_object_get_string(&ev, "source", source, sizeof(source));
    if (strcmp(type, "error") != 0 && strcmp(type, "run_failed") != 0 &&
        strcmp(type, "action_failed") != 0 &&
        strcmp(type, "action_rejected") != 0) continue;
    if (!errors_found) printf("\nerrors\n");
    errors_found = 1;
    if (json_ref_object_get_object(&ev, "payload", &payload) == 0) {
      char raw[RT_LARGE];
      if (json_ref_value_copy(&payload, raw, sizeof(raw)) == 0)
        printf("  %-13s %s %s\n", type, source, raw);
    }
    if (strcmp(type, "action_failed") == 0 &&
        json_ref_object_get_object(&ev, "payload", &payload) == 0) {
      char request_id[RT_MED] = "";
      char artifact_path[PATH_MAX] = "";
      char *err_text = NULL;
      if (json_ref_object_get_string(&payload, "request_id", request_id,
                                     sizeof(request_id)) == 0 &&
          read_action_stderr(run_dir, request_id, artifact_path,
                             sizeof(artifact_path), &err_text)) {
        printf("  --- %s ---\n%s", artifact_path, err_text);
        if (err_text[strlen(err_text) - 1] != '\n') fputc('\n', stdout);
      }
      free(err_text);
    }
    /* Agent failures carry their exact run-relative stderr artifact on the
     * error event — resolve it directly, never by guessing among attempts. */
    if (strcmp(type, "error") == 0 &&
        json_ref_object_get_object(&ev, "payload", &payload) == 0) {
      char artifact[RT_MED] = "";
      if (json_ref_object_get_string(&payload, "artifact", artifact,
                                     sizeof(artifact)) == 0 &&
          artifact[0] && !strstr(artifact, "..")) {
        char full[PATH_MAX];
        char *err_text = NULL;
        snprintf(full, sizeof(full), "%s/%s", run_dir, artifact);
        if (fs_read_text(full, &err_text, FS_READ_TEXT_DEFAULT_CAP) == 0 &&
            err_text && err_text[0]) {
          printf("  --- %s ---\n%s", artifact, err_text);
          if (err_text[strlen(err_text) - 1] != '\n') fputc('\n', stdout);
        }
        free(err_text);
      }
    }
  }
  if (!errors_found) printf("\nerrors: (none)\n");
}

static void render_agent_jsonl(const char *event_text, const char *trace_text) {
  char line[RT_XL];
  const char *cursor;
  cursor = event_text ? event_text : "";
  while (next_line(&cursor, line, sizeof(line))) {
    JsonRef ev;
    if (json_ref_first_object(line, &ev) != 0) continue;
    printf("%s\n", line);
  }
  cursor = trace_text ? trace_text : "";
  while (next_line(&cursor, line, sizeof(line))) {
    JsonRef ev;
    char type[RT_SMALL] = "";
    char phase[RT_SMALL] = "";
    char detail[RT_SMALL] = "";
    char ep[RT_SMALL];
    char ed[RT_SMALL];
    if (json_ref_first_object(line, &ev) != 0) continue;
    json_ref_object_get_string(&ev, "type", type, sizeof(type));
    if (type[0]) {
      printf("%s\n", line);
      continue;
    }
    json_ref_object_get_string(&ev, "phase", phase, sizeof(phase));
    json_ref_object_get_string(&ev, "detail", detail, sizeof(detail));
    json_escape(phase, ep, sizeof(ep));
    json_escape(detail, ed, sizeof(ed));
    printf("{\"type\":\"trace_note\",\"phase\":\"%s\",\"detail\":\"%s\"}\n", ep, ed);
  }
}

int rt_view_run(const char *run_arg, const ViewOpts *opts);

int rt_view_run(const char *run_arg, const ViewOpts *opts) {
  char run_dir[PATH_MAX];
  char event_path[PATH_MAX];
  char trace_path[PATH_MAX];
  char state_path[PATH_MAX];
  char *event_text = NULL;
  char *trace_text = NULL;
  char *state_text = NULL;
  if (!run_arg || !*run_arg) {
    fprintf(stderr, "view: missing run id\n");
    if (opts && opts->agent_readable)
      fc_cli_print_json_error("view", "missing_run_id", "missing run id");
    return -1;
  }
  if (strchr(run_arg, '/')) {
    snprintf(run_dir, sizeof(run_dir), "%s", run_arg);
  } else {
    snprintf(run_dir, sizeof(run_dir), "workspace/runs/%s", run_arg);
  }
  if (!fs_file_exists(run_dir)) {
    fprintf(stderr, "view: run not found: %s\n", run_dir);
    if (opts->agent_readable)
      fc_cli_print_json_error("view", "run_not_found", run_dir);
    return -1;
  }
  snprintf(event_path, sizeof(event_path), "%s/event_log.jsonl", run_dir);
  snprintf(trace_path, sizeof(trace_path), "%s/trace.jsonl", run_dir);
  snprintf(state_path, sizeof(state_path), "%s/state.json", run_dir);
  if (read_file_or_warn(event_path, &event_text, 1) != 0) return -1;
  (void)read_file_or_warn(trace_path, &trace_text, 0);
  (void)read_file_or_warn(state_path, &state_text, 0);
  if (opts->agent_readable) {
    render_agent_jsonl(event_text, trace_text);
  } else {
    char agent_dir[PATH_MAX];
    char action_dir[PATH_MAX];
    printf("run: %s\n", run_dir);
    printf("event_log.jsonl (truth)\n");
    render_event_log(opts, event_text);
    if (opts->debug) render_debug(run_dir, event_text);
    if (opts->full) {
      render_trace(opts, trace_text);
      render_provider_calls(opts, run_dir);
      snprintf(agent_dir, sizeof(agent_dir), "%s/agent_outputs", run_dir);
      snprintf(action_dir, sizeof(action_dir), "%s/action_calls", run_dir);
      list_dir(agent_dir, "agent_outputs/");
      list_dir(action_dir, "action_calls/");
      if (state_text) {
        printf("\nstate.json\n%s\n", state_text);
      }
    }
    render_timing_summary(opts, run_dir, event_text);
  }
  free(event_text);
  free(trace_text);
  free(state_text);
  return 0;
}

int rt_view_main(int argc, char **argv) {
  ViewOpts opts;
  const char *run_arg = NULL;
  FcCliOutputMode mode;
  memset(&opts, 0, sizeof(opts));
  fc_cli_output_mode_init(&mode);
  opts.agent_readable = 1;
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw view");
    if (mode_rc < 0) {
      fc_cli_print_json_error("view", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return -1;
    }
    if (mode_rc > 0) {
      opts.agent_readable = !mode.human;
      opts.colorful = mode.human;
    } else if (strcmp(argv[i], "-f") == 0) opts.full = 1;
    else if (strcmp(argv[i], "-d") == 0) opts.debug = 1;
    else if (argv[i][0] == '-') {
      fprintf(stderr, "view: unknown flag %s\n", argv[i]);
      return -1;
    } else if (!run_arg) {
      run_arg = argv[i];
    } else {
      fprintf(stderr, "view: too many arguments\n");
      return -1;
    }
  }
  if (!run_arg) {
    fprintf(stderr, "view: missing run id\n");
    if (opts.agent_readable)
      fc_cli_print_json_error("view", "missing_run_id", "missing run id");
    return -1;
  }
  return rt_view_run(run_arg, &opts);
}
