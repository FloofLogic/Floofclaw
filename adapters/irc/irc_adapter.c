/* IRC channel adapter — single-process nonblocking reactor module.
 * State machine driven by the reactor's on_fd / tick. */

#include "irc_adapter.h"

#include "../../runtime/bus/bus.h"
#include "../../runtime/runtime.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/json.h"
#include "../../runtime/support/net.h"
#include "../../runtime/support/net_tls.h"
#include "../../runtime/support/reconnect_backoff.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define IRC_RX_BUF      8192
#define IRC_TX_BUF      32768
#define IRC_MAX_CHANNELS 8
#define IRC_LINE_MAX    2048
#define IRC_PRIVMSG_MAX_CHARS 380 /* leaves room for PRIVMSG <target> :<text>\r\n */

typedef enum {
  IRC_DISABLED,
  IRC_DISCONNECTED,
  IRC_CONNECTING,
  IRC_TLS_HANDSHAKE,  /* only reached when use_tls is set */
  IRC_REGISTERING,
  IRC_READY,
  IRC_FAILED
} IrcState;

/* Unified return from the read/write helpers so the rest of the
 * adapter doesn't branch on TLS vs raw socket at every call site. */
typedef enum {
  IRC_IO_OK = 0,        /* bytes moved (n_out may still be 0 — try again later) */
  IRC_IO_WOULDBLOCK,    /* re-arm pollset; helper has updated tls_want if needed */
  IRC_IO_CLOSED,        /* peer hung up */
  IRC_IO_ERROR          /* unrecoverable */
} IrcIoResult;

typedef struct {
  /* config */
  char server[128];
  int  port;
  int  use_tls;
  int  tls_verify;
  int  require_mention_in_channel;
  char nick[64];
  char username[64];
  char realname[128];
  char channels[IRC_MAX_CHANNELS][64];
  int  channel_count;

  /* identity */
  char module_id_buf[64];
  /* The nick the server currently knows us by. Starts as the
   * configured nick; if the server rejects with 433/437 during
   * registration we append '_' and try again. Mention detection
   * and self-message filtering match on this. */
  char active_nick[64];
  int  nick_attempt;

  /* state */
  IrcState state;
  int      sock_fd;
  FcTls   *tls;       /* non-NULL once handshake starts; freed on close */
  short    tls_want;  /* POLLIN/POLLOUT hint from the last fc_tls_* call */
  uint64_t state_entered_ms;
  uint64_t next_reconnect_ms;
  FcReconnectBackoff reconnect_backoff;

  /* parser buffers */
  char rxbuf[IRC_RX_BUF];
  size_t rxbuf_len;
  uint64_t partial_line_started_ms;

  char txbuf[IRC_TX_BUF];
  size_t txbuf_len;

  /* delivery cursor (tail of workspace/logs/deliveries.jsonl) */
  long long delivery_cursor;
  int       delivery_cursor_init;
  /* ops narration: mirror workspace/logs/narration.jsonl lines into a
   * configured channel so a person can watch the bot run its shop. */
  char      ops_channel[64];
  long long narration_cursor;
  int       narration_cursor_init;
  /* READY-state liveness: a TCP connection can die without an fd
   * event (sleep, NAT timeout, silent reset), leaving a zombie READY
   * adapter that never reconnects. Track received bytes; after
   * IRC_SILENCE_PING_MS of silence send our own PING, and after
   * IRC_SILENCE_DROP_MS give up and reconnect. */
  uint64_t  last_rx_ms;
  int       keepalive_ping_sent;
} IrcAdapter;

#define DELIVERIES_LOG "workspace/logs/deliveries.jsonl"

/* ----- helpers ----- */

static int set_state(IrcAdapter *a, IrcState s, uint64_t now_ms) {
  a->state = s;
  a->state_entered_ms = now_ms;
  return 0;
}

static short irc_tls_want_events(FcTlsResult r) {
  if (r == FC_TLS_WANT_READ) return POLLIN;
  if (r == FC_TLS_WANT_WRITE) return POLLOUT;
  return 0;
}

static FcTls *irc_tls_client_new(IrcAdapter *a) {
  if (!a) return NULL;
  return a->tls_verify
             ? fc_tls_client_new(a->sock_fd, a->server)
             : fc_tls_client_new_local_insecure(a->sock_fd, a->server);
}

static void irc_narrate_tls_failure(IrcAdapter *a) {
  char reason[256];
  if (!a || !a->tls) return;
  if (fc_tls_verify_error(a->tls, reason, sizeof(reason)) == 1)
    (void)rt_narrate("irc: TLS certificate verification failed for %s: %s",
                     a->server, reason);
}

static void irc_schedule_reconnect(IrcAdapter *a, uint64_t now_ms) {
  FcReconnectDelay delay;
  if (!a) return;
  delay = fc_reconnect_backoff_next(&a->reconnect_backoff, now_ms);
  a->next_reconnect_ms = now_ms + delay.delay_ms;
  (void)rt_narrate("irc: reconnect in %llus (attempt %u)",
                   (unsigned long long)((delay.delay_ms + 999U) / 1000U),
                   delay.attempt);
}

