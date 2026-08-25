/* Telegram Bot API adapter — inbound long poll, config, and the shared
 * HTTPS connection state machine both directions use. */

#include "telegram_internal.h"

#include "../../runtime/bus/bus.h"
#include "../../runtime/runtime.h"
#include "../../runtime/secure/auth.h"
#include "../../runtime/secure/secret_store.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/http1.h"
#include "../../runtime/support/json.h"
#include "../../runtime/support/net.h"
#include "../../runtime/support/timing.h"
#include "../../runtime/version.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ----- small shared helpers ----- */

FcTls *tg_tls_client_new(TgAdapter *a, int fd) {
  if (!a) return NULL;
  return a->tls_verify ? fc_tls_client_new(fd, a->api_host)
                       : fc_tls_client_new_local_insecure(fd, a->api_host);
}

int tg_resolve_token(TgAdapter *a, char *out, size_t out_len) {
  if (!a || !a->token_key[0] || !out || out_len == 0) return -1;
  return auth_resolve_static_secret(a->token_key, out, out_len);
}

static short tg_want_events(FcTlsResult r) {
  return r == FC_TLS_WANT_WRITE ? POLLOUT : POLLIN;
}

void tg_conn_close(TgConn *c) {
  if (!c) return;
  if (c->tls) { fc_tls_free(c->tls); c->tls = NULL; }
  if (c->fd >= 0) { net_close(c->fd); c->fd = -1; }
  c->state = TG_CONN_IDLE;
  c->plain = 0;
  c->want = 0;
  c->tx_len = 0;
  c->rx_len = 0;
  c->read_started_ms = 0;
}

int tg_conn_start(TgAdapter *a, TgConn *c, uint64_t now_ms) {
  int fd = -1;
  if (!a || !c || c->state != TG_CONN_IDLE || now_ms < c->retry_after_ms)
    return 0;
  if (net_connect_nb(a->api_host, a->api_port, &fd) != 0 || fd < 0) {
    c->retry_after_ms = now_ms + TG_RETRY_MS;
    return -1;
  }
  c->fd = fd;
  c->plain = a->api_plain;
  c->state = TG_CONN_TCP;
  c->want = POLLOUT;
  c->rx_len = 0;
  return 0;
}

static int tg_flush(TgConn *c) {
  if (c->plain) {
    while (c->tx_len > 0) {
      ssize_t w = write(c->fd, c->tx, c->tx_len);
      if (w > 0) {
        memmove(c->tx, c->tx + w, c->tx_len - (size_t)w);
        c->tx_len -= (size_t)w;
        continue;
      }
      if (w < 0 && errno == EINTR) continue;
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        c->want = POLLOUT;
        return 0;
      }
      return -1;
    }
    c->want = POLLIN;
    return 0;
  }
  while (c->tx_len > 0) {
    size_t wrote = 0;
    FcTlsResult tr = fc_tls_write(c->tls, c->tx, c->tx_len, &wrote);
    if (wrote > 0) {
      memmove(c->tx, c->tx + wrote, c->tx_len - wrote);
      c->tx_len -= wrote;
    }
    if (tr == FC_TLS_OK) continue;
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      c->want = tg_want_events(tr);
      return 0;
    }
    return -1;
  }
  c->want = POLLIN;
  return 0;
}

/* Returns 1 when the peer closed, 0 when more may arrive, -1 on error. */
int tg_read(TgConn *c) {
  if (c->plain) {
    for (;;) {
      ssize_t n;
      if (c->rx_len >= TG_RX_MAX - 1U) return -1;
      n = read(c->fd, c->rx + c->rx_len, TG_RX_MAX - 1U - c->rx_len);
      if (n > 0) { c->rx_len += (size_t)n; continue; }
      if (n == 0) return 1;
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        c->want = POLLIN;
        return 0;
      }
      return -1;
    }
  }
  for (;;) {
    size_t n = 0;
    FcTlsResult tr;
    if (c->rx_len >= TG_RX_MAX - 1U) return -1;
    tr = fc_tls_read(c->tls, c->rx + c->rx_len, TG_RX_MAX - 1U - c->rx_len, &n);
    if (n > 0) c->rx_len += n;
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      c->want = tg_want_events(tr);
      return 0;
    }
    if (tr == FC_TLS_CLOSED) return 1;
    if (tr == FC_TLS_ERROR || tr == FC_TLS_NOT_BUILT) return -1;
    if (n == 0) return 0;
  }
}

