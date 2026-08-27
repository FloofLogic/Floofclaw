#ifndef FCLAW_DISCORD_INTERNAL_H
#define FCLAW_DISCORD_INTERNAL_H

/* Discord adapter internals shared across discord_adapter.c (shim +
 * reactor callbacks + helpers), discord_gateway.c (WSS protocol),
 * discord_rest.c (REST POST), and discord_outbox.c (deliveries.jsonl
 * tailer). Not part of any public runtime surface. */

#include "discord_adapter.h"
#include "../../runtime/bus/media_manifest.h"
#include "../../runtime/runtime.h"
#include "../../runtime/support/net_tls.h"
#include "../../runtime/support/reconnect_backoff.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define DC_GATEWAY_HOST "gateway.discord.gg"
#define DC_GATEWAY_PORT 443
#define DC_GATEWAY_PATH "/?v=10&encoding=json"
#define DC_REST_HOST    "discord.com"
#define DC_REST_PORT    443
#define DC_API_PREFIX   "/api/v10"

#define DC_MODULE_ID       "discord-main"
#define DC_INTENTS         37377U
#define DC_RX_MAX          65536
#define DC_TX_MAX          65536
#define DC_TEXT_MAX        4096
#define DC_REPLY_MAX       16384
#define DC_CHUNK_MAX       2000
#define DC_ALLOWED_MAX     16
#define DC_OUT_QUEUE_MAX   32
#define DC_QUEUE_FULL      (-2)
#define DC_REST_RETRY_MS   1500
#define DC_REST_AMBIGUOUS_RETRY_MAX 1U
#define DC_REST_RETRY_AFTER_MAX_MS 86400000ULL
#define DC_PARSE_PROGRESS_TIMEOUT_MS 30000ULL
#define DC_DELIVERIES_LOG  "workspace/logs/deliveries.jsonl"

typedef enum {
  DC_DISABLED = 0,
  DC_DISCONNECTED,
  DC_TCP_CONNECTING,
  DC_TLS_HANDSHAKE,
  DC_WS_HANDSHAKE,
  DC_GATEWAY_OPEN
} DcGatewayState;

typedef enum {
  DC_REST_IDLE = 0,
  DC_REST_TCP_CONNECTING,
  DC_REST_TLS_HANDSHAKE,
  DC_REST_WRITING,
  DC_REST_READING
} DcRestState;

typedef struct {
  char channel_id[128];
  char payload[DC_CHUNK_MAX * 2 + 64];
} DcOutbound;

typedef struct {
  char user_id[64];
  char channel_id[128];
  char guild_id[128];
  char content[DC_TEXT_MAX];
  FcMediaDescriptor *attachments;
  size_t attachment_count;
  uint64_t attachment_bytes;
  int author_is_bot;
  int is_dm;
} DcInboundMessage;

typedef struct {
  long long op;
  long long sequence;
  long long heartbeat_interval_ms;
  int invalid_session_resumable;
  char event_name[64];
} DcGatewayControl;

typedef struct {
  int enabled;
  int debug;
  int allow_bots;
  int allow_dms;
  int require_mention_in_guild;
  int tls_verify;
  char token_key[128];
  char guild_id[128];
  char allowed_channel_ids[DC_ALLOWED_MAX][128];
  /* Proactive deliveries with no inbound ref (affair reviews) go here;
   * for a DM deployment set it to the user's DM channel id. Falls back
   * to allowed_channel_ids[0]. */
  char home_channel_id[128];
  int allowed_channel_count;
  char module_id_buf[64];

  DcGatewayState gw_state;
  int gw_fd;
  FcTls *gw_tls;
  short gw_want;
  uint64_t next_reconnect_ms;
  FcReconnectBackoff reconnect_backoff;
  char gw_rx[DC_RX_MAX];
  size_t gw_rx_len;
  uint64_t gw_parse_progress_ms;
  char gw_tx[DC_TX_MAX];
  size_t gw_tx_len;
  long long sequence;
  long long heartbeat_interval_ms;
  uint64_t next_heartbeat_ms;
  int heartbeat_ack_pending;
  int resume_eligible;
  char session_id[256];
  char bot_user_id[64];

  long long delivery_cursor;
  int delivery_cursor_init;
  long long delivery_backpressure_cursor;
  int delivery_backpressure_narrated;
  DcOutbound outq[DC_OUT_QUEUE_MAX];
  int out_head;
  int out_count;

  DcRestState rest_state;
  int rest_fd;
  FcTls *rest_tls;
  short rest_want;
  uint64_t rest_retry_after_ms;
  unsigned int rest_server_retries;
  unsigned int rest_ambiguous_retries;
  DcOutbound rest_cur;
  int rest_has_cur;
  char rest_rx[DC_RX_MAX];
  size_t rest_rx_len;
  uint64_t rest_read_started_ms;
  char rest_tx[DC_TX_MAX];
  size_t rest_tx_len;
} DcAdapter;