static void irc_schedule_dns_reconnect(IrcAdapter *a, uint64_t now_ms) {
  FcReconnectDelay delay;
  if (!a) return;
  delay = fc_reconnect_backoff_next(&a->reconnect_backoff, now_ms);
  a->next_reconnect_ms = now_ms + delay.delay_ms;
  (void)rt_narrate(
      "irc: dns lookup failed for %s; reconnect in %llus, attempt %u",
      a->server,
      (unsigned long long)((delay.delay_ms + 999U) / 1000U),
      delay.attempt);
}

/* Tear down the transport and schedule a reconnect. Used by every
 * error path so the cleanup stays in one place. */
static void irc_close_conn(IrcAdapter *a, uint64_t now_ms) {
  if (!a) return;
  if (a->tls) { fc_tls_free(a->tls); a->tls = NULL; }
  if (a->sock_fd >= 0) { net_close(a->sock_fd); a->sock_fd = -1; }
  a->rxbuf_len = 0;
  a->partial_line_started_ms = 0;
  a->txbuf_len = 0;
  a->tls_want = 0;
  a->nick_attempt = 0;
  snprintf(a->active_nick, sizeof(a->active_nick), "%s", a->nick);
  irc_schedule_reconnect(a, now_ms);
  set_state(a, IRC_DISCONNECTED, now_ms);
}

static IrcIoResult irc_io_read(IrcAdapter *a, char *buf, size_t len, size_t *n_out) {
  *n_out = 0;
  if (a->use_tls) {
    size_t n = 0;
    FcTlsResult tr = fc_tls_read(a->tls, buf, len, &n);
    *n_out = n;
    if (tr == FC_TLS_OK) { a->tls_want = 0; return IRC_IO_OK; }
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      a->tls_want = irc_tls_want_events(tr);
      return IRC_IO_WOULDBLOCK;
    }
    if (tr == FC_TLS_CLOSED) return IRC_IO_CLOSED;
    return IRC_IO_ERROR;
  } else {
    ssize_t n = net_read_nb(a->sock_fd, buf, len);
    if (n > 0) { *n_out = (size_t)n; return IRC_IO_OK; }
    if (n == 0) return IRC_IO_CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IRC_IO_WOULDBLOCK;
    return IRC_IO_ERROR;
  }
}

static IrcIoResult irc_io_write(IrcAdapter *a, const char *buf, size_t len, size_t *n_out) {
  *n_out = 0;
  if (a->use_tls) {
    size_t n = 0;
    FcTlsResult tr = fc_tls_write(a->tls, buf, len, &n);
    *n_out = n;
    if (tr == FC_TLS_OK) { a->tls_want = 0; return IRC_IO_OK; }
    if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
      a->tls_want = irc_tls_want_events(tr);
      return IRC_IO_WOULDBLOCK;
    }
    return IRC_IO_ERROR;
  } else {
    ssize_t n = net_write_nb(a->sock_fd, buf, len);
    if (n >= 0) { *n_out = (size_t)n; return IRC_IO_OK; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return IRC_IO_WOULDBLOCK;
    return IRC_IO_ERROR;
  }
}

static int append_tx(IrcAdapter *a, const char *bytes, size_t n) {
  if (a->txbuf_len + n > sizeof(a->txbuf)) return -1;
  memcpy(a->txbuf + a->txbuf_len, bytes, n);
  a->txbuf_len += n;
  return 0;
}

static int send_line(IrcAdapter *a, const char *line) {
  size_t n = strlen(line);
  if (append_tx(a, line, n) != 0) return -1;
  return append_tx(a, "\r\n", 2);
}

static int send_linef(IrcAdapter *a, const char *fmt, ...) {
  char buf[IRC_LINE_MAX];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return send_line(a, buf);
}

/* Drain txbuf to the socket. Returns 0 on success (some or all written),
 * -1 on hard error. Stops at WOULDBLOCK. Same flow for TLS and raw —
 * irc_io_write hides the difference and updates tls_want on its own. */
static int drain_tx(IrcAdapter *a) {
  while (a->txbuf_len > 0) {
    size_t wrote = 0;
    IrcIoResult r = irc_io_write(a, a->txbuf, a->txbuf_len, &wrote);
    if (wrote > 0) {
      memmove(a->txbuf, a->txbuf + wrote, a->txbuf_len - wrote);
      a->txbuf_len -= wrote;
    }
    if (r == IRC_IO_WOULDBLOCK) return 0;
    if (r == IRC_IO_ERROR) return -1;
    if (wrote == 0) return 0;
  }
  return 0;
}

/* Sanitize a user-provided string for IRC: strip CR/LF and other control. */
static void sanitize_for_irc(const char *src, char *dst, size_t dst_len) {
  size_t i = 0, j = 0;
  if (!dst || dst_len == 0) return;
  if (!src) src = "";
  while (src[i] && j + 1 < dst_len) {
    unsigned char c = (unsigned char)src[i];
    if (c >= 32 && c != 127) dst[j++] = (char)c;
    i++;
  }
  dst[j] = '\0';
}

