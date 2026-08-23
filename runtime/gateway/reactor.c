#include "reactor.h"
#include "../runtime.h"
#include "../support/timing.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t fc_now_ms(void) {
  return timing_monotonic_ms();
}

void fc_pollset_clear(FcPollSet *ps) {
  if (!ps) return;
  ps->count = 0;
  ps->overflows = 0;
}

int fc_pollset_add(FcPollSet *ps, int fd, short events, FcReactorModule *owner, void *tag) {
  if (!ps || fd < 0) return -1;
  if (ps->count >= FC_POLL_MAX_FDS) {
    /* Refused, and recorded. The caller may ignore this return value —
     * most do — but the reactor still learns the descriptor went
     * unwatched. */
    ps->overflows++;
    return -1;
  }
  ps->entries[ps->count].fd = fd;
  ps->entries[ps->count].events = events;
  ps->entries[ps->count].owner = owner;
  ps->entries[ps->count].tag = tag;
  ps->count++;
  return 0;
}

void fc_reactor_init(FcReactor *r) {
  if (!r) return;
  memset(r, 0, sizeof(*r));
  r->default_timeout_ms = 200;
}

int fc_reactor_register(FcReactor *r, FcReactorModule *m) {
  if (!r || !m) return -1;
  if (r->module_count >= FC_REACTOR_MAX_MODULES) return -1;
  r->modules[r->module_count++] = m;
  return 0;
}

static const char *module_label(const FcReactorModule *m) {
  if (!m) return "(unknown)";
  if (m->module_id && *m->module_id) return m->module_id;
  if (m->name && *m->name) return m->name;
  return "(anonymous)";
}

static size_t module_index(const FcReactor *r, const FcReactorModule *m) {
  size_t i;
  if (!r || !m) return FC_REACTOR_MAX_MODULES;
  for (i = 0; i < r->module_count; ++i)
    if (r->modules[i] == m) return i;
  return FC_REACTOR_MAX_MODULES;
}

static int report_count(uint64_t count) {
  return count == 1U || (count != 0U && (count & (count - 1U)) == 0U);
}

static void record_callback_error(FcReactor *r, size_t index,
                                  const char *callback, int rc) {
  FcReactorModuleStats *stats;
  FcReactorModule *m;
  if (!r || index >= r->module_count) return;
  stats = &r->module_stats[index];
  m = r->modules[index];
  stats->callback_errors++;
  if (strcmp(callback, "collect_fds") == 0) stats->collect_fds_errors++;
  else if (strcmp(callback, "on_fd") == 0) stats->on_fd_errors++;
  else if (strcmp(callback, "tick") == 0) stats->tick_errors++;
  /* Keep persistent failures visible without turning stderr into the outage. */
  if (report_count(stats->callback_errors))
    fprintf(stderr,
            "reactor: module %s callback %s failed rc=%d count=%llu\n",
            module_label(m), callback, rc,
            (unsigned long long)stats->callback_errors);
}

static void record_tick(FcReactor *r, size_t index, uint64_t duration_ms) {
  FcReactorModuleStats *stats;
  FcReactorModule *m;
  if (!r || index >= r->module_count) return;
  stats = &r->module_stats[index];
  m = r->modules[index];
  stats->last_tick_ms = duration_ms;
  if (duration_ms > stats->max_tick_ms) stats->max_tick_ms = duration_ms;
  if (duration_ms < FC_REACTOR_SLOW_TICK_MS) return;
  stats->slow_ticks++;
  (void)rt_narrate(
      "reactor: slow tick module=%s duration_ms=%llu threshold_ms=%llu",
      module_label(m), (unsigned long long)duration_ms,
      (unsigned long long)FC_REACTOR_SLOW_TICK_MS);
}

