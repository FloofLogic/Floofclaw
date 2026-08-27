#ifndef FCLAW_TELEGRAM_INTERNAL_H
#define FCLAW_TELEGRAM_INTERNAL_H

#include "../../runtime/gateway/adapter.h"
#include "../../runtime/bus/media_manifest.h"
#include "../../runtime/support/json.h"
#include "../../runtime/support/net_tls.h"
#include "../../runtime/support/reconnect_backoff.h"

#include <stdint.h>

/* Telegram Bot API adapter.
 *
 * Simpler than Discord by construction: plain HTTPS both ways — long-poll
 * GET /getUpdates inbound, POST /sendMessage outbound. No websocket, no
 * heartbeat, no RESUME state machine. Self-echo runaways are structurally
 * absent too: getUpdates never returns the bot's own messages, and privacy
 * mode hides other bots.
 *
 * Three independent connections, each its own small state machine, because
 * a 50-second long poll must not block getFile resolution or an outbound
 * reply. */

#define TG_HOST            "api.telegram.org"
#define TG_PORT            443
#define TG_LONG_POLL_S     50
#define TG_RX_MAX          262144   /* getUpdates can return a batch */
#define TG_TX_MAX          16384
#define TG_BODY_MAX        262144
#define TG_TEXT_MAX        8192
#define TG_MESSAGE_CHUNK   4096     /* Telegram's own sendMessage cap */
#define TG_OUT_QUEUE_MAX   64
#define TG_ALLOWED_MAX     16
#define TG_RETRY_MS        5000ULL
#define TG_PROGRESS_TIMEOUT_MS 90000ULL  /* > the 50s poll it must outlive */
#define TG_OFFSET_PATH     ".fclaw/run/telegram_offset"
#define TG_DELIVERIES_LOG  "workspace/logs/deliveries.jsonl"
#define TG_FILE_ID_MAX     256
#define TG_INBOUND_MEDIA_MAX 2U /* Bot API message media fields are singular. */
#define TG_FILE_FETCH_MAX_BYTES (20ULL * 1024ULL * 1024ULL)

typedef enum {
  TG_CONN_IDLE = 0,
  TG_CONN_TCP,
  TG_CONN_TLS,
  TG_CONN_WRITING,
  TG_CONN_READING
} TgConnState;

/* One HTTPS request/response in flight. `plain` is the loopback test seam:
 * no TLS states, plain read/write on the socket. It is set per connection
 * from the adapter's api_plain, which only ever comes from a loopback
 * api_base admitted under FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK=1. */
typedef struct {
  TgConnState state;
  int fd;
  int plain;
  FcTls *tls;
  short want;
  char tx[TG_TX_MAX];
  size_t tx_len;
  char *rx;               /* heap: TG_RX_MAX, only while the adapter lives */
  size_t rx_len;
  uint64_t read_started_ms;
  uint64_t retry_after_ms;
} TgConn;

typedef struct {
  char chat_id[64];
  char text[TG_MESSAGE_CHUNK + 1];
} TgOutbound;

typedef struct {
  char file_id[TG_FILE_ID_MAX];
  char id[FC_MEDIA_ID_MAX];
  char filename[FC_MEDIA_FILENAME_MAX];
  char media_type[FC_MEDIA_TYPE_MAX];
  uint64_t size_bytes;
  char source_url[FC_MEDIA_SOURCE_URL_MAX];
} TgInboundMedia;

typedef struct {
  char text[TG_TEXT_MAX];
  char chat_id[64];
  char chat_type[32];
  char user_id[64];
  char message_id[64];
  int author_is_bot;
  TgInboundMedia media[TG_INBOUND_MEDIA_MAX];
  size_t media_count;
  uint64_t media_bytes;
} TgInboundMessage;

typedef struct {
  int active;
  long long update_id;
  size_t resolve_index;
  TgInboundMessage message;
} TgPendingInbound;

typedef struct {
  int enabled;
  int tls_verify;
  int allow_dms;
  int allow_bots;
  /* Where the Bot API lives. Defaults to api.telegram.org:443 over TLS.
   * channels.telegram.api_base may point this at http://127.0.0.1:<port>,
   * but only when FCLAW_TELEGRAM_TEST_ALLOW_LOOPBACK=1 — a hermetic test
   * seam, never a production override. */
  char api_host[128];
  char api_host_header[160];
  int api_port;
  int api_plain;
  char token_key[128];
  char module_id_buf[64];
  char allowed_chat_ids[TG_ALLOWED_MAX][64];
  int allowed_chat_count;

  TgConn poll;                 /* inbound getUpdates */
  FcReconnectBackoff poll_backoff;
  uint64_t poll_next_ms;
  long long offset;            /* next update_id to request */
  int offset_loaded;

  TgConn file;                 /* getFile metadata resolution */
  TgPendingInbound pending;

  TgConn send;                 /* outbound sendMessage */
  TgOutbound outq[TG_OUT_QUEUE_MAX];
  int out_head;
  int out_count;
  long long delivery_cursor;
  int delivery_cursor_init;

  char *body;                  /* heap: TG_BODY_MAX decode target */
} TgAdapter;

/* telegram_adapter.c */
FcTls *tg_tls_client_new(TgAdapter *a, int fd);
int  tg_resolve_token(TgAdapter *a, char *out, size_t out_len);
void tg_conn_close(TgConn *c);
int  tg_conn_start(TgAdapter *a, TgConn *c, uint64_t now_ms);
int  tg_read(TgConn *c);
int  tg_conn_drive(TgAdapter *a, TgConn *c, int revents, uint64_t now_ms,
                   const char *what);
/* Conversation key: the chat id for a group, "dm:<user_id>" for a private
 * chat. Mirrors dc_context_id(); never let one deployment collapse into a
 * single context. */
void tg_context_id(const char *chat_type, const char *chat_id,
                   const char *user_id, char *out, size_t out_len);
int  tg_chat_allowed(const TgAdapter *a, const char *chat_type,
                     const char *chat_id);
int tg_parse_inbound_message(const JsonRef *msg, TgInboundMessage *out);
int tg_source_url_is_trusted(const char *url, const char *token);

/* Test hook: run one getUpdates response body through the parse, publish,
 * and offset-advance path without a network. */
void tg_test_consume_updates(TgAdapter *a, const char *body);
long long tg_test_offset(const TgAdapter *a);
TgAdapter *tg_test_adapter_new(int allow_dms, int allow_bots,
                               const char *const *allowed, int allowed_count);
void tg_test_adapter_free(TgAdapter *a);
int tg_test_out_count(const TgAdapter *a);
const char *tg_test_out_text(const TgAdapter *a, int index);
const char *tg_test_out_chat(const TgAdapter *a, int index);

/* telegram_outbox.c */
void tg_drain_deliveries(TgAdapter *a);
int  tg_queue_chunks(TgAdapter *a, const char *chat_id, const char *text);
void tg_drive_send(TgAdapter *a, int revents, uint64_t now_ms);
int  tg_send_pending(const TgAdapter *a);

#endif