/* Advance one connection through connect -> handshake -> write. Returns 1
 * when it has reached READING (the caller then parses), 0 otherwise, -1 on
 * a failure that closed the connection. */
int tg_conn_drive(TgAdapter *a, TgConn *c, int revents, uint64_t now_ms,
                  const char *what) {
  if (!a || !c) return -1;
  if (c->state == TG_CONN_TCP && (revents & (POLLOUT | POLLERR | POLLHUP))) {
    if (net_check_connected(c->fd) != 1) {
      tg_conn_close(c);
      c->retry_after_ms = now_ms + TG_RETRY_MS;
      return -1;
    }
    if (c->plain) {
      c->state = TG_CONN_WRITING;
      c->want = POLLOUT;
    } else {
      c->tls = tg_tls_client_new(a, c->fd);
      if (!c->tls) {
        (void)rt_narrate("telegram: TLS client setup failed for %s",
                         a->api_host);
        tg_conn_close(c);
        c->retry_after_ms = now_ms + TG_RETRY_MS;
        return -1;
      }
      c->state = TG_CONN_TLS;
      c->want = POLLOUT;
    }
  }
  if (c->state == TG_CONN_TLS && (revents & (POLLIN | POLLOUT))) {
    FcTlsResult tr = fc_tls_handshake(c->tls);
    if (tr == FC_TLS_OK) {
      c->state = TG_CONN_WRITING;
      c->want = POLLOUT;
    } else if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      c->want = tg_want_events(tr);
    } else {
      char reason[256];
      if (fc_tls_verify_error(c->tls, reason, sizeof(reason)) == 1)
        (void)rt_narrate("telegram: %s TLS verification failed for %s: %s",
                         what, a->api_host, reason);
      tg_conn_close(c);
      c->retry_after_ms = now_ms + TG_RETRY_MS;
      return -1;
    }
  }
  if (c->state == TG_CONN_WRITING && (revents & POLLOUT)) {
    if (tg_flush(c) != 0) {
      tg_conn_close(c);
      c->retry_after_ms = now_ms + TG_RETRY_MS;
      return -1;
    }
    if (c->tx_len == 0) {
      c->state = TG_CONN_READING;
      c->read_started_ms = now_ms;
    }
  }
  return c->state == TG_CONN_READING ? 1 : 0;
}

/* ----- conversation key ----- */

void tg_context_id(const char *chat_type, const char *chat_id,
                   const char *user_id, char *out, size_t out_len) {
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (chat_type && strcmp(chat_type, "private") == 0) {
    if (user_id && user_id[0]) snprintf(out, out_len, "dm:%s", user_id);
    return;
  }
  if (chat_id && chat_id[0]) snprintf(out, out_len, "%s", chat_id);
}

int tg_chat_allowed(const TgAdapter *a, const char *chat_type,
                    const char *chat_id) {
  int i;
  if (!a || !chat_type || !chat_id) return 0;
  if (strcmp(chat_type, "private") == 0) return a->allow_dms ? 1 : 0;
  /* Group traffic requires an explicit allowlist — same posture as
   * Discord. An unlisted group is silently someone else's server. */
  for (i = 0; i < a->allowed_chat_count; ++i)
    if (strcmp(a->allowed_chat_ids[i], chat_id) == 0) return 1;
  return 0;
}

/* ----- offset cursor ----- */

static void tg_offset_load(TgAdapter *a) {
  char *text = NULL;
  if (a->offset_loaded) return;
  a->offset_loaded = 1;
  a->offset = 0;
  if (fs_read_text(TG_OFFSET_PATH, &text, 64) == 0 && text) {
    long long v = atoll(text);
    if (v > 0) a->offset = v;
    free(text);
  }
}

/* Persisted only after bus_publish returns: publication is the commit, so a
 * crash between the two replays the update rather than losing it. */
