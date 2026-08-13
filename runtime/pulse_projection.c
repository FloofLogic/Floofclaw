#include "runtime.h"

#include "support/fsutil.h"
#include "support/json.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PULSE_MAX_AFFAIRS    AFFAIR_MAX_ITEMS
#define PULSE_MAX_TASKS      128
#define PULSE_MAX_OPERATIONS OPERATION_MAX_ITEMS
#define PULSE_MAX_BUS        256
#define PULSE_MAX_MESSAGES   80
#define PULSE_MAX_RUN_NAMES  256
#define PULSE_MAX_WARNINGS   96
#define PULSE_NOTE_KEEP      6
#define PULSE_ACTION_KEEP    16

typedef struct {
  char affair_id[RT_SMALL], context_id[RT_MED], status[RT_SMALL];
  char manage[RT_LARGE];
  long long next_review_at_ms, created_ms;
  char notes[PULSE_NOTE_KEEP][RT_MED];
  size_t note_count;
  int dormant;
} PulseAffair;

typedef struct {
  char task_id[RT_SMALL], kind[RT_SMALL], status[RT_SMALL];
  char work[RT_LARGE], done_when[RT_LARGE];
  long long work_rev, updated_ms;
  char external[RT_LARGE];
} PulseTask;

typedef struct {
  char handle[RT_SMALL], action[RT_SMALL], task_id[RT_SMALL], status[RT_SMALL];
  long long work_rev, next_poll_at_ms, poll_interval_ms;
} PulseOperation;

typedef struct {
  char event_id[RT_SMALL], kind[RT_SMALL], channel[RT_SMALL], nick[RT_SMALL];
  char text[RT_LARGE];
  long long ts_ms;
} PulseBusEvent;

typedef struct {
  char dir[8], kind[RT_SMALL], who[RT_SMALL], text[RT_LARGE];
  char event_id[RT_SMALL], run_id[RT_SMALL];
  long long ts_ms;
  int failure, proactive;
} PulseMessage;

typedef struct {
  char run_id[RT_SMALL], origin_kind[RT_SMALL], outcome[RT_SMALL];
  char reason[RT_MED];
  char actions[PULSE_ACTION_KEEP][RT_SMALL];
  size_t action_count;
  long long start_ms, end_ms;
} PulseRun;

typedef struct {
  char values[PULSE_MAX_WARNINGS][RT_MED];
  size_t count;
} PulseWarnings;

static int pulse_appendf(char **cur, size_t *left, const char *fmt, ...) {
  va_list ap;
  int n;
  if (!cur || !*cur || !left || *left == 0) return -1;
  va_start(ap, fmt);
  n = vsnprintf(*cur, *left, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= *left) return -1;
  *cur += (size_t)n;
  *left -= (size_t)n;
  return 0;
}

static int root_path(const char *root, const char *rel,
                     char *out, size_t out_len) {
  const char *base = root && *root ? root : ".";
  return snprintf(out, out_len, "%s/%s", base, rel) < (int)out_len ? 0 : -1;
}

static char *read_root_text(const char *root, const char *rel) {
  char path[PATH_MAX];
  char *text = NULL;
  if (root_path(root, rel, path, sizeof(path)) != 0 ||
      fs_read_text(path, &text, FS_READ_TEXT_DEFAULT_CAP) != 0)
    return NULL;
  return text;
}

static void add_warning(PulseWarnings *warnings, const char *fmt, ...) {
  va_list ap;
  if (!warnings || warnings->count >= PULSE_MAX_WARNINGS) return;
  va_start(ap, fmt);
  (void)vsnprintf(warnings->values[warnings->count],
                  sizeof(warnings->values[0]), fmt, ap);
  va_end(ap);
  warnings->count++;
}

static long long iso_to_ms(const char *iso) {
  struct tm tmv;
  int year, mon, day, hour, min, sec, millis = 0;
  const char *fraction;
  time_t t;
  if (!iso ||
      sscanf(iso, "%d-%d-%dT%d:%d:%d",
             &year, &mon, &day, &hour, &min, &sec) != 6)
    return 0;
  fraction = strchr(iso, '.');
  if (fraction) {
    int scale = 100;
    fraction++;
    while (*fraction >= '0' && *fraction <= '9' && scale > 0) {
      millis += (*fraction++ - '0') * scale;
      scale /= 10;
    }
  }
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = year - 1900;
  tmv.tm_mon = mon - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = min;
  tmv.tm_sec = sec;
  t = timegm(&tmv);
  return t < 0 ? 0 : (long long)t * 1000LL + millis;
}

