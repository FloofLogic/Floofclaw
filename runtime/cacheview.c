#include "runtime.h"
#include "support/cli_output.h"
#include "llm/llm.h"
#include "support/color.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CV_MAX_REQ 10
#define CV_REQ_CAP (LLM_PROMPT_MAX + 1024)
#define CV_HEAT_BUCKETS 64

typedef struct {
  char run_id[RT_SMALL];
  char raw_path[PATH_MAX];
  char profile[RT_SMALL];
  char provider[RT_SMALL];
  char model[RT_SMALL];
  char url[RT_MED];
  int seq;
  long long order;
  long long input_tokens;
  long long provider_cached_input_tokens;
  int provider_cached_input_tokens_reported;
  size_t len;
} CvReq;

typedef struct {
  const char *agent;
  const char *run_id;
  int call_seq;
  int call_seq_set;
  int limit;
  int color;
  int agent_readable;
  int list;
} CvOpts;

typedef struct {
  size_t start;
  size_t end;
  double match_pct;
} CvHeatBucket;

static char g_body[CV_MAX_REQ][CV_REQ_CAP];

static int read_fixed(const char *path, char *out, size_t out_len,
                      size_t *len_out) {
  FILE *f;
  long n;
  if (len_out) *len_out = 0;
  if (!path || !out || out_len == 0) return -1;
  f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
      (size_t)n >= out_len || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  if (n > 0 && fread(out, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    return -1;
  }
  fclose(f);
  out[n] = '\0';
  if (len_out) *len_out = (size_t)n;
  return 0;
}

static int parse_limit(const char *value, int *out) {
  char *end = NULL;
  long parsed;
  if (!value || !*value || !out) return -1;
  parsed = strtol(value, &end, 10);
  if (!end || *end || parsed < 2 || parsed > CV_MAX_REQ) return -1;
  *out = (int)parsed;
  return 0;
}

static int parse_call_seq(const char *value, int *out) {
  char *end = NULL;
  long parsed;
  if (!value || !*value || !out) return -1;
  parsed = strtol(value, &end, 10);
  if (!end || *end || parsed < 1 || parsed > 998) return -1;
  *out = (int)parsed;
  return 0;
}

static void keep_latest(CvReq *reqs, int *count, int limit,
                        const CvReq *candidate) {
  int slot = *count;
  if (*count < limit) {
    (*count)++;
  } else {
    long long min_order = reqs[0].order;
    slot = 0;
    for (int i = 1; i < *count; ++i) {
      if (reqs[i].order < min_order) {
        min_order = reqs[i].order;
        slot = i;
      }
    }
    if (candidate->order <= min_order) return;
  }
  reqs[slot] = *candidate;
}

static int request_is_selected(const CvOpts *opts, const CvReq *req) {
  if (!opts->run_id) return 0;
  if (strcmp(opts->run_id, req->run_id) != 0) return 0;
  return !opts->call_seq_set || opts->call_seq == req->seq;
}

static int scan_agent(const CvOpts *opts, CvReq *reqs, int *count,
                      int *skipped, int *media_excluded, CvReq *selected,
                      int *selected_matches) {
  DIR *runs = opendir("workspace/runs");
  struct dirent *entry;
  char meta[RT_LARGE];
  if (!runs) return -1;
  *count = 0;
  *skipped = 0;
  *media_excluded = 0;
  *selected_matches = 0;
  memset(selected, 0, sizeof(*selected));
  while ((entry = readdir(runs)) != NULL) {
    int run_no = 0;
    if (sscanf(entry->d_name, "run_%d", &run_no) != 1) continue;
    for (int seq = 1; seq < 999; ++seq) {
      char meta_path[PATH_MAX], raw_path[PATH_MAX];
      JsonRef root, cached;
      CvReq req;
      char meta_agent[RT_SMALL] = "";
      char meta_floop[RT_SMALL] = "";
      long long input_media_count = 0;
      snprintf(meta_path, sizeof(meta_path),
               "workspace/runs/%s/provider_calls/%03d_meta.json",
               entry->d_name, seq);
      if (!fs_file_exists(meta_path)) {
        if (seq > 1) break;
        continue;
      }
      if (read_fixed(meta_path, meta, sizeof(meta), NULL) != 0 ||
          json_ref_first_object(meta, &root) != 0 ||
          json_ref_object_get_string(&root, "agent", meta_agent,
                                     sizeof(meta_agent)) != 0 ||
          strcmp(meta_agent, opts->agent) != 0 ||
          json_ref_object_get_string(&root, "floop", meta_floop,
                                     sizeof(meta_floop)) != 0 ||
          !meta_floop[0])
        continue;

      memset(&req, 0, sizeof(req));
      snprintf(raw_path, sizeof(raw_path),
               "workspace/runs/%s/provider_calls/%03d_raw_request.json",
               entry->d_name, seq);
      snprintf(req.run_id, sizeof(req.run_id), "%s", entry->d_name);
      snprintf(req.raw_path, sizeof(req.raw_path), "%s", raw_path);
      if (json_ref_object_get_string(&root, "profile", req.profile,
                                     sizeof(req.profile)) != 0 ||
          json_ref_object_get_string(&root, "provider", req.provider,
                                     sizeof(req.provider)) != 0 ||
          json_ref_object_get_string(&root, "model", req.model,
                                     sizeof(req.model)) != 0 ||
          json_ref_object_get_string(&root, "url", req.url,
                                     sizeof(req.url)) != 0 ||
          json_ref_object_get_long(&root, "input_media_count",
                                   &input_media_count) != 0 ||
          json_ref_object_get_long(&root, "input_tokens",
                                   &req.input_tokens) != 0 ||
          json_ref_object_get(&root, "provider_cached_input_tokens",
                              &cached) != 0)
        continue;
      if (!req.profile[0] || !req.provider[0] || !req.model[0] ||
          !req.url[0] || input_media_count < 0 || req.input_tokens < 0)
        continue;
      if (input_media_count > 0) {
        (*media_excluded)++;
        continue;
      }
      if (!json_ref_is_null(&cached)) {
        if (json_ref_get_long(&cached,
                              &req.provider_cached_input_tokens) != 0 ||
            req.provider_cached_input_tokens < 0)
          continue;
        req.provider_cached_input_tokens_reported = 1;
      }
      req.seq = seq;
      req.order = (long long)run_no * 1000LL + seq;
      if (read_fixed(raw_path, g_body[0], sizeof(g_body[0]), &req.len) != 0) {
        (*skipped)++;
        continue;
      }
      keep_latest(reqs, count, opts->limit, &req);
      if (request_is_selected(opts, &req)) {
        *selected = req;
        (*selected_matches)++;
      }
    }
  }
  closedir(runs);
  return 0;
}

static void sort_latest(CvReq *reqs, int count) {
  for (int i = 0; i < count; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (reqs[j].order > reqs[i].order) {
        CvReq tmp = reqs[i];
        reqs[i] = reqs[j];
        reqs[j] = tmp;
      }
    }
  }
}

