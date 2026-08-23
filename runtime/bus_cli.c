#include "runtime.h"

#include "bus/bus.h"
#include "support/json.h"
#include "support/cli_output.h"
#include "support/color.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

/* Exit codes. Distinct so a producer's wrapper can tell "I sent something
 * the runtime will never accept" from "this reserved id is already a
 * different envelope" from "the write did not land, retry later". */
#define BUS_CLI_OK          0
#define BUS_CLI_VALIDATION  1
#define BUS_CLI_CONFLICT    3
#define BUS_CLI_PUBLISH     4

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw bus publish [-a|-h] [--channel <id>] [--adapter-id <id>] [--ref <json>] --text <text>\n"
          "  fclaw bus publish [-a|-h] --type <kind> (--payload <json> | --payload-stdin)\n"
          "                    [--channel <id>] [--adapter-id <id>] [--context-id <id>]\n"
          "                    [--ref <json>] [--event-id <reserved>]\n"
          "  fclaw bus reserve [-a|-h]\n"
          "  fclaw bus log [-a|-h] [-f|--follow] [--channel <id>] [--type <name>] [-n|--last <count>]\n");
}

/* One rejection path: the specific field on stderr for a person, a valid
 * JSON object on stdout so agent mode never emits a non-JSON line. */
static int publish_reject(const FcCliOutputMode *mode, const char *code,
                          const char *message, int exit_code) {
  fprintf(stderr, "bus publish: %s\n", message);
  if (mode && mode->human)
    printf(COLOR_RED "rejected" COLOR_RESET " %s\n", code);
  else
    fc_cli_print_json_error("bus.publish", code, message);
  return exit_code;
}

/* Read the whole of stdin as the payload. Bounded by BUS_PAYLOAD_MAX so an
 * unbounded producer fails before publication rather than mid-write. */
static int read_payload_stdin(char *out, size_t out_len, size_t *used_out) {
  size_t used = 0;
  size_t n;
  if (!out || out_len == 0) return -1;
  while ((n = fread(out + used, 1, out_len - used, stdin)) > 0) {
    used += n;
    if (used >= out_len) return -1;   /* would not fit its NUL: too large */
  }
  if (ferror(stdin)) return -1;
  out[used] = '\0';
  if (used_out) *used_out = used;
  return 0;
}

static int do_reserve(int argc, char **argv) {
  char id[BUS_ID_MAX];
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw bus reserve");
    if (mode_rc < 0) return BUS_CLI_VALIDATION;
    if (mode_rc > 0) continue;
    usage();
    return BUS_CLI_VALIDATION;
  }
  if (bus_reserve_envelope_id(id, sizeof(id)) != 0) {
    fprintf(stderr, "bus reserve: could not allocate an envelope id\n");
    if (!mode.human)
      fc_cli_print_json_error("bus.reserve", "reserve_failed",
                              "could not allocate an envelope id");
    return BUS_CLI_PUBLISH;
  }
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "reserved" COLOR_RESET " %s\n", id);
  else {
    fputs("{\"event_id\":", stdout);
    fc_cli_print_json_string(stdout, id);
    puts("}");
  }
  return BUS_CLI_OK;
}

/* Legacy text shortcut. Its payload shape and success output are a
 * published contract; custom-event work must not touch either. */
static int publish_text(const FcCliOutputMode *mode, const char *channel,
                        const char *text, const char *adapter_id,
                        const char *ref) {
  char escaped[RT_LARGE];
  char escaped_adapter[RT_SMALL];
  char payload[RT_LARGE + RT_LARGE + 128];
  char id[BUS_ID_MAX];
  size_t pos;
  if (json_escape(text, escaped, sizeof(escaped)) != 0)
    return publish_reject(mode, "invalid_text", "--text could not be encoded",
                          BUS_CLI_VALIDATION);
  pos = (size_t)snprintf(payload, sizeof(payload), "{\"text\":\"%s\"", escaped);
  if (adapter_id) {
    if (json_escape(adapter_id, escaped_adapter, sizeof(escaped_adapter)) != 0)
      return publish_reject(mode, "invalid_adapter_id",
                            "--adapter-id could not be encoded",
                            BUS_CLI_VALIDATION);
    pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos,
                            ",\"adapter_id\":\"%s\"", escaped_adapter);
  }
  if (ref)
    pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos, ",\"ref\":%s", ref);
  if (pos >= sizeof(payload) - 2)
    return publish_reject(mode, "payload_too_large", "payload too large",
                          BUS_CLI_VALIDATION);
  snprintf(payload + pos, sizeof(payload) - pos, "}");
  if (bus_publish(channel, "user_message", payload, id, sizeof(id)) != 0)
    return publish_reject(mode, "publish_failed", "publish failed",
                          BUS_CLI_PUBLISH);
  if (mode->human)
    printf(COLOR_GREEN_BRIGHT "published" COLOR_RESET " %s\n", id);
  else {
    fputs("{\"event_id\":", stdout); fc_cli_print_json_string(stdout, id);
    fputs(",\"channel\":", stdout); fc_cli_print_json_string(stdout, channel);
    puts("}");
  }
  return BUS_CLI_OK;
}