/* Lifted from src/channel/irc.c:irc_format_privmsg_lines, simplified.
 * Splits text on word boundaries at IRC_PRIVMSG_MAX_CHARS and queues each
 * resulting "PRIVMSG <target> :<chunk>" line into the tx buffer. */
static int queue_privmsg(IrcAdapter *a, const char *target, const char *text) {
  char clean[IRC_LINE_MAX * 4];
  size_t off, len;
  if (!target || !*target || !text) return -1;
  sanitize_for_irc(text, clean, sizeof(clean));
  len = strlen(clean);
  if (len == 0) return 0;
  off = 0;
  while (off < len) {
    char chunk[IRC_LINE_MAX];
    size_t take = len - off;
    if (take > IRC_PRIVMSG_MAX_CHARS) {
      take = IRC_PRIVMSG_MAX_CHARS;
      while (take > 1 && clean[off + take] != ' ') take--;
      if (take == 1) take = IRC_PRIVMSG_MAX_CHARS;
    }
    if (take >= sizeof(chunk)) take = sizeof(chunk) - 1;
    memcpy(chunk, clean + off, take);
    chunk[take] = '\0';
    if (send_linef(a, "PRIVMSG %s :%s", target, chunk) != 0) return -1;
    off += take;
    while (off < len && clean[off] == ' ') off++;
  }
  return 0;
}

/* ----- inbound parsing ----- */

/* Returns the first line in rxbuf (terminated by \r\n) or NULL.
 * Consumed bytes (including the terminator) are removed from rxbuf. */
static int extract_line(IrcAdapter *a, char *out, size_t out_len) {
  size_t i;
  for (i = 0; i + 1 < a->rxbuf_len; ++i) {
    if (a->rxbuf[i] == '\r' && a->rxbuf[i + 1] == '\n') {
      size_t copy = i;
      if (copy >= out_len) copy = out_len - 1;
      memcpy(out, a->rxbuf, copy);
      out[copy] = '\0';
      memmove(a->rxbuf, a->rxbuf + i + 2, a->rxbuf_len - (i + 2));
      a->rxbuf_len -= (i + 2);
      return 1;
    }
  }
  return 0;
}

static void parse_line(const char *line, char *src, size_t src_len,
                       char *cmd, size_t cmd_len,
                       char params[4][128], int *param_count_out,
                       char *trailing, size_t trailing_len) {
  const char *p = line;
  *param_count_out = 0;
  src[0] = '\0';
  cmd[0] = '\0';
  trailing[0] = '\0';
  if (*p == ':') {
    size_t i = 0;
    p++;
    while (*p && *p != ' ' && i + 1 < src_len) src[i++] = *p++;
    src[i] = '\0';
    while (*p == ' ') p++;
  }
  {
    size_t i = 0;
    while (*p && *p != ' ' && i + 1 < cmd_len) cmd[i++] = *p++;
    cmd[i] = '\0';
    while (*p == ' ') p++;
  }
  while (*p && *param_count_out < 4) {
    if (*p == ':') {
      size_t t = 0;
      p++;
      while (*p && t + 1 < trailing_len) trailing[t++] = *p++;
      trailing[t] = '\0';
      break;
    }
    {
      size_t pi = 0;
      while (*p && *p != ' ' && pi + 1 < 128) params[*param_count_out][pi++] = *p++;
      params[*param_count_out][pi] = '\0';
    }
    (*param_count_out)++;
    while (*p == ' ') p++;
  }
}

/* Extract the nick portion of a PRIVMSG source ("nick!user@host" -> "nick"). */
static void src_nick(const char *src, char *out, size_t out_len) {
  const char *bang;
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!src) return;
  bang = strchr(src, '!');
  if (bang) {
    size_t n = (size_t)(bang - src);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, src, n);
    out[n] = '\0';
  } else {
    snprintf(out, out_len, "%s", src);
  }
}

/* Case-insensitive substring match with word boundaries on both sides.
 * "fclaw" matches "fclaw:", "hi fclaw", "fclaw, hi"; doesn't match
 * "fclawsy" or "myfclaw". '_' counts as alnum so nicks with underscores
 * survive intact. */
static int message_mentions_us(const char *content, const char *nick) {
  size_t nlen;
  const char *p;
  if (!content || !nick || !*nick) return 0;
  nlen = strlen(nick);
  if (nlen == 0) return 0;
  for (p = content; *p; ++p) {
    unsigned char prev, next;
    if (strncasecmp(p, nick, nlen) != 0) continue;
    if (p > content) {
      prev = (unsigned char)p[-1];
      if (isalnum(prev) || prev == '_') continue;
    }
    next = (unsigned char)p[nlen];
    if (isalnum(next) || next == '_') continue;
    return 1;
  }
  return 0;
}

