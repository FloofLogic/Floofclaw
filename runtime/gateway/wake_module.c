#include "wake_module.h"

#include "../bus/bus.h"
#include "../support/fsutil.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
  int fd;
} BusWakeState;

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int wake_init(FcReactorModule *m) {
  BusWakeState *s = m ? (BusWakeState *)m->state : NULL;
  struct sockaddr_un addr;
  int fd;
  if (!s) return -1;
  if (fs_mkdir_p(".fclaw/run") != 0) return -1;
  (void)unlink(BUS_WAKE_SOCK_PATH);
  fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  if (set_nonblocking(fd) != 0) { close(fd); return -1; }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", BUS_WAKE_SOCK_PATH);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
  s->fd = fd;
  return 0;
}

static int wake_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  BusWakeState *s = m ? (BusWakeState *)m->state : NULL;
  if (!s || s->fd < 0) return 0;
  return fc_pollset_add(ps, s->fd, POLLIN, m, NULL);
}

static int wake_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  BusWakeState *s = m ? (BusWakeState *)m->state : NULL;
  char buf[64];
  (void)tag;
  if (!s || fd != s->fd || !(events & (POLLIN | POLLHUP | POLLERR))) return 0;
  while (recv(s->fd, buf, sizeof(buf), 0) > 0) {}
  return 0;
}

static int wake_tick(FcReactorModule *m, uint64_t now_ms) {
  (void)m; (void)now_ms;
  return 0;
}

static void wake_shutdown(FcReactorModule *m) {
  BusWakeState *s = m ? (BusWakeState *)m->state : NULL;
  if (!s) return;
  if (s->fd >= 0) close(s->fd);
  s->fd = -1;
  (void)unlink(BUS_WAKE_SOCK_PATH);
}

FcReactorModule *fc_bus_wake_module_create(void) {
  FcReactorModule *m = (FcReactorModule *)calloc(1, sizeof(*m));
  BusWakeState *s = (BusWakeState *)calloc(1, sizeof(*s));
  if (!m || !s) { free(m); free(s); return NULL; }
  s->fd = -1;
  m->name = "bus_wake";
  m->module_id = "bus-wake-main";
  m->state = s;
  m->init = wake_init;
  m->collect_fds = wake_collect_fds;
  m->on_fd = wake_on_fd;
  m->tick = wake_tick;
  m->shutdown = wake_shutdown;
  return m;
}

void fc_bus_wake_module_destroy(FcReactorModule *m) {
  if (!m) return;
  free(m->state);
  free(m);
}