static int find_request(const CvReq *reqs, int count, const CvReq *wanted) {
  for (int i = 0; i < count; ++i)
    if (reqs[i].seq == wanted->seq &&
        strcmp(reqs[i].run_id, wanted->run_id) == 0)
      return i;
  return -1;
}

static void make_selected_baseline(CvReq *reqs, int count,
                                   const CvReq *selected) {
  int index = find_request(reqs, count, selected);
  if (index < 0 && count > 0) {
    reqs[count - 1] = *selected;
    index = count - 1;
  }
  if (index > 0) {
    CvReq tmp = reqs[0];
    reqs[0] = reqs[index];
    reqs[index] = tmp;
  }
}

static size_t shared_prefix(char bodies[][CV_REQ_CAP], const CvReq *reqs,
                            int count) {
  size_t n = reqs[0].len;
  for (int i = 1; i < count; ++i)
    if (reqs[i].len < n) n = reqs[i].len;
  for (size_t pos = 0; pos < n; ++pos)
    for (int i = 1; i < count; ++i)
      if (bodies[i][pos] != bodies[0][pos]) return pos;
  return n;
}

static int build_heatmap(char bodies[][CV_REQ_CAP], const CvReq *reqs,
                         int count, CvHeatBucket *out, int out_cap) {
  size_t baseline_len;
  int bucket_count;
  if (!out || out_cap <= 0 || !reqs || count <= 0) return 0;
  baseline_len = reqs[0].len;
  if (baseline_len == 0) return 0;
  bucket_count = baseline_len < (size_t)out_cap ? (int)baseline_len : out_cap;
  for (int bucket = 0; bucket < bucket_count; ++bucket) {
    size_t start = baseline_len * (size_t)bucket / (size_t)bucket_count;
    size_t end = baseline_len * (size_t)(bucket + 1) /
                 (size_t)bucket_count;
    size_t matches = 0;
    size_t possible = (end - start) * (size_t)count;
    for (int i = 0; i < count; ++i) {
      for (size_t pos = start; pos < end; ++pos) {
        if (pos < reqs[i].len && bodies[i][pos] == bodies[0][pos])
          matches++;
      }
    }
    out[bucket].start = start;
    out[bucket].end = end;
    out[bucket].match_pct = possible
        ? 100.0 * (double)matches / (double)possible : 0.0;
  }
  return bucket_count;
}

