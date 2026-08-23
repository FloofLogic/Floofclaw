/* Poll-set capacity.
 *
 * A descriptor the reactor cannot watch is a stall, not a crash: a child
 * pipe goes unread until the next poll deadline (5 s in daemon mode) or
 * until its 4 KB kernel buffer fills and the child blocks writing. The set
 * held 64 descriptors, four call sites discarded fc_pollset_add's result,
 * and nothing counted the refusals — so the failure was invisible from
 * `gateway status` and from the logs alike. */

#include "test_support.h"

#include "../../runtime/gateway/reactor.h"
#include "../../runtime/gateway/status_module.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  int fd;
  int add_attempts;
  volatile sig_atomic_t *stop;
} PollFillCanary;

static int fill_collect(FcReactorModule *m, FcPollSet *ps) {
  PollFillCanary *c = (PollFillCanary *)m->state;
  /* The same descriptor, over and over: this is about the set's capacity,
   * not about opening a hundred real files. */
  for (int i = 0; i < c->add_attempts; ++i)
    (void)fc_pollset_add(ps, c->fd, POLLIN, m, NULL);
  return 0;
}

static int fill_tick(FcReactorModule *m, uint64_t now_ms) {
  PollFillCanary *c = (PollFillCanary *)m->state;
  (void)now_ms;
  if (c->stop) *c->stop = 1;
  return 0;
}

int reactor_poll_set_overflow_is_counted_and_reported(void) {
  FcReactor reactor;
  FcReactorModule module;
  FcPollSet *ps = NULL;
  PollFillCanary canary;
  volatile sig_atomic_t stop = 0;
  int pipe_fds[2] = {-1, -1};
  char status[FC_STATUS_JSON_CAP];
  const int extra = 24;
  int rc = 0;

  rc |= test_reset_workspace();

  /* The cap must cover the worst pass this gateway can actually build:
   * 16 concurrent jobs at four descriptors each, the wake and status
   * sockets, the WebSocket listener and its 16 clients, the Discord
   * gateway and REST sockets, and the IRC socket. */
  rc |= expect(FC_POLL_MAX_FDS >= 16 * 4 + 2 + 1 + 16 + 2 + 1,
               "the poll set covers the worst realistic pass");

  /* The set accepts exactly its cap, then refuses and counts. */
  ps = (FcPollSet *)calloc(1, sizeof(*ps));
  rc |= expect(ps != NULL, "allocate a probe poll set");
  rc |= expect(pipe(pipe_fds) == 0, "create a probe descriptor");
  if (!ps || pipe_fds[0] < 0) {
    free(ps);
    return rc == 0 ? -1 : rc;
  }
  for (int i = 0; i < FC_POLL_MAX_FDS; ++i)
    rc |= expect(fc_pollset_add(ps, pipe_fds[0], POLLIN, NULL, NULL) == 0,
                 "the set accepts descriptors up to its cap");
  rc |= expect(ps->count == (size_t)FC_POLL_MAX_FDS,
               "a full set holds exactly its cap");
  rc |= expect(ps->overflows == 0, "a full set has refused nothing yet");
  for (int i = 0; i < extra; ++i)
    rc |= expect(fc_pollset_add(ps, pipe_fds[0], POLLIN, NULL, NULL) != 0,
                 "a full set refuses further descriptors");
  rc |= expect(ps->count == (size_t)FC_POLL_MAX_FDS,
               "a refused descriptor does not grow the set");
  rc |= expect(ps->overflows == (uint64_t)extra,
               "every refusal is counted, not discarded");
  fc_pollset_clear(ps);
  rc |= expect(ps->count == 0 && ps->overflows == 0,
               "clearing the set resets both counters");
  free(ps);

  /* A module that overruns the set has its refusals accumulated by the
   * reactor and surfaced in gateway status. */
  memset(&canary, 0, sizeof(canary));
  canary.fd = pipe_fds[0];
  canary.add_attempts = FC_POLL_MAX_FDS + extra;
  canary.stop = &stop;
  memset(&module, 0, sizeof(module));
  module.name = "poll_fill";
  module.module_id = "poll-fill";
  module.state = &canary;
  module.collect_fds = fill_collect;
  module.tick = fill_tick;

  fc_reactor_init(&reactor);
  rc |= expect(fc_reactor_register(&reactor, &module) == 0,
               "register the poll-fill canary");
  rc |= expect(fc_reactor_run(&reactor, &stop, 1) == 0,
               "the reactor keeps running through a full poll set");
  rc |= expect(fc_reactor_poll_overflows(&reactor) == (uint64_t)extra,
               "the reactor accumulates the pass's refused descriptors");
  rc |= expect(fc_status_build_json_with_reactor(NULL, &reactor,
                                                 status, sizeof(status)) == 0,
               "build gateway status with poll health");
  {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"poll_overflows\": %d", extra);
    rc |= expect_substr(status, needle,
                        "gateway status reports the refused descriptors");
  }

  if (pipe_fds[0] >= 0) close(pipe_fds[0]);
  if (pipe_fds[1] >= 0) close(pipe_fds[1]);
  return rc;
}