static void tg_offset_store(TgAdapter *a, long long next) {
  char buf[32];
  if (next <= a->offset) return;
  a->offset = next;
  snprintf(buf, sizeof(buf), "%lld\n", next);
  (void)fs_mkdir_p(".fclaw/run");
  (void)fs_write_text_atomic(TG_OFFSET_PATH, buf);
}

/* ----- inbound ----- */

static int tg_build_poll_request(TgAdapter *a) {
  char token[8192];
  char path[512];
  int rc = -1;
  memset(token, 0, sizeof(token));
  if (tg_resolve_token(a, token, sizeof(token)) != 0) goto done;
  if (snprintf(path, sizeof(path),
               "/bot%s/getUpdates?timeout=%d&offset=%lld"
               "&allowed_updates=%%5B%%22message%%22%%5D",
               token, TG_LONG_POLL_S, a->offset) >= (int)sizeof(path))
    goto done;
  rc = fc_http1_build_request(a->poll.tx, sizeof(a->poll.tx), "GET", path,
                              a->api_host_header, "floofclaw/" FCLAW_VERSION,
                              NULL, NULL, NULL);
  if (rc == 0) a->poll.tx_len = strlen(a->poll.tx);
  secret_store_zero(path, sizeof(path));
done:
  secret_store_zero(token, sizeof(token));
  return rc;
}

/* One message from the getUpdates batch. Returns 1 when published. */
static int tg_publish_message(TgAdapter *a, const JsonRef *msg) {
  char text[TG_TEXT_MAX] = "";
  char chat_id[64] = "", chat_type[32] = "", user_id[64] = "";
  char message_id[64] = "";
  char context_id[128] = "";
  char etext[TG_TEXT_MAX * 2], echat[128], ectx[256], eadapter[128];
  char payload[TG_TEXT_MAX * 2 + 1024];
  char bus_id[BUS_ID_MAX];
  JsonRef chat, from;
  long long numeric = 0;
  int is_bot = 0;

  if (json_ref_object_get_string(msg, "text", text, sizeof(text)) != 0 ||
      !text[0])
    return 0; /* non-text update: not v1 traffic */
  if (json_ref_object_get_object(msg, "chat", &chat) != 0) return 0;
  if (json_ref_object_get_long(&chat, "id", &numeric) != 0) return 0;
  snprintf(chat_id, sizeof(chat_id), "%lld", numeric);
  (void)json_ref_object_get_string(&chat, "type", chat_type,
                                   sizeof(chat_type));
  if (json_ref_object_get_object(msg, "from", &from) == 0) {
    (void)json_ref_object_get_bool(&from, "is_bot", &is_bot);
    if (json_ref_object_get_long(&from, "id", &numeric) == 0)
      snprintf(user_id, sizeof(user_id), "%lld", numeric);
  }
  if (json_ref_object_get_long(msg, "message_id", &numeric) == 0)
    snprintf(message_id, sizeof(message_id), "%lld", numeric);

  if (is_bot && !a->allow_bots) {
    (void)rt_narrate("telegram: dropped a bot-authored message in chat %s",
                     chat_id);
    return 0;
  }
  if (!tg_chat_allowed(a, chat_type, chat_id)) {
    (void)rt_narrate(
        "telegram: dropped a message from unallowed %s chat %s; fix: add it "
        "to channels.telegram.allowed_chat_ids (or set allow_dms)",
        chat_type[0] ? chat_type : "unknown", chat_id);
    return 0;
  }
  tg_context_id(chat_type, chat_id, user_id, context_id, sizeof(context_id));
  if (!context_id[0]) return 0;

  if (json_escape(text, etext, sizeof(etext)) != 0 ||
      json_escape(chat_id, echat, sizeof(echat)) != 0 ||
      json_escape(context_id, ectx, sizeof(ectx)) != 0 ||
      json_escape(a->module_id_buf, eadapter, sizeof(eadapter)) != 0)
    return 0;
  if (snprintf(payload, sizeof(payload),
               "{\"text\":\"%s\",\"adapter_id\":\"%s\","
               "\"context_id\":\"%s\","
               "\"ref\":{\"chat_id\":\"%s\",\"message_id\":\"%s\"}}",
               etext, eadapter, ectx, echat, message_id) >=
      (int)sizeof(payload))
    return 0;
  if (bus_publish("telegram", "user_message", payload, bus_id,
                  sizeof(bus_id)) != 0) {
    (void)rt_narrate("telegram: failed to publish inbound message");
    return -1;
  }
  return 1;
}