/* ===== shared helpers (defined in discord_adapter.c) ============= */
void dc_diag(DcAdapter *a, const char *fmt, ...);
void dc_secure_zero(void *ptr, size_t len);
short dc_tls_want_events(FcTlsResult r);
FcTls *dc_tls_client_new(DcAdapter *a, int fd, const char *hostname);
void dc_narrate_tls_failure(DcAdapter *a, FcTls *tls,
                            const char *hostname);
int dc_rand_bytes(unsigned char *out, size_t len);
int dc_base64_encode(const unsigned char *src, size_t src_len,
                     char *dst, size_t dst_len);
int dc_resolve_token(DcAdapter *a, char *out, size_t out_len);
int dc_build_auth_header(DcAdapter *a, char *out, size_t out_len);
void dc_sanitize_text(const char *src, char *out, size_t out_len);
void dc_diag_message(DcAdapter *a, const char *stage,
                     const DcInboundMessage *msg);
size_t dc_choose_chunk_len(const char *text, size_t max_len);
int dc_queue_raw(DcAdapter *a, const char *channel_id, const char *payload);
int dc_queue_message_chunks(DcAdapter *a, const char *channel_id,
                            const char *text);

/* ===== gateway (defined in discord_gateway.c) ==================== */
void dc_close_gateway(DcAdapter *a);
void dc_close_gateway_with_cause(DcAdapter *a, const char *cause);
int  dc_gateway_start_connect(DcAdapter *a, uint64_t now_ms);
int  dc_gw_flush(DcAdapter *a);
int  dc_queue_ws_frame(DcAdapter *a, unsigned char opcode,
                       const char *payload, size_t payload_len);
void dc_drive_gateway(DcAdapter *a, int revents);
int  dc_gateway_progress_expired(DcAdapter *a, uint64_t now_ms);
int  dc_parse_message_create(const char *json, DcInboundMessage *out);
void dc_inbound_message_dispose(DcInboundMessage *msg);
int  dc_source_url_is_trusted(const char *url);
int  dc_publish_message(DcAdapter *a, const DcInboundMessage *msg);
int  dc_test_gateway_json(DcAdapter *a, const char *json);
int  dc_test_gateway_close_frame(DcAdapter *a, unsigned int code,
                                 const char *reason);
int  dc_test_gateway_raw_frame(DcAdapter *a, const unsigned char *frame,
                               size_t frame_len);
int  dc_test_heartbeat_due(DcAdapter *a, uint64_t now_ms);
/* Conversation key for one inbound message: the channel id for a guild
 * channel or thread, "dm:<user_id>" for a direct message. */
void dc_context_id(const DcInboundMessage *msg, char *out, size_t out_len);

/* ===== rest (defined in discord_rest.c) ========================== */
void dc_close_rest(DcAdapter *a);
int  dc_rest_start(DcAdapter *a, uint64_t now_ms);
void dc_drive_rest(DcAdapter *a, int revents);
int  dc_rest_progress_expired(DcAdapter *a, uint64_t now_ms);
int  dc_test_rest_response(DcAdapter *a, const char *response,
                           uint64_t now_ms);

/* ===== outbox (defined in discord_outbox.c) ====================== */
void dc_drain_deliveries(DcAdapter *a);

#endif