static void wall_iso(long long now_ms, char *out, size_t out_len) {
  time_t seconds = (time_t)(now_ms / 1000LL);
  struct tm tmv;
  if (!out || out_len == 0) return;
  if (!gmtime_r(&seconds, &tmv) ||
      strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0)
    snprintf(out, out_len, "1970-01-01T00:00:00Z");
}

static int json_string_or(const JsonRef *obj, const char *key,
                          char *out, size_t out_len, const char *fallback) {
  if (json_ref_object_get_string(obj, key, out, out_len) == 0) return 0;
  snprintf(out, out_len, "%s", fallback ? fallback : "");
  return 1;
}

static size_t collect_affairs(const char *root, long long now_ms,
                              PulseAffair *out, PulseWarnings *warnings,
                              int *truncated) {
  char *text = read_root_text(root, "workspace/memory/state/affairs.json");
  JsonRef top, arr;
  size_t count = 0;
  if (!text) return 0;
  if (json_ref_top_object(text, &top) != 0 ||
      json_ref_object_get_array(&top, "affairs", &arr) != 0) {
    free(text);
    return 0;
  }
  for (size_t i = 0; i < json_ref_array_size(&arr); ++i) {
    JsonRef item, notes;
    PulseAffair *a;
    if (count >= PULSE_MAX_AFFAIRS) {
      if (truncated) *truncated = 1;
      break;
    }
    if (json_ref_array_get(&arr, i, &item) != 0 ||
        item.type != JSON_REF_OBJECT)
      continue;
    a = &out[count];
    memset(a, 0, sizeof(*a));
    (void)json_string_or(&item, "affair_id", a->affair_id,
                         sizeof(a->affair_id), "");
    (void)json_string_or(&item, "context_id", a->context_id,
                         sizeof(a->context_id), "");
    (void)json_string_or(&item, "status", a->status,
                         sizeof(a->status), "");
    (void)json_string_or(&item, "manage", a->manage,
                         sizeof(a->manage), "");
    (void)json_ref_object_get_long(&item, "next_review_at_ms",
                                   &a->next_review_at_ms);
    (void)json_ref_object_get_long(&item, "created_ms", &a->created_ms);
    if (json_ref_object_get_array(&item, "notes", &notes) == 0) {
      size_t total = json_ref_array_size(&notes);
      size_t begin = total > PULSE_NOTE_KEEP ? total - PULSE_NOTE_KEEP : 0;
      for (size_t ni = begin; ni < total; ++ni) {
        JsonRef note;
        if (json_ref_array_get(&notes, ni, &note) == 0 &&
            json_ref_object_get_string(&note, "text",
                                       a->notes[a->note_count],
                                       sizeof(a->notes[0])) == 0)
          a->note_count++;
      }
    }
    if (strcmp(a->status, "active") == 0 && !a->next_review_at_ms) {
      a->dormant = 1;
      add_warning(warnings,
                  "affair %s is active but has no next review armed (dormant)",
                  a->affair_id);
    }
    if (strcmp(a->status, "active") == 0 && a->next_review_at_ms &&
        a->next_review_at_ms < now_ms - 60000LL)
      add_warning(warnings,
                  "affair %s review is overdue and has not fired",
                  a->affair_id);
    count++;
  }
  free(text);
  /* Existing generator ordering: active first, then next review time. */
  for (size_t i = 1; i < count; ++i) {
    PulseAffair v = out[i];
    size_t j = i;
    while (j > 0) {
      int va = strcmp(v.status, "active") != 0;
      int pa = strcmp(out[j - 1].status, "active") != 0;
      long long vn = v.next_review_at_ms ? v.next_review_at_ms : LLONG_MAX;
      long long pn = out[j - 1].next_review_at_ms
                         ? out[j - 1].next_review_at_ms : LLONG_MAX;
      if (pa < va || (pa == va && pn <= vn)) break;
      out[j] = out[j - 1];
      j--;
    }
    out[j] = v;
  }
  return count;
}