/* Parse one getUpdates body and publish what it carries. */
static void tg_consume_updates(TgAdapter *a, const char *body) {
  JsonRef root, result, item, message;
  int ok = 0;
  long long highest = 0;
  size_t i;
  if (json_ref_top_object(body, &root) != 0) {
    (void)rt_narrate("telegram: getUpdates reply was not a JSON object");
    return;
  }
  if (json_ref_object_get_bool(&root, "ok", &ok) != 0 || !ok) {
    char description[256] = "";
    (void)json_ref_object_get_string(&root, "description", description,
                                     sizeof(description));
    (void)rt_narrate("telegram: getUpdates refused: %s",
                     description[0] ? description : "no description");
    return;
  }
  if (json_ref_object_get_array(&root, "result", &result) != 0) return;
  for (i = 0; i < json_ref_array_size(&result); ++i) {
    long long update_id = 0;
    if (json_ref_array_get(&result, i, &item) != 0) continue;
    if (json_ref_object_get_long(&item, "update_id", &update_id) != 0)
      continue;
    if (json_ref_object_get_object(&item, "message", &message) == 0) {
      if (tg_publish_message(a, &message) < 0) {
        /* Publication failed: stop here and retry this update, rather than
         * acking past a message that never reached the bus. */
        break;
      }
    }
    if (update_id >= highest) highest = update_id;
    /* The offset acks server-side; advance only past published work. */
    tg_offset_store(a, highest + 1);
  }
}

/* ----- reactor module ----- */

static int tg_init(FcReactorModule *m) {
  TgAdapter *a = (TgAdapter *)m->state;
  tg_offset_load(a);
  (void)rt_narrate("telegram: adapter ready (offset %lld)", a->offset);
  return 0;
}

static int tg_collect_fds(FcReactorModule *m, FcPollSet *ps) {
  TgAdapter *a = (TgAdapter *)m->state;
  if (a->poll.fd >= 0 && a->poll.want)
    (void)fc_pollset_add(ps, a->poll.fd, a->poll.want, m, &a->poll);
  if (a->send.fd >= 0 && a->send.want)
    (void)fc_pollset_add(ps, a->send.fd, a->send.want, m, &a->send);
  return 0;
}

static void tg_drive_poll(TgAdapter *a, int revents, uint64_t now_ms) {
  FcHttp1Response resp;
  FcHttp1Status status;
  int rc, closed;
  if (a->poll.state == TG_CONN_IDLE) return;
  if (tg_conn_drive(a, &a->poll, revents, now_ms, "getUpdates") < 0) return;
  if (a->poll.state == TG_CONN_WRITING && a->poll.tx_len == 0) return;
  if (a->poll.state != TG_CONN_READING || !(revents & POLLIN)) return;
  rc = tg_read(&a->poll);
  if (rc < 0) {
    tg_conn_close(&a->poll);
    a->poll.retry_after_ms = now_ms + TG_RETRY_MS;
    return;
  }
  closed = rc > 0;
  status = fc_http1_parse_response(a->poll.rx, a->poll.rx_len, a->body,
                                   TG_BODY_MAX, &resp);
  if (status == FC_HTTP1_NEED_MORE && !closed) return;
  if (status == FC_HTTP1_BAD || (status == FC_HTTP1_NEED_MORE && closed &&
                                 resp.status == 0)) {
    (void)rt_narrate("telegram: getUpdates reply was not readable HTTP: %s",
                     resp.error[0] ? resp.error : "closed mid-response");
    tg_conn_close(&a->poll);
    a->poll.retry_after_ms = now_ms + TG_RETRY_MS;
    return;
  }
  if (resp.status == 200) {
    fc_reconnect_backoff_mark_connected(&a->poll_backoff, now_ms);
    tg_consume_updates(a, a->body);
    a->poll_next_ms = now_ms;
  } else if (resp.status == 429) {
    char retry[32] = "";
    unsigned long long wait_ms = TG_RETRY_MS;
    if (fc_http1_header(a->poll.rx, a->poll.rx_len, "retry-after", retry,
                        sizeof(retry)) == 0 && retry[0])
      wait_ms = (unsigned long long)atoll(retry) * 1000ULL;
    (void)rt_narrate("telegram: getUpdates rate limited; waiting %llums",
                     wait_ms);
    a->poll_next_ms = now_ms + wait_ms;
  } else {
    FcReconnectDelay d = fc_reconnect_backoff_next(&a->poll_backoff, now_ms);
    (void)rt_narrate("telegram: getUpdates failed status=%d; retrying in %llums",
                     resp.status, (unsigned long long)d.delay_ms);
    a->poll_next_ms = now_ms + d.delay_ms;
  }
  tg_conn_close(&a->poll);
}