/* Publish an inbound PRIVMSG as a user_message bus envelope. */
static void publish_privmsg(IrcAdapter *a, const char *src, const char *target, const char *text) {
  char nick[64];
  const char *reply_target;
  const char *scope;
  int is_channel;
  char esc_text[IRC_LINE_MAX * 4];
  char esc_server[256];
  char esc_target[256];
  char esc_nick[256];
  char esc_adapter[256];
  char payload[IRC_LINE_MAX * 4 + 1024];
  char id[BUS_ID_MAX];

  src_nick(src, nick, sizeof(nick));
  /* Never treat our own messages as user input. IRC servers normally do not
   * echo a client's PRIVMSG, but ops narration, a bouncer, or a second
   * same-nick connection can surface our own lines — and reading them back as
   * user_messages is a self-sustaining run loop. Drop anything from our nick. */
  if (a->active_nick[0] && strcasecmp(nick, a->active_nick) == 0)
    return;
  is_channel = (target[0] == '#' || target[0] == '&');
  scope = is_channel ? "channel" : "dm";
  reply_target = is_channel ? target : nick;

  if (json_escape(text, esc_text, sizeof(esc_text)) != 0) esc_text[0] = '\0';
  if (json_escape(a->server, esc_server, sizeof(esc_server)) != 0) esc_server[0] = '\0';
  if (json_escape(reply_target, esc_target, sizeof(esc_target)) != 0) esc_target[0] = '\0';
  if (json_escape(nick, esc_nick, sizeof(esc_nick)) != 0) esc_nick[0] = '\0';
  if (json_escape(a->module_id_buf, esc_adapter, sizeof(esc_adapter)) != 0) esc_adapter[0] = '\0';

  snprintf(payload, sizeof(payload),
           "{\"text\":\"%s\",\"adapter_id\":\"%s\","
           "\"ref\":{\"server\":\"%s\",\"target\":\"%s\",\"nick\":\"%s\",\"scope\":\"%s\"}}",
           esc_text, esc_adapter, esc_server, esc_target, esc_nick, scope);
  (void)bus_publish("irc", "user_message", payload, id, sizeof(id));
}

static void handle_irc_line(IrcAdapter *a, const char *line, uint64_t now_ms) {
  char src[256], cmd[64], params[4][128], trailing[IRC_LINE_MAX];
  int pc = 0;
  parse_line(line, src, sizeof(src), cmd, sizeof(cmd), params, &pc, trailing, sizeof(trailing));
  if (cmd[0] == '\0') return;

  if (strcmp(cmd, "PING") == 0) {
    /* :server PING :token  →  PONG :token */
    (void)send_linef(a, "PONG :%s", trailing[0] ? trailing : (pc ? params[0] : ""));
    return;
  }
  if (strcmp(cmd, "001") == 0) {
    /* welcome — proceed to JOIN */
    int i;
    for (i = 0; i < a->channel_count; ++i) {
      (void)send_linef(a, "JOIN %s", a->channels[i]);
    }
    if (a->ops_channel[0])
      (void)send_linef(a, "JOIN %s", a->ops_channel);
    set_state(a, IRC_READY, now_ms);
    fc_reconnect_backoff_mark_connected(&a->reconnect_backoff, now_ms);
    a->last_rx_ms = now_ms;
    a->keepalive_ping_sent = 0;
    return;
  }
  /* Nick-in-use during registration: try a variant. 433 is
   * ERR_NICKNAMEINUSE, 437 is ERR_UNAVAILRESOURCE (often raised
   * when a previous session's nick hasn't timed out yet). */
  if ((strcmp(cmd, "433") == 0 || strcmp(cmd, "437") == 0) &&
      a->state == IRC_REGISTERING) {
    size_t cur = strnlen(a->active_nick, sizeof(a->active_nick));
    if (a->nick_attempt >= 3 || cur + 1 >= sizeof(a->active_nick)) {
      /* Give up; let backoff retry the configured nick fresh. */
      irc_close_conn(a, now_ms);
      return;
    }
    a->nick_attempt++;
    a->active_nick[cur] = '_';
    a->active_nick[cur + 1] = '\0';
    (void)send_linef(a, "NICK %s", a->active_nick);
    return;
  }
  if (strcmp(cmd, "PRIVMSG") == 0 && pc >= 1 && trailing[0]) {
    const char *target = params[0];
    /* '#' covers the modern world; '&' is rare but legal. Other
     * prefixes (+, !, .) are obscure and treated as DM-shaped. */
    int is_channel = (target[0] == '#' || target[0] == '&');
    char from[64] = "";
    src_nick(src, from, sizeof(from));
    /* Drop self-message echoes (some bouncers/relays re-deliver). */
    if (from[0] && strcasecmp(from, a->active_nick) == 0) return;
    /* In a channel, only attend when our nick is mentioned. DMs
     * always attend. Same shape Discord uses. */
    if (is_channel && a->require_mention_in_channel &&
        !message_mentions_us(trailing, a->active_nick)) return;
    publish_privmsg(a, src, target, trailing);
    return;
  }
  /* All other numerics/responses are noise we don't act on. */
}