static size_t collect_tasks(const char *root, PulseTask *out,
                            long long *archived, int *truncated) {
  char *text = read_root_text(root, "workspace/memory/state/tasks.json");
  JsonRef top, arr;
  size_t count = 0;
  if (archived) *archived = 0;
  if (text && json_ref_top_object(text, &top) == 0 &&
      json_ref_object_get_array(&top, "tasks", &arr) == 0) {
    for (size_t i = 0; i < json_ref_array_size(&arr); ++i) {
      JsonRef item, state, input, ext;
      PulseTask *t;
      if (count >= PULSE_MAX_TASKS) {
        if (truncated) *truncated = 1;
        break;
      }
      if (json_ref_array_get(&arr, i, &item) != 0 ||
          item.type != JSON_REF_OBJECT)
        continue;
      t = &out[count];
      memset(t, 0, sizeof(*t));
      (void)json_string_or(&item, "task_id", t->task_id,
                           sizeof(t->task_id), "");
      (void)json_string_or(&item, "kind", t->kind, sizeof(t->kind), "");
      if (json_ref_object_get_object(&item, "state", &state) == 0) {
        (void)json_string_or(&state, "status", t->status,
                             sizeof(t->status), "");
        (void)json_string_or(&state, "work", t->work,
                             sizeof(t->work), "");
        (void)json_string_or(&state, "done_when", t->done_when,
                             sizeof(t->done_when), "");
        (void)json_ref_object_get_long(&state, "work_rev", &t->work_rev);
        (void)json_ref_object_get_long(&state, "updated_ms", &t->updated_ms);
        if (json_ref_object_get(&state, "external", &ext) == 0)
          (void)json_ref_value_copy(&ext, t->external,
                                    sizeof(t->external));
      }
      if (!t->work[0] &&
          json_ref_object_get_object(&item, "input", &input) == 0)
        (void)json_ref_object_get_string(&input, "text", t->work,
                                         sizeof(t->work));
      count++;
    }
  }
  free(text);
  text = read_root_text(root,
                        "workspace/memory/state/tasks_archive.jsonl");
  if (text && archived) {
    const char *p = text;
    while (*p) {
      const char *e = strchr(p, '\n');
      size_t n = e ? (size_t)(e - p) : strlen(p);
      if (n > 0) (*archived)++;
      if (!e) break;
      p = e + 1;
    }
  }
  free(text);
  return count;
}

static size_t collect_operations(const char *root, PulseOperation *out,
                                 PulseWarnings *warnings, int *truncated) {
  char *text =
      read_root_text(root, "workspace/memory/state/operations.json");
  JsonRef top, arr;
  size_t count = 0;
  if (!text) return 0;
  if (json_ref_top_object(text, &top) != 0 ||
      json_ref_object_get_array(&top, "operations", &arr) != 0) {
    free(text);
    return 0;
  }
  for (size_t i = 0; i < json_ref_array_size(&arr); ++i) {
    JsonRef item;
    PulseOperation *o;
    if (count >= PULSE_MAX_OPERATIONS) {
      if (truncated) *truncated = 1;
      break;
    }
    if (json_ref_array_get(&arr, i, &item) != 0 ||
        item.type != JSON_REF_OBJECT)
      continue;
    o = &out[count];
    memset(o, 0, sizeof(*o));
    (void)json_string_or(&item, "handle", o->handle,
                         sizeof(o->handle), "");
    (void)json_string_or(&item, "action", o->action,
                         sizeof(o->action), "");
    (void)json_string_or(&item, "task_id", o->task_id,
                         sizeof(o->task_id), "");
    (void)json_string_or(&item, "status", o->status,
                         sizeof(o->status), "");
    (void)json_ref_object_get_long(&item, "work_rev", &o->work_rev);
    (void)json_ref_object_get_long(&item, "next_poll_at_ms",
                                   &o->next_poll_at_ms);
    (void)json_ref_object_get_long(&item, "poll_interval_ms",
                                   &o->poll_interval_ms);
    if (strcmp(o->status, "running") == 0 && !o->next_poll_at_ms)
      add_warning(warnings,
                  "operation %s is running with no poll timer armed",
                  o->handle);
    count++;
  }
  free(text);
  return count;
}

static PulseBusEvent *find_bus(PulseBusEvent *bus, size_t n,
                               const char *event_id) {
  for (size_t i = 0; i < n; ++i)
    if (strcmp(bus[i].event_id, event_id) == 0) return &bus[i];
  return NULL;
}

static int message_cmp(const void *a, const void *b) {
  const PulseMessage *ma = (const PulseMessage *)a;
  const PulseMessage *mb = (const PulseMessage *)b;
  if (ma->ts_ms < mb->ts_ms) return -1;
  if (ma->ts_ms > mb->ts_ms) return 1;
  return strcmp(ma->dir, mb->dir);
}