static void print_span(const char *text, size_t len, const char *color) {
  if (color && *color) fputs(color, stdout);
  if (len > 0) fwrite(text, 1, len, stdout);
  if (color && *color) fputs(COLOR_RESET, stdout);
  if (len == 0 || text[len - 1] != '\n') putchar('\n');
}

static void print_json_string_n(const char *text, size_t len) {
  putchar('"');
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)text[i];
    switch (c) {
      case '"': fputs("\\\"", stdout); break;
      case '\\': fputs("\\\\", stdout); break;
      case '\b': fputs("\\b", stdout); break;
      case '\f': fputs("\\f", stdout); break;
      case '\n': fputs("\\n", stdout); break;
      case '\r': fputs("\\r", stdout); break;
      case '\t': fputs("\\t", stdout); break;
      default:
        if (c < 0x20) printf("\\u%04x", c);
        else putchar((int)c);
    }
  }
  putchar('"');
}

static void print_json_string(const char *text) {
  print_json_string_n(text ? text : "", strlen(text ? text : ""));
}

static void render_agent_json(const CvOpts *opts, const CvReq *reqs,
                              int count, int skipped, int media_excluded,
                              size_t prefix,
                              const CvHeatBucket *heat, int heat_count) {
  const char *mode = opts->list ? "list" : (opts->run_id ? "run" : "heatmap");
  fputs("{\"schema_version\":1,\"mode\":", stdout);
  print_json_string(mode);
  fputs(",\"selection\":{\"agent\":", stdout);
  print_json_string(opts->agent);
  printf(",\"limit\":%d,\"run_id\":", opts->limit);
  if (opts->run_id) print_json_string(opts->run_id); else fputs("null", stdout);
  printf(",\"call_seq\":");
  if (opts->call_seq_set) printf("%d", opts->call_seq); else fputs("null", stdout);
  fputs("},\"calls\":[", stdout);
  for (int i = 0; i < count; ++i) {
    if (i) putchar(',');
    fputs("{\"run_id\":", stdout); print_json_string(reqs[i].run_id);
    printf(",\"call_seq\":%d,\"profile\":", reqs[i].seq);
    print_json_string(reqs[i].profile);
    fputs(",\"provider\":", stdout); print_json_string(reqs[i].provider);
    fputs(",\"model\":", stdout); print_json_string(reqs[i].model);
    fputs(",\"url\":", stdout); print_json_string(reqs[i].url);
    printf(",\"bytes\":%zu,\"input_tokens\":%lld,"
           "\"provider_cached_input_tokens\":",
           reqs[i].len, reqs[i].input_tokens);
    if (reqs[i].provider_cached_input_tokens_reported)
      printf("%lld", reqs[i].provider_cached_input_tokens);
    else
      fputs("null", stdout);
    fputs(",\"raw_request\":", stdout);
    print_json_string_n(g_body[i], reqs[i].len);
    putchar('}');
  }
  printf("],\"shared_prefix\":{\"bytes\":%zu,\"percent\":%.1f},"
         "\"heatmap\":[", prefix,
         reqs[0].len ? 100.0 * (double)prefix / (double)reqs[0].len : 0.0);
  for (int i = 0; i < heat_count; ++i) {
    if (i) putchar(',');
    printf("{\"start\":%zu,\"end\":%zu,\"match_percent\":%.1f}",
           heat[i].start, heat[i].end, heat[i].match_pct);
  }
  printf("],\"summary\":{\"requests\":%d,\"baseline_run_id\":",
         count);
  print_json_string(reqs[0].run_id);
  printf(",\"baseline_call_seq\":%d,\"baseline_bytes\":%zu,"
         "\"skipped\":%d,\"media_requests_excluded\":%d}}\n",
         reqs[0].seq, reqs[0].len, skipped, media_excluded);
}