/* ----- delivery tail ----- */

static int line_to_delivery_for_us(const char *line, IrcAdapter *a,
                                   char *target_out, size_t target_out_len,
                                   char *text_out, size_t text_out_len) {
  JsonRef root, delivery, ref;
  char ch[64], aid[64];
  if (json_ref_first_object(line, &root) != 0) return 0;
  if (json_ref_object_get_object(&root, "delivery", &delivery) != 0) return 0;
  if (json_ref_object_get_string(&delivery, "channel", ch, sizeof(ch)) != 0) return 0;
  if (strcmp(ch, "irc") != 0) return 0;
  if (json_ref_object_get_string(&delivery, "adapter_id", aid, sizeof(aid)) != 0) return 0;
  if (strcmp(aid, a->module_id_buf) != 0) return 0;
  if (json_ref_object_get_string(&delivery, "text", text_out, text_out_len) != 0) return 0;
  if (json_ref_object_get_object(&delivery, "ref", &ref) != 0 ||
      json_ref_object_get_string(&ref, "target", target_out, target_out_len) != 0) {
    /* Proactive delivery (an affair review or operation result speaking
     * up on its own): no inbound ref exists to route back to. Deliver
     * to the adapter's home channel — the first configured autojoin. */
    if (a->channel_count == 0) return 0;
    snprintf(target_out, target_out_len, "%s", a->channels[0]);
  }
  return 1;
}

static void drain_deliveries(IrcAdapter *a) {
  char *t = NULL;
  long long total;
  FILE *fp;
  if (!a->delivery_cursor_init) {
    if (fs_read_text(DELIVERIES_LOG, &t, FS_READ_TEXT_DEFAULT_CAP) == 0 && t) {
      a->delivery_cursor = (long long)strlen(t);
      free(t);
    } else {
      a->delivery_cursor = 0;
    }
    a->delivery_cursor_init = 1;
    return;
  }
  if (fs_read_text(DELIVERIES_LOG, &t, FS_READ_TEXT_DEFAULT_CAP) != 0 || !t) return;
  total = (long long)strlen(t);
  free(t);
  /* Rotation detected: janitor renamed the live file; append site
   * created a fresh one. Reset cursor to top of the new file. */
  if (total < a->delivery_cursor) a->delivery_cursor = 0;
  if (total <= a->delivery_cursor) return;
  fp = fopen(DELIVERIES_LOG, "rb");
  if (!fp) return;
  if (fseek(fp, a->delivery_cursor, SEEK_SET) == 0) {
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
      char target[256] = "", text[IRC_LINE_MAX * 4] = "";
      size_t llen = strlen(line);
      if (llen > 0 && line[llen - 1] == '\n') line[llen - 1] = '\0';
      if (line_to_delivery_for_us(line, a, target, sizeof(target), text, sizeof(text))) {
        (void)queue_privmsg(a, target, text);
      }
    }
  }
  fclose(fp);
  a->delivery_cursor = total;
}

#define NARRATION_LOG "workspace/logs/narration.jsonl"

static void drain_narration(IrcAdapter *a) {
  char *t = NULL;
  long long total;
  FILE *fp;
  if (!a->ops_channel[0]) return;
  if (!a->narration_cursor_init) {
    if (fs_read_text(NARRATION_LOG, &t, FS_READ_TEXT_DEFAULT_CAP) == 0 && t) {
      a->narration_cursor = (long long)strlen(t);
      free(t);
    } else {
      a->narration_cursor = 0;
    }
    a->narration_cursor_init = 1;
    return;
  }
  if (fs_read_text(NARRATION_LOG, &t, FS_READ_TEXT_DEFAULT_CAP) != 0 || !t) return;
  total = (long long)strlen(t);
  free(t);
  /* Rotation detected: the janitor renamed narration.jsonl to .1 and
   * the next append recreated a fresh empty file. Reset to top. */
  if (total < a->narration_cursor) a->narration_cursor = 0;
  if (total <= a->narration_cursor) return;
  fp = fopen(NARRATION_LOG, "rb");
  if (!fp) return;
  if (fseek(fp, a->narration_cursor, SEEK_SET) == 0) {
    char line[8192];
    while (fgets(line, sizeof(line), fp)) {
      JsonRef root;
      char text[IRC_LINE_MAX * 2] = "";
      size_t llen = strlen(line);
      if (llen > 0 && line[llen - 1] == '\n') line[llen - 1] = '\0';
      if (json_ref_first_object(line, &root) == 0 &&
          json_ref_object_get_string(&root, "text", text, sizeof(text)) == 0 &&
          text[0])
        (void)queue_privmsg(a, a->ops_channel, text);
    }
  }
  fclose(fp);
  a->narration_cursor = total;
}

/* ----- adapter callbacks ----- */