static int tg_on_fd(FcReactorModule *m, int fd, int events, void *tag) {
  TgAdapter *a = (TgAdapter *)m->state;
  uint64_t now = fc_now_ms();
  (void)fd;
  if (tag == &a->poll) tg_drive_poll(a, events, now);
  else if (tag == &a->send) tg_drive_send(a, events, now);
  return 0;
}

static int tg_tick(FcReactorModule *m, uint64_t now_ms) {
  TgAdapter *a = (TgAdapter *)m->state;
  /* Inbound: re-issue the long poll whenever nothing is in flight. */
  if (a->poll.state == TG_CONN_IDLE && now_ms >= a->poll_next_ms) {
    if (tg_conn_start(a, &a->poll, now_ms) == 0 &&
        a->poll.state == TG_CONN_TCP) {
      if (tg_build_poll_request(a) != 0) {
        (void)rt_narrate(
            "telegram: could not build getUpdates; fix: set the bot token "
            "with `fclaw auth set-stdin %s`", a->token_key);
        tg_conn_close(&a->poll);
        a->poll.retry_after_ms = now_ms + TG_RETRY_MS;
      }
    }
  }
  if (a->poll.state == TG_CONN_READING &&
      net_progress_timed_out(a->poll.read_started_ms, now_ms,
                             TG_PROGRESS_TIMEOUT_MS)) {
    (void)rt_narrate("telegram: getUpdates made no progress; reconnecting");
    tg_conn_close(&a->poll);
    a->poll.retry_after_ms = now_ms + TG_RETRY_MS;
  }
  /* Outbound: pick up new deliveries, then push the queue. */
  tg_drain_deliveries(a);
  if (a->send.state == TG_CONN_IDLE && tg_send_pending(a))
    tg_drive_send(a, 0, now_ms);
  if (a->send.state == TG_CONN_READING &&
      net_progress_timed_out(a->send.read_started_ms, now_ms,
                             TG_PROGRESS_TIMEOUT_MS)) {
    (void)rt_narrate("telegram: sendMessage made no progress; retrying");
    tg_conn_close(&a->send);
    a->send.retry_after_ms = now_ms + TG_RETRY_MS;
  }
  return 0;
}

static uint64_t tg_next_deadline(FcReactorModule *m, uint64_t now_ms) {
  TgAdapter *a = (TgAdapter *)m->state;
  uint64_t next = FC_NO_DEADLINE;
  if (a->poll.state == TG_CONN_IDLE)
    next = a->poll_next_ms > now_ms ? a->poll_next_ms : now_ms;
  if (a->out_count > 0 || a->send.state != TG_CONN_IDLE) return now_ms;
  return next;
}

static void tg_shutdown(FcReactorModule *m) {
  TgAdapter *a = (TgAdapter *)m->state;
  if (!a) return;
  tg_conn_close(&a->poll);
  tg_conn_close(&a->send);
}

/* ----- config + factory ----- */

