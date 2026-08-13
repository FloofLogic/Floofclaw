#ifndef FCLAW_SECURE_ACTION_SECRET_BROKER_H
#define FCLAW_SECURE_ACTION_SECRET_BROKER_H

#include <stddef.h>

#define ACTION_SECRET_SUFFIX_MAX 192
#define ACTION_SECRET_VALUE_MAX  8192
#define ACTION_SECRET_IO_MAX     32768

typedef struct {
  int fd;
  char action_id[128];
  unsigned char input[ACTION_SECRET_IO_MAX];
  size_t input_len;
  unsigned char output[ACTION_SECRET_IO_MAX];
  size_t output_len;
  size_t output_sent;
  int closed;
} ActionSecretBroker;

/* Create the private full-duplex channel inherited by an action child. */
int action_secret_broker_pair(int fds[2]);

/* Parent-side broker. The action id is trusted registry state and is always
 * prepended by the parent; protocol requests contain suffixes only. */
int action_secret_broker_init(ActionSecretBroker *broker, int fd,
                              const char *action_id);
short action_secret_broker_events(const ActionSecretBroker *broker);
int action_secret_broker_on_events(ActionSecretBroker *broker, short events);
void action_secret_broker_close(ActionSecretBroker *broker);

#endif
