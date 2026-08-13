#ifndef FCLAW_REACTOR_H
#define FCLAW_REACTOR_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define FC_POLL_MAX_FDS         64
#define FC_REACTOR_MAX_MODULES  16
/* A single 250 ms callback has consumed more than the reactor's ordinary
 * 200 ms poll cadence by itself. That is operationally visible latency, while
 * shorter filesystem/TLS bursts remain below the warning line. */
#define FC_REACTOR_SLOW_TICK_MS 250ULL

typedef struct FcReactorModule FcReactorModule;

typedef struct {
  int fd;
  short events;            /* POLLIN | POLLOUT */
  FcReactorModule *owner;  /* back-pointer for dispatch */
  void *tag;               /* module-defined per-fd cookie */
} FcPollEntry;

typedef struct {
  FcPollEntry entries[FC_POLL_MAX_FDS];
  size_t count;
} FcPollSet;

typedef struct {
  uint64_t callback_errors;
  uint64_t collect_fds_errors;
  uint64_t on_fd_errors;
  uint64_t tick_errors;
  uint64_t slow_ticks;
  uint64_t last_tick_ms;
  uint64_t max_tick_ms;
} FcReactorModuleStats;

/* Anything participating in the reactor loop is a module: channel adapters
 * (IRC, Discord, WS) and internal subsystems (jobrunner, bus_intake,
 * runtime) all implement the same shape. The reactor itself doesn't know
 * the difference.
 *
 * Hard contract: no callback may block. collect_fds, on_fd, and tick must
 * all return promptly. Long-running work is launched into a child whose
 * stdout/stderr pipes are observed via the jobrunner module. */
struct FcReactorModule {
  const char *name;       /* "irc", "ws", "jobrunner", "bus_intake", "runtime", ... */
  const char *module_id;  /* "irc-main", "runtime-main", ... — uniqueness is the
                           * module's own concern (channel modules use it for
                           * delivery routing). */
  void *state;

  int  (*init)(struct FcReactorModule *m);
  int  (*collect_fds)(struct FcReactorModule *m, FcPollSet *pollset);
  int  (*on_fd)(struct FcReactorModule *m, int fd, int events, void *tag);
  int  (*tick)(struct FcReactorModule *m, uint64_t now_ms);
  void (*shutdown)(struct FcReactorModule *m);

  /* Absolute ms the module wants its next tick by, or 0 / <=now_ms for
   * "immediate work, don't sleep." Return FC_NO_DEADLINE for "nothing
   * pending." Called once per pass, after collect_fds, before poll. The
   * reactor uses min(deadlines, now + default_timeout_ms) as the poll
   * timeout. */
  uint64_t (*next_deadline_ms)(struct FcReactorModule *m, uint64_t now_ms);
};

#define FC_NO_DEADLINE ((uint64_t)-1)

typedef struct {
  FcReactorModule *modules[FC_REACTOR_MAX_MODULES];
  size_t module_count;
  volatile sig_atomic_t *stop_flag;
  int default_timeout_ms;
  uint64_t total_ticks;
  uint64_t total_polls;
  FcReactorModuleStats module_stats[FC_REACTOR_MAX_MODULES];
} FcReactor;

void fc_pollset_clear(FcPollSet *ps);
int  fc_pollset_add(FcPollSet *ps, int fd, short events, FcReactorModule *owner, void *tag);

void fc_reactor_init(FcReactor *r);
/* Modules are ticked in registration order. Order matters when modules
 * share state (e.g. the jobrunner module must tick before runtime so the
 * runtime sees just-finished jobs). */
int  fc_reactor_register(FcReactor *r, FcReactorModule *m);
int  fc_reactor_run(FcReactor *r, volatile sig_atomic_t *stop_flag, int default_timeout_ms);
void fc_reactor_shutdown(FcReactor *r);
uint64_t fc_reactor_total_module_errors(const FcReactor *r);
uint64_t fc_reactor_total_slow_ticks(const FcReactor *r);

uint64_t fc_now_ms(void);

/* Built-in noop module, used by smokes and as the canonical example. */
FcReactorModule *fc_noop_module_create(void);
void             fc_noop_module_destroy(FcReactorModule *m);
uint64_t         fc_noop_module_tick_count(const FcReactorModule *m);

#endif