static int do_publish(int argc, char **argv) {
  const char *channel = "cli";
  const char *text = NULL;
  const char *adapter_id = NULL;
  const char *ref = NULL;
  const char *type = NULL;
  const char *context_id = NULL;
  const char *payload_inline = NULL;
  const char *event_id = NULL;
  int payload_stdin = 0;
  char id[BUS_ID_MAX];
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw bus publish");
    if (mode_rc < 0) return BUS_CLI_VALIDATION;
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) channel = argv[++i];
    else if (strcmp(argv[i], "--adapter-id") == 0 && i + 1 < argc) adapter_id = argv[++i];
    else if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) ref = argv[++i];
    else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) text = argv[++i];
    else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) type = argv[++i];
    else if (strcmp(argv[i], "--context-id") == 0 && i + 1 < argc) context_id = argv[++i];
    else if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) payload_inline = argv[++i];
    else if (strcmp(argv[i], "--payload-stdin") == 0) payload_stdin = 1;
    else if (strcmp(argv[i], "--event-id") == 0 && i + 1 < argc) event_id = argv[++i];
    else { usage(); return BUS_CLI_VALIDATION; }
  }
  /* ref must be a JSON object — it's embedded raw as the envelope's reply
   * address (Discord's channel_id, an iMessage handle). Reject anything
   * that isn't a well-formed object so a malformed --ref can't corrupt the
   * envelope. */
  if (ref) {
    JsonRef probe;
    if (json_ref_first_object(ref, &probe) != 0)
      return publish_reject(&mode, "invalid_ref",
                            "--ref must be a JSON object "
                            "(e.g. '{\"handle\":\"...\"}')",
                            BUS_CLI_VALIDATION);
  }
  if (text && (type || payload_inline || payload_stdin || context_id ||
               event_id))
    return publish_reject(&mode, "conflicting_publish_mode",
                          "--text is the user_message shortcut; it cannot be "
                          "mixed with --type/--payload/--payload-stdin/"
                          "--context-id/--event-id",
                          BUS_CLI_VALIDATION);
  if (text) return publish_text(&mode, channel, text, adapter_id, ref);
  if (!type) { usage(); return BUS_CLI_VALIDATION; }

  /* ---- custom typed event ---- */
  if (payload_inline && payload_stdin)
    return publish_reject(&mode, "conflicting_payload_source",
                          "pass exactly one of --payload or --payload-stdin",
                          BUS_CLI_VALIDATION);
  if (!payload_inline && !payload_stdin)
    return publish_reject(&mode, "missing_payload_source",
                          "pass exactly one of --payload or --payload-stdin",
                          BUS_CLI_VALIDATION);
  if (!bus_type_token_valid(type))
    return publish_reject(&mode, "invalid_type",
                          "--type must be 1-63 bytes matching "
                          "[A-Za-z][A-Za-z0-9_.:-]*",
                          BUS_CLI_VALIDATION);
  if (bus_type_is_reserved(type))
    return publish_reject(&mode, "reserved_type",
                          "--type is in a runtime-owned namespace "
                          "(user_, action_, affair_, operation_, work_, "
                          "task_, run_, memory_)",
                          BUS_CLI_VALIDATION);
  if (!channel[0] || strlen(channel) >= BUS_CHANNEL_MAX)
    return publish_reject(&mode, "invalid_channel",
                          "--channel must be 1-63 bytes",
                          BUS_CLI_VALIDATION);
  if (adapter_id && strlen(adapter_id) > BUS_ROUTING_ADAPTER_MAX)
    return publish_reject(&mode, "invalid_adapter_id",
                          "--adapter-id exceeds 64 bytes",
                          BUS_CLI_VALIDATION);
  if (context_id) {
    if (!context_id[0] || strlen(context_id) > BUS_ROUTING_CONTEXT_MAX)
      return publish_reject(&mode, "invalid_context_id",
                            "--context-id must be 1-128 bytes",
                            BUS_CLI_VALIDATION);
    if (strncmp(context_id, "action:", 7) == 0)
      return publish_reject(&mode, "invalid_context_id",
                            "--context-id may not use the runtime-owned "
                            "action: namespace",
                            BUS_CLI_VALIDATION);
  }
  if (ref && strlen(ref) > BUS_ROUTING_REF_MAX)
    return publish_reject(&mode, "invalid_ref",
                          "--ref exceeds 512 bytes",
                          BUS_CLI_VALIDATION);
  if (event_id) {
    /* A reserved id is the only runtime identity a caller may supply, and
     * only one this runtime minted. */
    const char *p;
    if (strlen(event_id) >= BUS_ID_MAX || strncmp(event_id, "bus_", 4) != 0)
      return publish_reject(&mode, "invalid_event_id",
                            "--event-id must be a reserved bus_NNNNNN id",
                            BUS_CLI_VALIDATION);
    for (p = event_id + 4; *p; ++p)
      if (*p < '0' || *p > '9')
        return publish_reject(&mode, "invalid_event_id",
                              "--event-id must be a reserved bus_NNNNNN id",
                              BUS_CLI_VALIDATION);
    if (p == event_id + 4)
      return publish_reject(&mode, "invalid_event_id",
                            "--event-id must be a reserved bus_NNNNNN id",
                            BUS_CLI_VALIDATION);
  }
  {
    BusRouting routing;
    JsonRef probe;
    char *raw = NULL;
    char *compact = NULL;
    int rc;
    raw = (char *)malloc(BUS_PAYLOAD_MAX);
    compact = (char *)malloc(BUS_PAYLOAD_MAX);
    if (!raw || !compact) {
      free(raw); free(compact);
      return publish_reject(&mode, "publish_failed",
                            "could not allocate the payload buffer",
                            BUS_CLI_PUBLISH);
    }
    if (payload_stdin) {
      if (read_payload_stdin(raw, BUS_PAYLOAD_MAX, NULL) != 0) {
        free(raw); free(compact);
        return publish_reject(&mode, "payload_too_large",
                              "--payload-stdin exceeded the 16384-byte "
                              "payload bound or could not be read",
                              BUS_CLI_VALIDATION);
      }
    } else if (snprintf(raw, BUS_PAYLOAD_MAX, "%s", payload_inline) >=
               BUS_PAYLOAD_MAX) {
      free(raw); free(compact);
      return publish_reject(&mode, "payload_too_large",
                            "--payload exceeded the 16384-byte payload bound",
                            BUS_CLI_VALIDATION);
    }
    /* Object-shaped and well-formed, proven before anything touches the
     * inbox: a rejected command must leave no partial envelope behind. */
    if (json_ref_top_object(raw, &probe) != 0 ||
        probe.type != JSON_REF_OBJECT) {
      free(raw); free(compact);
      return publish_reject(&mode, "invalid_payload",
                            "payload must be one well-formed JSON object",
                            BUS_CLI_VALIDATION);
    }
    if (json_compact(raw, compact, BUS_PAYLOAD_MAX) != 0) {
      free(raw); free(compact);
      return publish_reject(&mode, "payload_too_large",
                            "payload exceeded the 16384-byte payload bound",
                            BUS_CLI_VALIDATION);
    }
    memset(&routing, 0, sizeof(routing));
    routing.adapter_id = adapter_id;
    routing.context_id = context_id;
    routing.ref_json = ref;
    if (event_id) {
      snprintf(id, sizeof(id), "%s", event_id);
      rc = bus_publish_stable_routed(id, channel, type, compact, &routing);
    } else {
      rc = bus_publish_routed(channel, type, compact, &routing,
                              id, sizeof(id));
    }
    free(raw); free(compact);
    if (rc == BUS_PUBLISH_CONFLICT)
      return publish_reject(&mode, "envelope_conflict",
                            "the reserved event id already holds a different "
                            "envelope",
                            BUS_CLI_CONFLICT);
    if (rc != 0)
      return publish_reject(&mode, "publish_failed",
                            "the envelope could not be committed to the bus",
                            BUS_CLI_PUBLISH);
  }
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "published" COLOR_RESET " %s %s\n", id, type);
  else {
    fputs("{\"event_id\":", stdout);  fc_cli_print_json_string(stdout, id);
    fputs(",\"type\":", stdout);      fc_cli_print_json_string(stdout, type);
    fputs(",\"channel\":", stdout);   fc_cli_print_json_string(stdout, channel);
    fputs(",\"context_id\":", stdout);
    fc_cli_print_json_string(stdout, context_id ? context_id : "");
    puts("}");
  }
  return BUS_CLI_OK;
}