static size_t collect_messages(const char *root, int limit,
                               PulseMessage *out, int *truncated) {
  PulseBusEvent *bus =
      (PulseBusEvent *)calloc(PULSE_MAX_BUS, sizeof(PulseBusEvent));
  PulseMessage *all =
      (PulseMessage *)calloc(PULSE_MAX_MESSAGES * 2, sizeof(PulseMessage));
  size_t bus_count = 0, count = 0;
  char *text = read_root_text(root, "workspace/logs/bus.jsonl");
  const char *p;
  size_t result_count;
  if (!bus || !all) {
    free(bus); free(all); free(text);
    if (truncated) *truncated = 1;
    return 0;
  }
  if (limit < 0) limit = 0;
  if (limit > PULSE_MAX_MESSAGES) limit = PULSE_MAX_MESSAGES;
  p = text;
  while (p && *p) {
    const char *e = strchr(p, '\n');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    char line[RT_LARGE * 2];
    JsonRef item, payload, ref;
    PulseBusEvent ev;
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, p, n); line[n] = '\0';
    memset(&ev, 0, sizeof(ev));
    if (json_ref_first_object(line, &item) == 0) {
      char ts[RT_SMALL] = "";
      (void)json_ref_object_get_string(&item, "event_id",
                                       ev.event_id,
                                       sizeof(ev.event_id));
      (void)json_ref_object_get_string(&item, "type",
                                       ev.kind, sizeof(ev.kind));
      (void)json_ref_object_get_string(&item, "channel",
                                       ev.channel, sizeof(ev.channel));
      (void)json_ref_object_get_string(&item, "ts", ts, sizeof(ts));
      ev.ts_ms = iso_to_ms(ts);
      if (json_ref_object_get_object(&item, "payload", &payload) == 0) {
        (void)json_ref_object_get_string(&payload, "text",
                                         ev.text, sizeof(ev.text));
        if (json_ref_object_get_object(&payload, "ref", &ref) == 0)
          (void)json_ref_object_get_string(&ref, "nick",
                                           ev.nick, sizeof(ev.nick));
      }
      if (bus_count < PULSE_MAX_BUS) bus[bus_count++] = ev;
      if (ev.text[0] &&
          (strcmp(ev.kind, "user_message") == 0 ||
           strcmp(ev.kind, "affair_review") == 0 ||
           strcmp(ev.kind, "operation_result") == 0) &&
          count < PULSE_MAX_MESSAGES * 2U) {
        PulseMessage *m = &all[count++];
        snprintf(m->dir, sizeof(m->dir), "in");
        snprintf(m->kind, sizeof(m->kind), "%s", ev.kind);
        snprintf(m->who, sizeof(m->who), "%s",
                 ev.nick[0] ? ev.nick : ev.channel);
        snprintf(m->text, sizeof(m->text), "%s", ev.text);
        snprintf(m->event_id, sizeof(m->event_id), "%s", ev.event_id);
        m->ts_ms = ev.ts_ms;
      }
    }
    if (!e) break;
    p = e + 1;
  }
  free(text);
  text = read_root_text(root, "workspace/logs/deliveries.jsonl");
  p = text;
  while (p && *p) {
    const char *e = strchr(p, '\n');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    char line[RT_LARGE * 2];
    JsonRef item, delivery;
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, p, n); line[n] = '\0';
    if (json_ref_first_object(line, &item) == 0 &&
        json_ref_object_get_object(&item, "delivery", &delivery) == 0 &&
        count < PULSE_MAX_MESSAGES * 2U) {
      char origin_id[RT_SMALL] = "", request_id[RT_SMALL] = "";
      PulseBusEvent *origin;
      PulseMessage *m;
      (void)json_ref_object_get_string(&delivery, "origin_event_id",
                                       origin_id, sizeof(origin_id));
      origin = find_bus(bus, bus_count, origin_id);
      m = &all[count];
      if (json_ref_object_get_string(&delivery, "text",
                                     m->text, sizeof(m->text)) == 0 &&
          m->text[0]) {
        snprintf(m->dir, sizeof(m->dir), "out");
        snprintf(m->kind, sizeof(m->kind), "delivery");
        snprintf(m->who, sizeof(m->who), "bot");
        (void)json_ref_object_get_string(&item, "run_id",
                                         m->run_id, sizeof(m->run_id));
        (void)json_ref_object_get_string(&item, "request_id",
                                         request_id, sizeof(request_id));
        m->failure = strcmp(request_id, "run_failed") == 0;
        m->proactive = origin &&
            (strcmp(origin->kind, "affair_review") == 0 ||
             strcmp(origin->kind, "operation_result") == 0);
        m->ts_ms = origin && origin->ts_ms ? origin->ts_ms + 1 : 0;
        count++;
      }
    }
    if (!e) break;
    p = e + 1;
  }
  free(text);
  qsort(all, count, sizeof(all[0]), message_cmp);
  if ((size_t)limit < count) {
    if (truncated) *truncated = 1;
    memcpy(out, all + (count - (size_t)limit),
           (size_t)limit * sizeof(out[0]));
    result_count = (size_t)limit;
  } else {
    memcpy(out, all, count * sizeof(out[0]));
    result_count = count;
  }
  free(bus);
  free(all);
  return result_count;
}