static int irc_init(FcReactorModule *adapter) {
  IrcAdapter *a = (IrcAdapter *)adapter->state;
  if (!a) return 0;
  if (a->state == IRC_DISABLED) return 0; /* configured off */
  fc_reconnect_backoff_init(&a->reconnect_backoff, a->module_id_buf);
  a->state = IRC_DISCONNECTED;
  a->next_reconnect_ms = 0; /* try connect on first tick */
  return 0;
}

static int irc_collect_fds(FcReactorModule *adapter, FcPollSet *ps) {
  IrcAdapter *a = (IrcAdapter *)adapter->state;
  if (!a) return 0;
  if (a->state == IRC_CONNECTING) {
    return fc_pollset_add(ps, a->sock_fd, POLLOUT, adapter, NULL);
  }
  if (a->state == IRC_TLS_HANDSHAKE) {
    short events = a->tls_want ? a->tls_want : (POLLIN | POLLOUT);
    return fc_pollset_add(ps, a->sock_fd, events, adapter, NULL);
  }
  if (a->state == IRC_REGISTERING || a->state == IRC_READY) {
    short events = POLLIN;
    if (a->txbuf_len > 0) events |= POLLOUT;
    /* TLS may want the "other" direction mid-renegotiation. Rare in
     * practice but cheap to honor — OR the hint in. */
    if (a->use_tls && a->tls_want) events |= a->tls_want;
    return fc_pollset_add(ps, a->sock_fd, events, adapter, NULL);
  }
  return 0;
}

/* Send NICK + USER. Shared by both transports — when TLS is off
 * we hit this right after the TCP connect; when TLS is on we hit
 * it once the handshake completes. */
static void irc_send_registration(IrcAdapter *a) {
  (void)send_linef(a, "NICK %s", a->active_nick);
  (void)send_linef(a, "USER %s 0 * :%s", a->username, a->realname);
}

static int irc_on_fd(FcReactorModule *adapter, int fd, int revents, void *tag) {
  IrcAdapter *a = (IrcAdapter *)adapter->state;
  uint64_t now;
  (void)fd; (void)tag;
  if (!a) return 0;
  now = fc_now_ms();

  if (a->state == IRC_CONNECTING) {
    if (revents & (POLLOUT | POLLERR | POLLHUP)) {
      int rc = net_check_connected(a->sock_fd);
      if (rc == 1) {
        if (a->use_tls) {
          a->tls = irc_tls_client_new(a);
          if (!a->tls) {
            (void)rt_narrate("irc: TLS client setup failed for %s", a->server);
            irc_close_conn(a, now);
            return 0;
          }
          set_state(a, IRC_TLS_HANDSHAKE, now);
          a->tls_want = POLLOUT;
        } else {
          set_state(a, IRC_REGISTERING, now);
          irc_send_registration(a);
        }
      } else if (rc == -1) {
        irc_close_conn(a, now);
      }
    }
    return 0;
  }

  if (a->state == IRC_TLS_HANDSHAKE) {
    if (revents & (POLLIN | POLLOUT)) {
      FcTlsResult tr = fc_tls_handshake(a->tls);
      if (tr == FC_TLS_OK) {
        a->tls_want = 0;
        set_state(a, IRC_REGISTERING, now);
        irc_send_registration(a);
      } else if (tr == FC_TLS_WANT_READ || tr == FC_TLS_WANT_WRITE) {
        a->tls_want = irc_tls_want_events(tr);
      } else {
        irc_narrate_tls_failure(a);
        irc_close_conn(a, now);
      }
    }
    return 0;
  }

  /* REGISTERING / READY: bidirectional traffic. Always attempt a
   * read, then a write — irc_io_* tells us when to stop. */
  {
    char buf[1024];
    for (;;) {
      size_t n = 0;
      IrcIoResult r = irc_io_read(a, buf, sizeof(buf), &n);
      if (n > 0) {
        int parsed_line = 0;
        a->last_rx_ms = now;
        a->keepalive_ping_sent = 0;
        if (a->rxbuf_len + n > sizeof(a->rxbuf)) {
          /* line longer than IRC_RX_BUF — protocol error or hostile
           * peer. Drop the connection and let backoff retry. */
          irc_close_conn(a, now);
          return 0;
        }
        if (a->rxbuf_len == 0) a->partial_line_started_ms = now;
        memcpy(a->rxbuf + a->rxbuf_len, buf, n);
        a->rxbuf_len += n;
        {
          char line[IRC_LINE_MAX];
          while (extract_line(a, line, sizeof(line))) {
            parsed_line = 1;
            handle_irc_line(a, line, now);
          }
        }
        if (parsed_line)
          a->partial_line_started_ms = a->rxbuf_len > 0 ? now : 0;
      }
      if (r == IRC_IO_WOULDBLOCK) break;
      if (r == IRC_IO_CLOSED || r == IRC_IO_ERROR) { irc_close_conn(a, now); return 0; }
      if (n == 0) break;
    }
  }

  if (a->txbuf_len > 0) {
    if (drain_tx(a) < 0) { irc_close_conn(a, now); return 0; }
  }
  return 0;
}