static char heat_char(double pct) {
  if (pct >= 99.5) return '#';
  if (pct >= 75.0) return 'O';
  if (pct >= 50.0) return 'o';
  if (pct >= 25.0) return ':';
  return '.';
}

static const char *heat_color(double pct) {
  if (pct >= 99.5) return COLOR_GREEN_BRIGHT;
  if (pct >= 75.0) return COLOR_CYAN;
  if (pct >= 50.0) return COLOR_YELLOW;
  if (pct >= 25.0) return COLOR_ORANGE;
  return COLOR_RED_BRIGHT;
}

static void render_heatmap(const CvOpts *opts, const CvHeatBucket *heat,
                           int heat_count) {
  printf("\n=== aggregate request heatmap ===\n");
  for (int i = 0; i < heat_count; ++i) {
    if (opts->color) fputs(heat_color(heat[i].match_pct), stdout);
    putchar(heat_char(heat[i].match_pct));
    if (opts->color) fputs(COLOR_RESET, stdout);
  }
  putchar('\n');
  printf("# 100%% match   O 75-99%%   o 50-74%%   : 25-49%%   . 0-24%%\n");
  if (heat_count > 0)
    printf("byte 0%*sbyte %zu\n", heat_count > 14 ? heat_count - 14 : 1,
           "", heat[heat_count - 1].end);
}

static void render_summary(const CvOpts *opts, const CvReq *reqs, int count,
                           int skipped, int media_excluded, size_t prefix) {
  const char *key = opts->color ? COLOR_CYAN : "";
  const char *off = opts->color ? COLOR_RESET : "";
  printf("\n=== summary ===\n");
  printf("%sagent:%s %s\n%sprofile:%s %s\n%sprovider:%s %s\n"
         "%smodel:%s %s\n%surl:%s %s\n",
         key, off, opts->agent, key, off, reqs[0].profile, key, off,
         reqs[0].provider, key, off, reqs[0].model, key, off, reqs[0].url);
  printf("%srequests:%s %d\n%sbaseline:%s %s/%03d\n%sskipped:%s %d\n",
         key, off, count, key, off, reqs[0].run_id, reqs[0].seq,
         key, off, skipped);
  printf("%smedia_requests_excluded:%s %d\n", key, off, media_excluded);
  printf("%sshared_prefix_bytes:%s %zu\n%sbaseline_bytes:%s %zu\n"
         "%sshared_prefix_pct:%s %.1f\n",
         key, off, prefix, key, off, reqs[0].len, key, off,
         reqs[0].len ? 100.0 * (double)prefix / (double)reqs[0].len : 0.0);
  printf("%sprovider_cached_input_tokens:%s ", key, off);
  if (reqs[0].provider_cached_input_tokens_reported)
    printf("%lld\n", reqs[0].provider_cached_input_tokens);
  else
    printf("unavailable\n");
}

static void cacheview_usage(void) {
  fprintf(stderr,
          "usage: fclaw cacheview [-a|-h] --agent <agent> [-n 2..10] "
          "[--run <run_id> [--call <seq>] | --list]\n");
}