static int run_name_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static size_t collect_runs(const char *root, int limit, PulseRun *out,
                           int *truncated) {
  char runs_path[PATH_MAX];
  char names[PULSE_MAX_RUN_NAMES][RT_SMALL];
  size_t name_count = 0, count = 0;
  DIR *d;
  struct dirent *ent;
  if (limit < 0) limit = 0;
  if (root_path(root, "workspace/runs", runs_path, sizeof(runs_path)) != 0)
    return 0;
  d = opendir(runs_path);
  if (!d) return 0;
  while ((ent = readdir(d)) != NULL) {
    if (strncmp(ent->d_name, "run_", 4) != 0) continue;
    if (name_count >= PULSE_MAX_RUN_NAMES) {
      if (truncated) *truncated = 1;
      continue;
    }
    snprintf(names[name_count++], sizeof(names[0]), "%s", ent->d_name);
  }
  closedir(d);
  qsort(names, name_count, sizeof(names[0]), run_name_cmp);
  {
    size_t begin = name_count > (size_t)limit
                       ? name_count - (size_t)limit : 0;
    if (begin > 0 && truncated) *truncated = 1;
    for (size_t ni = name_count; ni > begin; --ni) {
      char rel[PATH_MAX];
      char *log;
      const char *p;
      PulseRun *r = &out[count];
      snprintf(rel, sizeof(rel),
               "workspace/runs/%s/event_log.jsonl", names[ni - 1]);
      log = read_root_text(root, rel);
      if (!log) continue;
      memset(r, 0, sizeof(*r));
      snprintf(r->run_id, sizeof(r->run_id), "%s", names[ni - 1]);
      snprintf(r->outcome, sizeof(r->outcome), "active");
      p = log;
      while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[RT_LARGE * 2], type[RT_SMALL] = "", ts[RT_SMALL] = "";
        JsonRef item, payload;
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n); line[n] = '\0';
        if (json_ref_first_object(line, &item) == 0) {
          (void)json_ref_object_get_string(&item, "type", type,
                                           sizeof(type));
          (void)json_ref_object_get_string(&item, "ts", ts, sizeof(ts));
          if (!r->origin_kind[0]) {
            snprintf(r->origin_kind, sizeof(r->origin_kind), "%s", type);
            r->start_ms = iso_to_ms(ts);
          }
          if (strcmp(type, "run_done") == 0 ||
              strcmp(type, "run_failed") == 0 ||
              strcmp(type, "run_canceled") == 0) {
            snprintf(r->outcome, sizeof(r->outcome), "%s", type);
            r->end_ms = iso_to_ms(ts);
            if (json_ref_object_get_object(&item, "payload", &payload) == 0)
              (void)json_ref_object_get_string(&payload, "reason",
                                               r->reason,
                                               sizeof(r->reason));
          }
          if (strcmp(type, "action_request") == 0 &&
              r->action_count < PULSE_ACTION_KEEP &&
              json_ref_object_get_object(&item, "payload", &payload) == 0 &&
              json_ref_object_get_string(
                  &payload, "action", r->actions[r->action_count],
                  sizeof(r->actions[0])) == 0)
            r->action_count++;
        }
        if (!e) break;
        p = e + 1;
      }
      free(log);
      if (r->origin_kind[0]) count++;
    }
  }
  return count;
}

static int append_json_string(char **cur, size_t *left, const char *value) {
  char escaped[RT_LARGE * 2];
  if (json_escape(value ? value : "", escaped, sizeof(escaped)) != 0)
    return -1;
  return pulse_appendf(cur, left, "\"%s\"", escaped);
}