/* A connection stuck between connect and welcome (lost POLLOUT edge,
 * server holding an unregistered socket open, stalled TLS) never
 * recovers on its own because the socket stays ESTABLISHED. Give the
 * pre-READY states a hard deadline; close + backoff retries cleanly. */
#define IRC_REGISTRATION_TIMEOUT_MS 30000
#define IRC_PARSE_PROGRESS_TIMEOUT_MS 30000
#define IRC_SILENCE_PING_MS  90000
#define IRC_SILENCE_DROP_MS 150000

static int irc_tick(FcReactorModule *adapter, uint64_t now_ms) {
  IrcAdapter *a = (IrcAdapter *)adapter->state;
  if (!a || a->state == IRC_DISABLED) return 0;
  if ((a->state == IRC_CONNECTING || a->state == IRC_TLS_HANDSHAKE ||
       a->state == IRC_REGISTERING) &&
      now_ms - a->state_entered_ms > IRC_REGISTRATION_TIMEOUT_MS) {
    irc_close_conn(a, now_ms);
  }
  if (a->rxbuf_len > 0 &&
      net_progress_timed_out(a->partial_line_started_ms, now_ms,
                             IRC_PARSE_PROGRESS_TIMEOUT_MS)) {
    (void)rt_narrate(
        "irc: partial line made no parse progress for %llus; reconnecting",
        (unsigned long long)(IRC_PARSE_PROGRESS_TIMEOUT_MS / 1000U));
    irc_close_conn(a, now_ms);
  }
  if (a->state == IRC_DISCONNECTED && now_ms >= a->next_reconnect_ms) {
    int fd = -1;
    int connect_rc = net_connect_nb(a->server, a->port, &fd);
    if (connect_rc == 0 && fd >= 0) {
      a->sock_fd = fd;
      set_state(a, IRC_CONNECTING, now_ms);
    } else if (connect_rc == NET_CONNECT_DNS_FAILED) {
      irc_schedule_dns_reconnect(a, now_ms);
    } else {
      irc_schedule_reconnect(a, now_ms);
    }
  }
  if (a->state == IRC_READY) {
    (void)fc_reconnect_backoff_maybe_reset(&a->reconnect_backoff, now_ms);
    if (a->last_rx_ms) {
      uint64_t silent = now_ms - a->last_rx_ms;
      if (silent > IRC_SILENCE_DROP_MS) {
        irc_close_conn(a, now_ms); /* zombie connection: reconnect */
        return 0;
      }
      if (silent > IRC_SILENCE_PING_MS && !a->keepalive_ping_sent) {
        (void)send_linef(a, "PING :fclaw-keepalive");
        a->keepalive_ping_sent = 1;
      }
    }
    drain_deliveries(a);
    drain_narration(a);
    if (a->txbuf_len > 0) (void)drain_tx(a);
  }
  return 0;
}

static uint64_t irc_next_deadline(FcReactorModule *adapter, uint64_t now_ms) {
  IrcAdapter *a = (IrcAdapter *)adapter->state;
  if (!a || a->state == IRC_DISABLED) return FC_NO_DEADLINE;
  if (a->rxbuf_len > 0 && a->partial_line_started_ms > 0)
    return a->partial_line_started_ms + IRC_PARSE_PROGRESS_TIMEOUT_MS;
  if (a->state == IRC_DISCONNECTED && a->next_reconnect_ms > 0) {
    return a->next_reconnect_ms;
  }
  if (a->state == IRC_CONNECTING || a->state == IRC_TLS_HANDSHAKE ||
      a->state == IRC_REGISTERING) {
    return a->state_entered_ms + IRC_REGISTRATION_TIMEOUT_MS;
  }
  if (a->state == IRC_READY && a->last_rx_ms) {
    uint64_t next = a->last_rx_ms +
        (a->keepalive_ping_sent ? IRC_SILENCE_DROP_MS : IRC_SILENCE_PING_MS);
    return next > now_ms ? next : now_ms;
  }
  return FC_NO_DEADLINE;
}

static void irc_shutdown_cb(FcReactorModule *adapter) {
  IrcAdapter *a = (IrcAdapter *)adapter->state;
  if (!a) return;
  if (a->sock_fd >= 0) {
    /* Best-effort QUIT then close. */
    (void)send_line(a, "QUIT :bye");
    (void)drain_tx(a);
    if (a->tls) { fc_tls_free(a->tls); a->tls = NULL; }
    net_close(a->sock_fd);
    a->sock_fd = -1;
  }
}

/* ----- config + factory ----- */

