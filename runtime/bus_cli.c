#include "runtime.h"

#include "bus/bus.h"
#include "support/json.h"
#include "support/cli_output.h"
#include "support/color.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw bus publish [-a|-h] [--channel <id>] [--adapter-id <id>] [--ref <json>] --text <text>\n"
          "  fclaw bus log [-a|-h] [-f|--follow] [--channel <id>] [--type <name>] [-n|--last <count>]\n");
}

static int do_publish(int argc, char **argv) {
  const char *channel = "cli";
  const char *text = NULL;
  const char *adapter_id = NULL;
  const char *ref = NULL;
  char escaped[RT_LARGE];
  char escaped_adapter[RT_SMALL];
  char payload[RT_LARGE + RT_LARGE + 128];
  char id[BUS_ID_MAX];
  size_t pos;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw bus publish");
    if (mode_rc < 0) return 1;
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) channel = argv[++i];
    else if (strcmp(argv[i], "--adapter-id") == 0 && i + 1 < argc) adapter_id = argv[++i];
    else if (strcmp(argv[i], "--ref") == 0 && i + 1 < argc) ref = argv[++i];
    else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) text = argv[++i];
    else { usage(); return 1; }
  }
  if (!text) { usage(); return 1; }
  if (json_escape(text, escaped, sizeof(escaped)) != 0) return 1;
  /* ref must be a JSON object — it's embedded raw as payload.ref, mirroring
   * how the runtime carries a channel's reply address (Discord's channel_id,
   * an iMessage handle). Reject anything that isn't a well-formed object so a
   * malformed --ref can't corrupt the envelope. */
  if (ref) {
    JsonRef probe;
    if (json_ref_first_object(ref, &probe) != 0) {
      fprintf(stderr, "bus: --ref must be a JSON object (e.g. '{\"handle\":\"...\"}')\n");
      return 1;
    }
  }
  pos = (size_t)snprintf(payload, sizeof(payload), "{\"text\":\"%s\"", escaped);
  if (adapter_id) {
    if (json_escape(adapter_id, escaped_adapter, sizeof(escaped_adapter)) != 0) return 1;
    pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos,
                            ",\"adapter_id\":\"%s\"", escaped_adapter);
  }
  if (ref)
    pos += (size_t)snprintf(payload + pos, sizeof(payload) - pos, ",\"ref\":%s", ref);
  if (pos >= sizeof(payload) - 2) { fprintf(stderr, "bus: payload too large\n"); return 1; }
  snprintf(payload + pos, sizeof(payload) - pos, "}");
  if (bus_publish(channel, "user_message", payload, id, sizeof(id)) != 0) {
    fprintf(stderr, "bus: publish failed\n");
    return 1;
  }
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "published" COLOR_RESET " %s\n", id);
  else {
    fputs("{\"event_id\":", stdout); fc_cli_print_json_string(stdout, id);
    fputs(",\"channel\":", stdout); fc_cli_print_json_string(stdout, channel);
    puts("}");
  }
  return 0;
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
  if (strcmp(argv[0], "log") == 0)     return do_log(argc - 1, argv + 1);
  usage();
  return 1;
}