int rt_pulse_projection_json(const char *root, long long now_ms,
                             int message_limit, int run_limit,
                             char *out, size_t out_len,
                             char *err, size_t err_len) {
  PulseAffair *affairs = NULL;
  PulseTask *tasks = NULL;
  PulseOperation *operations = NULL;
  PulseMessage *messages = NULL;
  PulseRun *runs = NULL;
  PulseWarnings *warnings = NULL;
  size_t affair_count, task_count, operation_count, message_count, run_count;
  long long archived = 0;
  int affairs_truncated = 0, tasks_truncated = 0, operations_truncated = 0;
  int messages_truncated = 0, runs_truncated = 0;
  char bot_name[RT_SMALL] = "floofclaw";
  char generated[64];
  char *config = NULL;
  char *cur = out;
  size_t left = out_len;
  int gateway_running = 0, gateway_pid = 0;
  char gateway_loop[RT_SMALL] = "";
  if (!out || out_len == 0) return -1;
  out[0] = '\0';
  if (err && err_len) err[0] = '\0';
  if (message_limit < 0 || message_limit > PULSE_MAX_MESSAGES ||
      run_limit < 0 || run_limit > PULSE_MAX_MESSAGES) {
    if (err) snprintf(err, err_len, "pulse collection limit out of range");
    return -1;
  }
  affairs = (PulseAffair *)calloc(PULSE_MAX_AFFAIRS, sizeof(*affairs));
  tasks = (PulseTask *)calloc(PULSE_MAX_TASKS, sizeof(*tasks));
  operations =
      (PulseOperation *)calloc(PULSE_MAX_OPERATIONS, sizeof(*operations));
  messages = (PulseMessage *)calloc(PULSE_MAX_MESSAGES, sizeof(*messages));
  runs = (PulseRun *)calloc(PULSE_MAX_MESSAGES, sizeof(*runs));
  warnings = (PulseWarnings *)calloc(1, sizeof(*warnings));
  if (!affairs || !tasks || !operations || !messages || !runs || !warnings) {
    if (err) snprintf(err, err_len, "pulse projection allocation failed");
    goto failed;
  }
  config = read_root_text(root, "config/floofclaw_config.json");
  if (config) {
    JsonRef top;
    if (json_ref_first_object(config, &top) == 0)
      (void)json_ref_object_get_string(&top, "bot_name",
                                       bot_name, sizeof(bot_name));
  }
  free(config);
  {
    char *pid_text = read_root_text(root, ".fclaw/run/gateway.pid");
    char *loop_text = read_root_text(root, ".fclaw/run/gateway.loop");
    if (pid_text) gateway_pid = atoi(pid_text);
    if (gateway_pid > 0 &&
        (kill((pid_t)gateway_pid, 0) == 0 || errno == EPERM))
      gateway_running = 1;
    if (loop_text) {
      fs_trim_newline(loop_text);
      snprintf(gateway_loop, sizeof(gateway_loop), "%s", loop_text);
    }
    free(pid_text); free(loop_text);
  }
  if (!gateway_running) add_warning(warnings, "gateway is not running");
  affair_count = collect_affairs(root, now_ms, affairs, warnings,
                                 &affairs_truncated);
  task_count = collect_tasks(root, tasks, &archived, &tasks_truncated);
  operation_count = collect_operations(root, operations, warnings,
                                       &operations_truncated);
  message_count = collect_messages(root, message_limit, messages,
                                   &messages_truncated);
  run_count = collect_runs(root, run_limit, runs, &runs_truncated);
  wall_iso(now_ms, generated, sizeof(generated));

  if (pulse_appendf(
          &cur, &left,
          "{\"schema_version\":1,\"generated_at\":\"%s\","
          "\"now_ms\":%lld,\"bot_name\":",
          generated, now_ms) != 0 ||
      append_json_string(&cur, &left, bot_name) != 0 ||
      pulse_appendf(&cur, &left,
                    ",\"gateway\":{\"running\":%s,\"pid\":%s",
                    gateway_running ? "true" : "false",
                    gateway_running ? "" : "null") != 0)
    goto too_large;
  if (gateway_running &&
      pulse_appendf(&cur, &left, "%d", gateway_pid) != 0) goto too_large;
  if (pulse_appendf(&cur, &left, ",\"loop\":") != 0 ||
      (gateway_loop[0]
           ? append_json_string(&cur, &left, gateway_loop)
           : pulse_appendf(&cur, &left, "null")) != 0 ||
      pulse_appendf(&cur, &left, ",\"started_ts\":null},\"affairs\":[") != 0)
    goto too_large;

  for (size_t i = 0; i < affair_count; ++i) {
    PulseAffair *a = &affairs[i];
    char eid[RT_MED], ectx[RT_LARGE], est[RT_MED], eman[RT_LARGE * 2];
    if (json_escape(a->affair_id, eid, sizeof(eid)) != 0 ||
        json_escape(a->context_id, ectx, sizeof(ectx)) != 0 ||
        json_escape(a->status, est, sizeof(est)) != 0 ||
        json_escape(a->manage, eman, sizeof(eman)) != 0 ||
        pulse_appendf(&cur, &left,
                      "%s{\"affair_id\":\"%s\",\"context_id\":\"%s\","
                      "\"status\":\"%s\",\"manage\":\"%s\","
                      "\"next_review_at_ms\":%lld,\"notes\":[",
                      i ? "," : "", eid, ectx, est, eman,
                      a->next_review_at_ms) != 0)
      goto too_large;
    for (size_t ni = 0; ni < a->note_count; ++ni) {
      if (pulse_appendf(&cur, &left, "%s", ni ? "," : "") != 0 ||
          append_json_string(&cur, &left, a->notes[ni]) != 0)
        goto too_large;
    }
    if (pulse_appendf(&cur, &left, "],\"created_ms\":%lld%s}",
                      a->created_ms,
                      a->dormant ? ",\"dormant\":true" : "") != 0)
      goto too_large;
  }
  if (pulse_appendf(&cur, &left, "],\"tasks\":[") != 0) goto too_large;
  for (size_t i = 0; i < task_count; ++i) {
    PulseTask *t = &tasks[i];
    char etid[RT_MED], ekind[RT_MED], est[RT_MED];
    char ework[RT_LARGE * 2], edone[RT_LARGE * 2];
    if (json_escape(t->task_id, etid, sizeof(etid)) != 0 ||
        json_escape(t->kind, ekind, sizeof(ekind)) != 0 ||
        json_escape(t->status, est, sizeof(est)) != 0 ||
        json_escape(t->work, ework, sizeof(ework)) != 0 ||
        json_escape(t->done_when, edone, sizeof(edone)) != 0 ||
        pulse_appendf(
            &cur, &left,
            "%s{\"task_id\":\"%s\",\"kind\":\"%s\",\"status\":\"%s\","
            "\"work\":\"%s\",\"done_when\":\"%s\",\"work_rev\":%lld,"
            "\"external\":%s,\"updated_ms\":%lld}",
            i ? "," : "", etid, ekind, est, ework, edone, t->work_rev,
            t->external[0] ? t->external : "null", t->updated_ms) != 0)
      goto too_large;
  }
  if (pulse_appendf(&cur, &left,
                    "],\"tasks_archived\":%lld,\"operations\":[", archived) != 0)
    goto too_large;
  for (size_t i = 0; i < operation_count; ++i) {
    PulseOperation *o = &operations[i];
    char eh[RT_MED], ea[RT_MED], et[RT_MED], es[RT_MED];
    if (json_escape(o->handle, eh, sizeof(eh)) != 0 ||
        json_escape(o->action, ea, sizeof(ea)) != 0 ||
        json_escape(o->task_id, et, sizeof(et)) != 0 ||
        json_escape(o->status, es, sizeof(es)) != 0 ||
        pulse_appendf(
            &cur, &left,
            "%s{\"handle\":\"%s\",\"action\":\"%s\",\"task_id\":\"%s\","
            "\"status\":\"%s\",\"work_rev\":%lld,\"next_poll_at_ms\":%lld,"
            "\"poll_interval_ms\":%lld}",
            i ? "," : "", eh, ea, et, es, o->work_rev,
            o->next_poll_at_ms, o->poll_interval_ms) != 0)
      goto too_large;
  }
  if (pulse_appendf(&cur, &left, "],\"messages\":[") != 0) goto too_large;
  for (size_t i = 0; i < message_count; ++i) {
    PulseMessage *m = &messages[i];
    char ek[RT_MED], ew[RT_MED], et[RT_LARGE * 2];
    if (json_escape(m->kind, ek, sizeof(ek)) != 0 ||
        json_escape(m->who, ew, sizeof(ew)) != 0 ||
        json_escape(m->text, et, sizeof(et)) != 0)
      goto too_large;
    if (m->ts_ms) {
      if (pulse_appendf(
              &cur, &left,
              "%s{\"ts_ms\":%lld,\"dir\":\"%s\",\"kind\":\"%s\","
              "\"who\":\"%s\",\"text\":\"%s\"",
              i ? "," : "", m->ts_ms, m->dir, ek, ew, et) != 0)
        goto too_large;
    } else if (pulse_appendf(
                   &cur, &left,
                   "%s{\"ts_ms\":null,\"dir\":\"%s\",\"kind\":\"%s\","
                   "\"who\":\"%s\",\"text\":\"%s\"",
                   i ? "," : "", m->dir, ek, ew, et) != 0) {
      goto too_large;
    }
    if (m->event_id[0]) {
      if (pulse_appendf(&cur, &left, ",\"event_id\":") != 0 ||
          append_json_string(&cur, &left, m->event_id) != 0)
        goto too_large;
    }
    if (m->run_id[0]) {
      if (pulse_appendf(&cur, &left, ",\"run_id\":") != 0 ||
          append_json_string(&cur, &left, m->run_id) != 0)
        goto too_large;
    }
    if (m->failure &&
        pulse_appendf(&cur, &left, ",\"failure\":true") != 0)
      goto too_large;
    if (m->proactive &&
        pulse_appendf(&cur, &left, ",\"proactive\":true") != 0)
      goto too_large;
    if (pulse_appendf(&cur, &left, "}") != 0) goto too_large;
  }
  if (pulse_appendf(&cur, &left, "],\"runs\":[") != 0) goto too_large;
  for (size_t i = 0; i < run_count; ++i) {
    PulseRun *r = &runs[i];
    char erid[RT_MED], eorigin[RT_MED], eout[RT_MED], ereason[RT_LARGE];
    if (json_escape(r->run_id, erid, sizeof(erid)) != 0 ||
        json_escape(r->origin_kind, eorigin, sizeof(eorigin)) != 0 ||
        json_escape(r->outcome, eout, sizeof(eout)) != 0 ||
        json_escape(r->reason, ereason, sizeof(ereason)) != 0 ||
        pulse_appendf(
            &cur, &left,
            "%s{\"run_id\":\"%s\",\"origin_kind\":\"%s\","
            "\"outcome\":\"%s\",\"reason\":\"%s\",\"actions\":[",
            i ? "," : "", erid, eorigin, eout, ereason) != 0)
      goto too_large;
    for (size_t ai = 0; ai < r->action_count; ++ai) {
      if (pulse_appendf(&cur, &left, "%s", ai ? "," : "") != 0 ||
          append_json_string(&cur, &left, r->actions[ai]) != 0)
        goto too_large;
    }
    if (pulse_appendf(&cur, &left, "],\"start_ms\":") != 0)
      goto too_large;
    if ((r->start_ms
             ? pulse_appendf(&cur, &left, "%lld", r->start_ms)
             : pulse_appendf(&cur, &left, "null")) != 0 ||
        pulse_appendf(&cur, &left, ",\"end_ms\":") != 0 ||
        (r->end_ms
             ? pulse_appendf(&cur, &left, "%lld", r->end_ms)
             : pulse_appendf(&cur, &left, "null")) != 0 ||
        pulse_appendf(&cur, &left, "}") != 0)
      goto too_large;
  }
  if (pulse_appendf(&cur, &left, "],\"warnings\":[") != 0) goto too_large;
  for (size_t i = 0; i < warnings->count; ++i) {
    if (pulse_appendf(&cur, &left, "%s", i ? "," : "") != 0 ||
        append_json_string(&cur, &left, warnings->values[i]) != 0)
      goto too_large;
  }
  if (pulse_appendf(
          &cur, &left,
          "],\"limits\":{\"messages\":%d,\"runs\":%d,"
          "\"affairs\":%d,\"tasks\":%d,\"operations\":%d,"
          "\"truncated\":{\"any\":%s,\"messages\":%s,\"runs\":%s,"
          "\"affairs\":%s,\"tasks\":%s,\"operations\":%s}}}\n",
          message_limit, run_limit, PULSE_MAX_AFFAIRS, PULSE_MAX_TASKS,
          PULSE_MAX_OPERATIONS,
          (affairs_truncated || tasks_truncated || operations_truncated ||
           messages_truncated || runs_truncated) ? "true" : "false",
          messages_truncated ? "true" : "false",
          runs_truncated ? "true" : "false",
          affairs_truncated ? "true" : "false",
          tasks_truncated ? "true" : "false",
          operations_truncated ? "true" : "false") != 0)
    goto too_large;
  free(affairs); free(tasks); free(operations);
  free(messages); free(runs); free(warnings);
  return 0;

too_large:
  if (err) snprintf(err, err_len,
                    "pulse projection exceeds output buffer");
  if (out_len) out[0] = '\0';
failed:
  free(affairs); free(tasks); free(operations);
  free(messages); free(runs); free(warnings);
  return -1;
}