static int load_config(IrcAdapter *a) {
  char *text = NULL;
  JsonRef root, channels, irc, join_arr;
  int enabled = 0;
  long long port = 0;

  memset(a, 0, sizeof(*a));
  a->sock_fd = -1;
  a->tls_verify = 1;
  a->require_mention_in_channel = 1; /* default, override in config */
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "irc-main");

  if (fs_read_text("config/floofclaw_config.json", &text, FS_READ_TEXT_DEFAULT_CAP) != 0 || !text) return -1;
  if (json_ref_first_object(text, &root) != 0) { free(text); return -1; }
  if (json_ref_object_get_object(&root, "channels", &channels) != 0 ||
      json_ref_object_get_object(&channels, "irc", &irc) != 0) {
    free(text);
    return -1;
  }
  (void)json_ref_object_get_bool(&irc, "enabled", &enabled);
  if (!enabled) {
    a->state = IRC_DISABLED;
    free(text);
    return 0;
  }
  (void)json_ref_object_get_string(&irc, "server", a->server, sizeof(a->server));
  (void)json_ref_object_get_long(&irc, "port", &port);
  (void)json_ref_object_get_bool(&irc, "tls", &a->use_tls);
  (void)json_ref_object_get_bool(&irc, "tls_verify", &a->tls_verify);
  (void)json_ref_object_get_bool(&irc, "require_mention_in_channel", &a->require_mention_in_channel);
  /* Default port follows TLS: 6697 for ircs, 6667 for plain. Either
   * can be overridden by an explicit `port` in config. */
  a->port = port > 0 ? (int)port : (a->use_tls ? 6697 : 6667);
  (void)json_ref_object_get_string(&irc, "nick", a->nick, sizeof(a->nick));
  (void)json_ref_object_get_string(&irc, "username", a->username, sizeof(a->username));
  (void)json_ref_object_get_string(&irc, "realname", a->realname, sizeof(a->realname));
  if (!a->nick[0])     snprintf(a->nick, sizeof(a->nick), "fclaw");
  if (!a->username[0]) snprintf(a->username, sizeof(a->username), "fclaw");
  if (!a->realname[0]) snprintf(a->realname, sizeof(a->realname), "FloofClaw");
  snprintf(a->active_nick, sizeof(a->active_nick), "%s", a->nick);
  (void)json_ref_object_get_string(&irc, "ops_channel", a->ops_channel, sizeof(a->ops_channel));
  if (json_ref_object_get_array(&irc, "join", &join_arr) == 0) {
    size_t n = json_ref_array_size(&join_arr), i;
    for (i = 0; i < n && a->channel_count < IRC_MAX_CHANNELS; ++i) {
      JsonRef item;
      if (json_ref_array_get(&join_arr, i, &item) == 0) {
        if (json_ref_string_copy(&item, a->channels[a->channel_count], 64) == 0)
          a->channel_count++;
      }
    }
  }
  free(text);
  if (!a->server[0] || a->port <= 0) {
    fprintf(stderr,
            "irc: enabled but server/port is missing; fix: set "
            "channels.irc.server and channels.irc.port in config/floofclaw_config.json\n");
    return -1;
  }
  a->state = IRC_DISCONNECTED;
  return 1;
}

static FcReactorModule *irc_create(struct RtScheduler *sched,
                                  int *startup_error) {
  (void)sched; /* IRC owns its own socket; the uniform signature is what
                * lets the generated registry need only a name. */
  FcReactorModule *adapter;
  IrcAdapter *a = (IrcAdapter *)calloc(1, sizeof(*a));
  int rc;
  if (startup_error) *startup_error = 0;
  if (!a) { if (startup_error) *startup_error = 1; return NULL; }
  rc = load_config(a);
  if (rc < 0) { if (startup_error) *startup_error = 1; free(a); return NULL; }
  if (rc == 0) {
    /* configured but disabled — return NULL so caller skips registration */
    free(a);
    return NULL;
  }
  if (a->use_tls && !fc_tls_available()) {
    fprintf(stderr, "irc: tls=true but OpenSSL support is missing; fix: install OpenSSL 3 and run make clean && make, or set channels.irc.tls to false\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  if (a->use_tls && !a->tls_verify &&
      !fc_tls_hostname_is_local(a->server)) {
    fprintf(stderr,
            "irc: tls_verify=false is allowed only for localhost; fix: set channels.irc.tls_verify to true\n");
    if (startup_error) *startup_error = 1;
    free(a);
    return NULL;
  }
  adapter = (FcReactorModule *)calloc(1, sizeof(*adapter));
  if (!adapter) { free(a); return NULL; }
  adapter->name = "irc";
  adapter->module_id = a->module_id_buf;
  adapter->state = a;
  adapter->init = irc_init;
  adapter->collect_fds = irc_collect_fds;
  adapter->on_fd = irc_on_fd;
  adapter->tick = irc_tick;
  adapter->shutdown = irc_shutdown_cb;
  adapter->next_deadline_ms = irc_next_deadline;
  return adapter;
}

static void irc_destroy(FcReactorModule *adapter) {
  if (!adapter) return;
  free(adapter->state);
  free(adapter);
}

/* The contract this directory satisfies, stated where you can see it. */
const FcAdapter fc_adapter_irc = {
  .name    = "irc",
  .create  = irc_create,
  .destroy = irc_destroy,
};
