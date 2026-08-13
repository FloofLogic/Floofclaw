#include "action_secret_broker.h"

#include "secret_store.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BROKER_HEADER_MAX 512

static int safe_id(const char *s) {
  size_t n = 0;
  if (!s || !*s) return 0;
  for (; *s; ++s, ++n) {
    unsigned char c = (unsigned char)*s;
    if (!(isalnum((int)c) || c == '_' || c == '-')) return 0;
    if (n >= 126U) return 0;
  }
  return 1;
}

static int safe_suffix(const char *s) {
  size_t n = 0;
  if (!s || !*s) return 0;
  for (; *s; ++s, ++n) {
    unsigned char c = (unsigned char)*s;
    if (!(isalnum((int)c) || c == '_' || c == '-' || c == '.')) return 0;
    if (n >= ACTION_SECRET_SUFFIX_MAX) return 0;
  }
  return 1;
}

static int make_key(const ActionSecretBroker *b, const char *suffix,
                    char *out, size_t out_len) {
  if (!b || !safe_suffix(suffix) || !out || out_len == 0) return -1;
  return snprintf(out, out_len, "endpoint:action:%s:%s", b->action_id,
                  suffix) < (int)out_len ? 0 : -1;
}

int action_secret_broker_pair(int fds[2]) {
  if (!fds || socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return -1;
  (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  /* The child endpoint must survive exec; its descriptor number is shared
   * through FCLAW_SECRET_FD, never through argv or an artifact. */
  (void)fcntl(fds[1], F_SETFD, 0);
  return 0;
}

int action_secret_broker_init(ActionSecretBroker *b, int fd,
                              const char *action_id) {
  int flags;
  if (!b || fd < 0 || !safe_id(action_id)) return -1;
  memset(b, 0, sizeof(*b));
  b->fd = fd;
  snprintf(b->action_id, sizeof(b->action_id), "%s", action_id);
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    close(fd);
    b->fd = -1;
    return -1;
  }
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  return 0;
}

static int set_response(ActionSecretBroker *b, const char *header,
                        const void *body, size_t body_len) {
  size_t hlen;
  if (!b || !header) return -1;
  hlen = strlen(header);
  if (hlen + body_len > sizeof(b->output)) return -1;
  secret_store_zero(b->output, sizeof(b->output));
  memcpy(b->output, header, hlen);
  if (body_len > 0 && body) memcpy(b->output + hlen, body, body_len);
  b->output_len = hlen + body_len;
  b->output_sent = 0;
  return 0;
}

static int set_simple(ActionSecretBroker *b, const char *line) {
  return set_response(b, line, NULL, 0);
}

static void clear_input(ActionSecretBroker *b) {
  if (!b) return;
  secret_store_zero(b->input, b->input_len + 1U);
  b->input_len = 0;
}

typedef struct {
  char body[ACTION_SECRET_IO_MAX - 64];
  size_t used;
  int overflow;
} ListResult;

static int append_suffix(const char *suffix, void *user) {
  ListResult *list = (ListResult *)user;
  size_t n;
  if (!list || !suffix || !safe_suffix(suffix)) return 0;
  n = strlen(suffix);
  if (list->used + n + 1U > sizeof(list->body)) {
    list->overflow = 1;
    return -1;
  }
  memcpy(list->body + list->used, suffix, n);
  list->used += n;
  list->body[list->used++] = '\n';
  return 0;
}

static int handle_request(ActionSecretBroker *b) {
  unsigned char *nl;
  char header[BROKER_HEADER_MAX];
  size_t header_len, consumed;
  if (!b || b->output_len != b->output_sent) return 0;
  nl = memchr(b->input, '\n', b->input_len);
  if (!nl) {
    if (b->input_len >= BROKER_HEADER_MAX) return -1;
    return 0;
  }
  header_len = (size_t)(nl - b->input);
  if (header_len == 0 || header_len >= sizeof(header)) return -1;
  memcpy(header, b->input, header_len);
  header[header_len] = '\0';
  consumed = header_len + 1U;

  if (strncmp(header, "GET ", 4) == 0) {
    char key[512], value[ACTION_SECRET_VALUE_MAX + 1];
    char reply[64];
    const char *suffix = header + 4;
    int rc;
    if (!safe_suffix(suffix) || make_key(b, suffix, key, sizeof(key)) != 0) {
      clear_input(b);
      return set_simple(b, "ERROR BAD_NAME\n");
    }
    rc = secret_store_get(key, value, sizeof(value));
    if (rc != 0) {
      secret_store_zero(value, sizeof(value));
      rc = set_simple(b, "NOT_FOUND\n");
    } else {
      size_t value_len = strlen(value);
      snprintf(reply, sizeof(reply), "VALUE %zu\n", value_len);
      rc = set_response(b, reply, value, value_len);
      secret_store_zero(value, sizeof(value));
    }
    clear_input(b);
    return rc;
  }

  if (strncmp(header, "DELETE ", 7) == 0) {
    char key[512];
    const char *suffix = header + 7;
    int rc;
    if (!safe_suffix(suffix) || make_key(b, suffix, key, sizeof(key)) != 0) {
      clear_input(b);
      return set_simple(b, "ERROR BAD_NAME\n");
    }
    rc = secret_store_delete(key) == 0
             ? set_simple(b, "OK\n")
             : set_simple(b, "ERROR STORE\n");
    clear_input(b);
    return rc;
  }

  if (strcmp(header, "LIST") == 0) {
    char prefix[512], reply[64];
    ListResult list;
    int rc;
    memset(&list, 0, sizeof(list));
    if (snprintf(prefix, sizeof(prefix), "endpoint:action:%s:", b->action_id) >=
        (int)sizeof(prefix))
      return -1;
    rc = secret_store_list_prefix(prefix, append_suffix, &list);
    if (rc != 0 || list.overflow) {
      secret_store_zero(&list, sizeof(list));
      clear_input(b);
      return set_simple(b, "ERROR TOO_LARGE\n");
    }
    snprintf(reply, sizeof(reply), "LIST %zu\n", list.used);
    rc = set_response(b, reply, list.body, list.used);
    secret_store_zero(&list, sizeof(list));
    clear_input(b);
    return rc;
  }

  if (strncmp(header, "SET ", 4) == 0) {
    char suffix[ACTION_SECRET_SUFFIX_MAX + 1], extra;
    char key[512];
    size_t value_len = 0;
    unsigned char *value;
    int parsed = sscanf(header + 4, "%192s %zu %c", suffix, &value_len, &extra);
    int rc;
    if (parsed != 2 || !safe_suffix(suffix) ||
        value_len == 0 || value_len > ACTION_SECRET_VALUE_MAX ||
        make_key(b, suffix, key, sizeof(key)) != 0) {
      clear_input(b);
      return set_simple(b, "ERROR BAD_REQUEST\n");
    }
    if (b->input_len < consumed + value_len) return 0;
    if (b->input_len != consumed + value_len) return -1;
    value = b->input + consumed;
    if (memchr(value, '\0', value_len) != NULL) return -1;
    value[value_len] = '\0';
    rc = secret_store_set(key, (const char *)value) == 0
             ? set_simple(b, "OK\n")
             : set_simple(b, "ERROR STORE\n");
    clear_input(b);
    return rc;
  }

  clear_input(b);
  return set_simple(b, "ERROR UNKNOWN_OP\n");
}

static int flush_output(ActionSecretBroker *b) {
  while (b->output_sent < b->output_len) {
    ssize_t n = write(b->fd, b->output + b->output_sent,
                      b->output_len - b->output_sent);
    if (n > 0) {
      b->output_sent += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
  }
  secret_store_zero(b->output, sizeof(b->output));
  b->output_len = 0;
  b->output_sent = 0;
  return 0;
}

static int read_input(ActionSecretBroker *b) {
  for (;;) {
    ssize_t n;
    if (b->input_len >= sizeof(b->input) - 1U) return -1;
    n = read(b->fd, b->input + b->input_len,
             sizeof(b->input) - b->input_len - 1U);
    if (n > 0) {
      b->input_len += (size_t)n;
      b->input[b->input_len] = '\0';
      continue;
    }
    if (n == 0) return 1;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
  }
}

short action_secret_broker_events(const ActionSecretBroker *b) {
  if (!b || b->fd < 0 || b->closed) return 0;
  return (short)(POLLIN |
                 (b->output_sent < b->output_len ? POLLOUT : 0));
}

int action_secret_broker_on_events(ActionSecretBroker *b, short events) {
  int rc;
  if (!b || b->fd < 0 || b->closed) return -1;
  if ((events & POLLOUT) && flush_output(b) != 0) goto fail;
  if (events & (POLLIN | POLLHUP | POLLERR)) {
    rc = read_input(b);
    if (rc != 0) goto fail;
  }
  if (b->output_len == b->output_sent && handle_request(b) != 0) goto fail;
  if (flush_output(b) != 0) goto fail;
  return 0;
fail:
  action_secret_broker_close(b);
  return -1;
}

void action_secret_broker_close(ActionSecretBroker *b) {
  if (!b) return;
  if (b->fd >= 0) close(b->fd);
  b->fd = -1;
  b->closed = 1;
  secret_store_zero(b->input, sizeof(b->input));
  secret_store_zero(b->output, sizeof(b->output));
  b->input_len = b->output_len = b->output_sent = 0;
}