static int tg_load_config(TgAdapter *a) {
  char *text = NULL;
  JsonRef root, channels, tg, allowed;
  int enabled = 0;
  memset(a, 0, sizeof(*a));
  a->poll.fd = -1;
  a->send.fd = -1;
  a->tls_verify = 1;
  snprintf(a->api_host, sizeof(a->api_host), "%s", TG_HOST);
  snprintf(a->api_host_header, sizeof(a->api_host_header), "%s", TG_HOST);
  a->api_port = TG_PORT;
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "telegram-main");
  if (fs_read_text("config/floofclaw_config.json", &text,
                   FS_READ_TEXT_DEFAULT_CAP) != 0 || !text)
    return -1;
  if (json_ref_first_object(text, &root) != 0) { free(text); return -1; }
  /* No channels block, or no telegram block inside it, is "not configured"
   * — not an error. Every config written before this adapter existed lacks
   * the key, and adding an adapter must never stop an existing deployment
   * from starting. Only an unreadable config file is a hard failure. */
  if (json_ref_object_get_object(&root, "channels", &channels) != 0 ||
      json_ref_object_get_object(&channels, "telegram", &tg) != 0) {
    free(text);
    return 0;
  }
  (void)json_ref_object_get_bool(&tg, "enabled", &enabled);
  if (!enabled) { free(text); return 0; }
  (void)json_ref_object_get_string(&tg, "token_key", a->token_key,
                                   sizeof(a->token_key));
  (void)json_ref_object_get_bool(&tg, "allow_dms", &a->allow_dms);
  (void)json_ref_object_get_bool(&tg, "allow_bots", &a->allow_bots);
  (void)json_ref_object_get_bool(&tg, "tls_verify", &a->tls_verify);
  {
    /* Loopback test seam. api_base is refused entirely unless the hermetic
     * switch is set, and then accepts exactly http://127.0.0.1:<port> —
     * the same posture as FCLAW_MEDIA_TEST_ALLOW_LOOPBACK. */
    char base[160] = "";
    (void)json_ref_object_get_string(&tg, "api_base", base, sizeof(base));
    if (base[0]) {
      const char *sw = getenv("FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK");
      char *end = NULL;
      long port = 0;
      if (!sw || strcmp(sw, "1") != 0) {
        fprintf(stderr,
                "telegram: channels.telegram.api_base is a hermetic test "
                "seam; fix: remove it, or run under "
                "FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK=1 in an isolated test\n");
        free(text);
        return -1;
      }
      if (strncmp(base, "http://127.0.0.1:", 17) != 0 ||
          (port = strtol(base + 17, &end, 10)) <= 0 || port > 65535 ||
          !end || (*end != '\0' && strcmp(end, "/") != 0)) {
        fprintf(stderr,
                "telegram: api_base must be exactly "
                "http://127.0.0.1:<port>; got %s\n", base);
        free(text);
        return -1;
      }
      a->api_plain = 1;
      snprintf(a->api_host, sizeof(a->api_host), "127.0.0.1");
      a->api_port = (int)port;
      snprintf(a->api_host_header, sizeof(a->api_host_header),
               "127.0.0.1:%ld", port);
    }
  }
  if (json_ref_object_get_array(&tg, "allowed_chat_ids", &allowed) == 0) {
    size_t i;
    for (i = 0; i < json_ref_array_size(&allowed) &&
                a->allowed_chat_count < TG_ALLOWED_MAX; ++i) {
      JsonRef item;
      if (json_ref_array_get(&allowed, i, &item) != 0) continue;
      if (json_ref_string_copy(&item,
                               a->allowed_chat_ids[a->allowed_chat_count],
                               sizeof(a->allowed_chat_ids[0])) == 0)
        a->allowed_chat_count++;
    }
  }
  if (!a->token_key[0])
    snprintf(a->token_key, sizeof(a->token_key), "telegram_token");
  free(text);
  a->enabled = 1;
  return 1;
}