static int parse_opts(int argc, char **argv, CvOpts *opts) {
  FcCliOutputMode mode;
  memset(opts, 0, sizeof(*opts));
  opts->limit = 4;
  opts->agent_readable = 1;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw cacheview");
    if (mode_rc < 0) {
      fc_cli_print_json_error("cacheview", "output_mode_conflict",
                              "choose exactly one output mode: -a or -h");
      return -1;
    }
    if (mode_rc > 0) {
      opts->agent_readable = !mode.human;
      opts->color = mode.human;
    } else if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc && argv[i + 1][0] != '-')
      opts->agent = argv[++i];
    else if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-list") == 0)
      opts->list = 1;
    else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc &&
             parse_limit(argv[++i], &opts->limit) == 0) {
      /* parsed */
    } else if (strcmp(argv[i], "--run") == 0 && i + 1 < argc &&
               argv[i + 1][0] != '-') {
      opts->run_id = argv[++i];
    } else if (strcmp(argv[i], "--call") == 0 && i + 1 < argc &&
               parse_call_seq(argv[++i], &opts->call_seq) == 0) {
      opts->call_seq_set = 1;
    } else {
      cacheview_usage();
      return -1;
    }
  }
  if (!opts->agent || !*opts->agent) {
    fprintf(stderr, "cacheview requires --agent <agent>\n");
    if (opts->agent_readable)
      fc_cli_print_json_error("cacheview", "missing_agent",
                              "cacheview requires --agent <agent>");
    return -1;
  }
  if (opts->list && opts->run_id) {
    fprintf(stderr, "cacheview: choose --run or --list, not both\n");
    return -1;
  }
  if (opts->call_seq_set && !opts->run_id) {
    fprintf(stderr, "cacheview: --call requires --run <run_id>\n");
    return -1;
  }
  if (opts->agent_readable) opts->color = 0;
  return 0;
}

int rt_cacheview_main(int argc, char **argv) {
  CvOpts opts;
  CvReq reqs[CV_MAX_REQ], selected;
  CvHeatBucket heat[CV_HEAT_BUCKETS];
  int count = 0, skipped = 0, media_excluded = 0;
  int selected_matches = 0, heat_count;
  size_t prefix;
  if (parse_opts(argc, argv, &opts) != 0) return 1;
  if (scan_agent(&opts, reqs, &count, &skipped, &media_excluded, &selected,
                 &selected_matches) != 0 || count < 2) {
    fprintf(stderr,
            "cacheview requires at least 2 text-only raw requests for agent "
            "'%s' (media requests excluded: %d)\n",
            opts.agent, media_excluded);
    return 1;
  }
  if (opts.run_id) {
    if (selected_matches == 0) {
      fprintf(stderr, "cacheview: no call for agent '%s' in run '%s'\n",
              opts.agent, opts.run_id);
      return 1;
    }
    if (!opts.call_seq_set && selected_matches > 1) {
      fprintf(stderr,
              "cacheview: run '%s' has multiple calls for agent '%s'; "
              "add --call <seq>\n", opts.run_id, opts.agent);
      return 1;
    }
  }
  sort_latest(reqs, count);
  if (opts.run_id) make_selected_baseline(reqs, count, &selected);
  for (int i = 0; i < count; ++i) {
    if (read_fixed(reqs[i].raw_path, g_body[i], sizeof(g_body[i]),
                   &reqs[i].len) != 0)
      return 1;
  }
  prefix = shared_prefix(g_body, reqs, count);
  heat_count = build_heatmap(g_body, reqs, count, heat, CV_HEAT_BUCKETS);
  if (opts.agent_readable) {
    render_agent_json(&opts, reqs, count, skipped, media_excluded, prefix,
                      heat, heat_count);
    return 0;
  }

  printf("FloofClaw Cache View\n\n=== cached raw prefix ===\n");
  print_span(g_body[0], prefix, opts.color ? COLOR_GREEN_BRIGHT : "");
  render_heatmap(&opts, heat, heat_count);
  if (opts.run_id) {
    size_t tail = reqs[0].len > prefix ? reqs[0].len - prefix : 0;
    printf("\n=== divergence: %s/%03d after byte %zu ===\n",
           reqs[0].run_id, reqs[0].seq, prefix);
    print_span(g_body[0] + prefix, tail,
               opts.color ? COLOR_RED_BRIGHT : "");
  } else if (opts.list) {
    printf("\n=== divergent tails after byte %zu ===\n", prefix);
    for (int i = 0; i < count; ++i) {
      size_t tail = reqs[i].len > prefix ? reqs[i].len - prefix : 0;
      printf("[%d] %s/%03d tail_bytes=%zu\n", i + 1, reqs[i].run_id,
             reqs[i].seq, tail);
      print_span(g_body[i] + prefix, tail,
                 opts.color ? COLOR_RED_BRIGHT : "");
    }
  }
  render_summary(&opts, reqs, count, skipped, media_excluded, prefix);
  return 0;
}