int fc_reactor_run(FcReactor *r, volatile sig_atomic_t *stop_flag, int default_timeout_ms) {
  size_t i;
  if (!r) return -1;
  r->stop_flag = stop_flag;
  if (default_timeout_ms > 0) r->default_timeout_ms = default_timeout_ms;

  /* init phase */
  for (i = 0; i < r->module_count; ++i) {
    FcReactorModule *m = r->modules[i];
    if (m->init && m->init(m) != 0) {
      fprintf(stderr, "reactor: module %s init failed\n", m->name ? m->name : "(anon)");
      return -1;
    }
  }

  /* main loop:  collect_fds  ->  poll  ->  dispatch ready fds  ->  tick */
  while (!stop_flag || !*stop_flag) {
    FcPollSet ps;
    struct pollfd pfds[FC_POLL_MAX_FDS];
    int rc;
    uint64_t now;

    fc_pollset_clear(&ps);
    for (i = 0; i < r->module_count; ++i) {
      FcReactorModule *m = r->modules[i];
      if (m->collect_fds) {
        int callback_rc = m->collect_fds(m, &ps);
        if (callback_rc != 0)
          record_callback_error(r, i, "collect_fds", callback_rc);
      }
    }
    if (ps.overflows > 0) {
      /* An unwatched descriptor is a stall, not a crash, so it has to be
       * loud on its own. Rate-limited the same way callback errors are. */
      r->poll_overflows += ps.overflows;
      if (report_count(r->poll_overflows))
        fprintf(stderr,
                "reactor: poll set full at %d descriptors; %llu not watched "
                "this pass, %llu total\n",
                FC_POLL_MAX_FDS, (unsigned long long)ps.overflows,
                (unsigned long long)r->poll_overflows);
    }
    for (i = 0; i < ps.count; ++i) {
      pfds[i].fd = ps.entries[i].fd;
      pfds[i].events = ps.entries[i].events;
      pfds[i].revents = 0;
    }

    {
      uint64_t now_pre = fc_now_ms();
      uint64_t earliest = now_pre + (uint64_t)r->default_timeout_ms;
      int timeout_ms;
      for (i = 0; i < r->module_count; ++i) {
        FcReactorModule *m = r->modules[i];
        if (m->next_deadline_ms) {
          uint64_t d = m->next_deadline_ms(m, now_pre);
          if (d != FC_NO_DEADLINE && d < earliest) earliest = d;
        }
      }
      timeout_ms = (earliest <= now_pre) ? 0 : (int)(earliest - now_pre);
      if (timeout_ms > r->default_timeout_ms) timeout_ms = r->default_timeout_ms;
      rc = poll(pfds, (nfds_t)ps.count, timeout_ms);
    }
    r->total_polls++;
    if (rc < 0) {
      if (errno == EINTR) continue;
      fprintf(stderr, "reactor: poll failed: %s\n", strerror(errno));
      break;
    }

    if (rc > 0) {
      for (i = 0; i < ps.count; ++i) {
        if (pfds[i].revents) {
          FcReactorModule *m = ps.entries[i].owner;
          if (m && m->on_fd) {
            int callback_rc = m->on_fd(m, pfds[i].fd, pfds[i].revents,
                                       ps.entries[i].tag);
            if (callback_rc != 0) {
              size_t owner_index = module_index(r, m);
              record_callback_error(r, owner_index, "on_fd", callback_rc);
            }
          }
        }
      }
    }

    now = fc_now_ms();
    for (i = 0; i < r->module_count; ++i) {
      FcReactorModule *m = r->modules[i];
      if (m->tick) {
        uint64_t started_ms = fc_now_ms();
        int callback_rc = m->tick(m, now);
        uint64_t finished_ms = fc_now_ms();
        uint64_t duration_ms = finished_ms >= started_ms
                                   ? finished_ms - started_ms
                                   : 0;
        record_tick(r, i, duration_ms);
        if (callback_rc != 0)
          record_callback_error(r, i, "tick", callback_rc);
      }
    }
    r->total_ticks++;
  }

  return 0;
}

uint64_t fc_reactor_total_module_errors(const FcReactor *r) {
  uint64_t total = 0;
  size_t i;
  if (!r) return 0;
  for (i = 0; i < r->module_count; ++i)
    total += r->module_stats[i].callback_errors;
  return total;
}

uint64_t fc_reactor_total_slow_ticks(const FcReactor *r) {
  uint64_t total = 0;
  size_t i;
  if (!r) return 0;
  for (i = 0; i < r->module_count; ++i)
    total += r->module_stats[i].slow_ticks;
  return total;
}

void fc_reactor_shutdown(FcReactor *r) {
  size_t i;
  if (!r) return;
  for (i = 0; i < r->module_count; ++i) {
    FcReactorModule *m = r->modules[i];
    if (m && m->shutdown) m->shutdown(m);
  }
}

/* ----- noop module ----- */

typedef struct {
  uint64_t tick_count;
  uint64_t last_tick_ms;
} NoopState;

static int noop_init(FcReactorModule *m)             { (void)m; return 0; }
static int noop_collect_fds(FcReactorModule *m, FcPollSet *ps) { (void)m; (void)ps; return 0; }
static int noop_on_fd(FcReactorModule *m, int fd, int rev, void *tag) {
  (void)m; (void)fd; (void)rev; (void)tag; return 0;
}
static int noop_tick(FcReactorModule *m, uint64_t now_ms) {
  NoopState *s = (NoopState *)m->state;
  if (s) { s->tick_count++; s->last_tick_ms = now_ms; }
  return 0;
}
static void noop_shutdown(FcReactorModule *m) { (void)m; }

FcReactorModule *fc_noop_module_create(void) {
  FcReactorModule *m = (FcReactorModule *)calloc(1, sizeof(*m));
  NoopState *s = (NoopState *)calloc(1, sizeof(*s));
  if (!m || !s) { free(m); free(s); return NULL; }
  m->name = "noop";
  m->module_id = "noop-main";
  m->state = s;
  m->init = noop_init;
  m->collect_fds = noop_collect_fds;
  m->on_fd = noop_on_fd;
  m->tick = noop_tick;
  m->shutdown = noop_shutdown;
  return m;
}

void fc_noop_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m->state);
  free(m);
}

uint64_t fc_noop_module_tick_count(const FcReactorModule *m) {
  const NoopState *s;
  if (!m || !m->state) return 0;
  s = (const NoopState *)m->state;
  return s->tick_count;
}

uint64_t fc_reactor_poll_overflows(const FcReactor *r) {
  return r ? r->poll_overflows : 0;
}