static FcReactorModule *tg_create(struct RtScheduler *sched,
                                  int *startup_error) {
  FcReactorModule *module;
  TgAdapter *a;
  int rc;
  (void)sched;
  if (startup_error) *startup_error = 0;
  a = (TgAdapter *)calloc(1, sizeof(*a));
  if (!a) { if (startup_error) *startup_error = 1; return NULL; }
  rc = tg_load_config(a);
  if (rc < 0) { if (startup_error) *startup_error = 1; free(a); return NULL; }
  if (rc == 0) { free(a); return NULL; }   /* configured but off */
  if (!a->api_plain && !fc_tls_available()) {
    fprintf(stderr,
            "telegram: the Bot API is HTTPS-only but OpenSSL support is "
            "missing; fix: install OpenSSL 3 and run make clean && make, or "
            "set channels.telegram.enabled to false\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  if (!a->allow_dms && a->allowed_chat_count == 0) {
    fprintf(stderr,
            "telegram: enabled with no reachable chats; fix: set "
            "channels.telegram.allow_dms true or list "
            "channels.telegram.allowed_chat_ids\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  a->poll.rx = (char *)calloc(1, TG_RX_MAX);
  a->send.rx = (char *)calloc(1, TG_RX_MAX);
  a->body = (char *)calloc(1, TG_BODY_MAX);
  module = (FcReactorModule *)calloc(1, sizeof(*module));
  if (!a->poll.rx || !a->send.rx || !a->body || !module) {
    free(a->poll.rx); free(a->send.rx); free(a->body); free(module); free(a);
    if (startup_error) *startup_error = 1;
    return NULL;
  }
  fc_reconnect_backoff_init(&a->poll_backoff, "telegram");
  module->name = "telegram";
  module->module_id = a->module_id_buf;
  module->state = a;
  module->init = tg_init;
  module->collect_fds = tg_collect_fds;
  module->on_fd = tg_on_fd;
  module->tick = tg_tick;
  module->shutdown = tg_shutdown;
  module->next_deadline_ms = tg_next_deadline;
  return module;
}

static void tg_destroy(FcReactorModule *module) {
  TgAdapter *a;
  if (!module) return;
  a = (TgAdapter *)module->state;
  if (a) {
    tg_conn_close(&a->poll);
    tg_conn_close(&a->send);
    free(a->poll.rx);
    free(a->send.rx);
    free(a->body);
    free(a);
  }
  free(module);
}

const FcAdapter fc_adapter_telegram = {
  .name    = "telegram",
  .create  = tg_create,
  .destroy = tg_destroy,
};

/* ----- test hooks -----
 *
 * The live path is TLS-only against api.telegram.org, so these expose the
 * decisions that do not need a socket: which conversation an update belongs
 * to, whether it is allowed, whether it reaches the bus, and when the
 * offset may advance. */

void tg_test_consume_updates(TgAdapter *a, const char *body) {
  if (a && body) tg_consume_updates(a, body);
}

long long tg_test_offset(const TgAdapter *a) { return a ? a->offset : -1; }

TgAdapter *tg_test_adapter_new(int allow_dms, int allow_bots,
                               const char *const *allowed, int allowed_count) {
  TgAdapter *a = (TgAdapter *)calloc(1, sizeof(*a));
  int i;
  if (!a) return NULL;
  a->poll.fd = -1;
  a->send.fd = -1;
  a->enabled = 1;
  a->tls_verify = 1;
  snprintf(a->api_host, sizeof(a->api_host), "%s", TG_HOST);
  snprintf(a->api_host_header, sizeof(a->api_host_header), "%s", TG_HOST);
  a->api_port = TG_PORT;
  a->allow_dms = allow_dms;
  a->allow_bots = allow_bots;
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "telegram-main");
  snprintf(a->token_key, sizeof(a->token_key), "telegram_token");
  for (i = 0; i < allowed_count && i < TG_ALLOWED_MAX; ++i)
    snprintf(a->allowed_chat_ids[i], sizeof(a->allowed_chat_ids[0]), "%s",
             allowed[i]);
  a->allowed_chat_count = i;
  a->offset_loaded = 1;
  return a;
}

void tg_test_adapter_free(TgAdapter *a) {
  if (!a) return;
  free(a->poll.rx);
  free(a->send.rx);
  free(a->body);
  free(a);
}

int tg_test_out_count(const TgAdapter *a) { return a ? a->out_count : -1; }

const char *tg_test_out_text(const TgAdapter *a, int index) {
  if (!a || index < 0 || index >= a->out_count) return NULL;
  return a->outq[(a->out_head + index) % TG_OUT_QUEUE_MAX].text;
}

const char *tg_test_out_chat(const TgAdapter *a, int index) {
  if (!a || index < 0 || index >= a->out_count) return NULL;
  return a->outq[(a->out_head + index) % TG_OUT_QUEUE_MAX].chat_id;
}