static volatile int g_log_stop = 0;
static void on_log_signal(int sig) { (void)sig; g_log_stop = 1; }

static int do_log(int argc, char **argv) {
  BusLogFilter f = {0};
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw bus log");
    if (mode_rc < 0) return 1;
    if (mode_rc > 0) {
      f.human = mode.human;
    } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--follow") == 0) {
      f.follow = 1;
    } else if ((strcmp(argv[i], "--last") == 0 || strcmp(argv[i], "-n") == 0 ||
                strcmp(argv[i], "--tail") == 0) && i + 1 < argc) {
      f.last_n = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
      f.channel = argv[++i];
    } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
      f.type = argv[++i];
    } else {
      usage();
      return 1;
    }
  }
  if (f.follow) {
    signal(SIGINT, on_log_signal);
    signal(SIGTERM, on_log_signal);
    f.stop_flag = &g_log_stop;
  }
  return bus_log_show_filtered(&f);
}

int rt_bus_main(int argc, char **argv) {
  if (argc < 1) { usage(); return 1; }
  if (strcmp(argv[0], "publish") == 0) return do_publish(argc - 1, argv + 1);
  if (strcmp(argv[0], "reserve") == 0) return do_reserve(argc - 1, argv + 1);
  if (strcmp(argv[0], "log") == 0)     return do_log(argc - 1, argv + 1);
  usage();
  return 1;
}
