#include "runtime.h"

#include "bus/bus.h"
#include "support/duration.h"
#include "support/cli_output.h"
#include "support/color.h"
#include "support/json.h"
#include "support/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  fclaw affair create [-a|-h] --context <id> --manage <text> [--in <duration>]\n"
          "  fclaw affair list [-a|-h] [--context <id>]\n"
          "  fclaw affair pause|resume|close [-a|-h] <affair_id>\n"
          "  fclaw affair review [-a|-h] <affair_id>\n"
          "  fclaw affair schedule [-a|-h] <affair_id> --in <duration>\n"
          "\n"
          "  <duration>: <number>(ms|s|m|h|d), e.g. 60s, 5m, 1h, 1d\n"
          "  --in sets next_review_at_ms = now + <duration>.\n");
}

static int do_create(int argc, char **argv) {
  const char *context = NULL, *manage = NULL, *in_str = NULL;
  long long in_ms = 0;
  long long initial_next_review_at_ms = 0;
  char affair_id[RT_SMALL];
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw affair create");
    if (mode_rc < 0) return 1;
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--context") == 0 && i + 1 < argc) context = argv[++i];
    else if (strcmp(argv[i], "--manage") == 0 && i + 1 < argc) manage = argv[++i];
    else if (strcmp(argv[i], "--in") == 0 && i + 1 < argc) in_str = argv[++i];
    else { usage(); return 1; }
  }
  if (!context || !manage) { usage(); return 1; }
  if (in_str && fc_parse_duration_ms(in_str, &in_ms) != 0) {
    fprintf(stderr, "affair: invalid --in duration: %s\n", in_str);
    return 1;
  }
  if (in_ms > 0)
    initial_next_review_at_ms = timing_system_wall_ms() + in_ms;
  if (rt_affair_create_manual(context, manage, initial_next_review_at_ms,
                              affair_id, sizeof(affair_id)) != 0) {
    fprintf(stderr, "affair: create failed (cap reached or invalid args)\n");
    return 1;
  }
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "created affair" COLOR_RESET " %s\n", affair_id);
  else {
    fputs("{\"affair_id\":", stdout); fc_cli_print_json_string(stdout, affair_id);
    fputs(",\"context_id\":", stdout); fc_cli_print_json_string(stdout, context);
    fputs(",\"status\":\"active\",\"manage\":", stdout);
    fc_cli_print_json_string(stdout, manage);
    printf(",\"next_review_at_ms\":%lld}\n", initial_next_review_at_ms);
  }
  return 0;
}

typedef struct {
  const char *filter;
  int human;
  int first;
} AffairListOutput;

static int list_visitor(const char *affair_id, const char *context_id,
                        const char *manage, const char *status,
                        long long next_review_at_ms,
                        size_t note_count, size_t task_id_count,
                        void *user) {
  AffairListOutput *output = (AffairListOutput *)user;
  const char *filter = output ? output->filter : NULL;
  if (filter && *filter && strcmp(filter, context_id) != 0) return 0;
  if (!output || output->human) {
    printf(COLOR_GREEN_BRIGHT "%-22s" COLOR_RESET "  %-12s  %-8s  ",
           affair_id, context_id, status);
  } else {
    if (!output->first) putchar(',');
    fputs("{\"affair_id\":", stdout); fc_cli_print_json_string(stdout, affair_id);
    fputs(",\"context_id\":", stdout); fc_cli_print_json_string(stdout, context_id);
    fputs(",\"status\":", stdout); fc_cli_print_json_string(stdout, status);
    printf(",\"next_review_at_ms\":%lld,\"note_count\":%zu,"
           "\"task_id_count\":%zu,\"manage\":",
           next_review_at_ms, note_count, task_id_count);
    fc_cli_print_json_string(stdout, manage);
    putchar('}');
    output->first = 0;
    return 0;
  }
  if (next_review_at_ms > 0) {
    long long delta_ms = next_review_at_ms - timing_system_wall_ms();
    if (delta_ms >= 0) printf("next_review_in=%llds  ", delta_ms / 1000);
    else                printf("next_review_overdue_by=%llds  ", -delta_ms / 1000);
  } else {
    printf("manual                ");
  }
  printf("notes=%zu  task_ids=%zu  manage=%s\n",
         note_count, task_id_count, manage);
  return 0;
}

static int do_list(int argc, char **argv) {
  const char *context = NULL;
  FcCliOutputMode mode;
  AffairListOutput output;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw affair list");
    if (mode_rc < 0) return 1;
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--context") == 0 && i + 1 < argc) context = argv[++i];
    else { usage(); return 1; }
  }
  output.filter = context;
  output.human = mode.human;
  output.first = 1;
  if (!mode.human) putchar('[');
  {
    int rc = rt_affair_each(list_visitor, &output) == 0 ? 0 : 1;
    if (!mode.human) puts("]");
    return rc;
  }
}

static int do_status(int argc, char **argv, const char *new_status) {
  const char *affair_id = NULL;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw affair status");
    if (mode_rc < 0) return 1;
    if (mode_rc > 0) continue;
    if (!affair_id) affair_id = argv[i];
    else { usage(); return 1; }
  }
  if (!affair_id) { usage(); return 1; }
  if (rt_affair_set_status(affair_id, new_status) != 0) {
    fprintf(stderr, "affair: status update failed for %s\n", affair_id);
    return 1;
  }
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "%s" COLOR_RESET " -> %s\n", affair_id, new_status);
  else {
    fputs("{\"affair_id\":", stdout); fc_cli_print_json_string(stdout, affair_id);
    fputs(",\"status\":", stdout); fc_cli_print_json_string(stdout, new_status);
    puts("}");
  }
  return 0;
}

static int do_review(int argc, char **argv) {
  char manage[RT_MED], context[RT_MED], escaped_text[RT_LARGE];
  char escaped_affair[RT_MED], payload[RT_LARGE + 256], id[BUS_ID_MAX];
  const char *affair_id = NULL;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw affair review");
    if (mode_rc < 0) return 1;
    if (mode_rc > 0) continue;
    if (!affair_id) affair_id = argv[i]; else { usage(); return 1; }
  }
  if (!affair_id) { usage(); return 1; }
  if (rt_affair_get_manage(affair_id, manage, sizeof(manage),
                           context, sizeof(context)) != 0) {
    fprintf(stderr, "affair: %s not found\n", affair_id);
    return 1;
  }
  {
    char synth[RT_LARGE];
    snprintf(synth, sizeof(synth),
             "(affair review) Check on this affair and respond: %s",
             manage);
    if (json_escape(synth, escaped_text, sizeof(escaped_text)) != 0) return 1;
  }
  if (json_escape(affair_id, escaped_affair, sizeof(escaped_affair)) != 0) return 1;
  snprintf(payload, sizeof(payload),
           "{\"text\":\"%s\",\"affair_id\":\"%s\"}",
           escaped_text, escaped_affair);
  if (bus_publish(context, "affair_review", payload, id, sizeof(id)) != 0) {
    fprintf(stderr, "affair: bus publish failed\n");
    return 1;
  }
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "review queued:" COLOR_RESET " %s (bus event %s)\n",
           affair_id, id);
  else {
    fputs("{\"affair_id\":", stdout); fc_cli_print_json_string(stdout, affair_id);
    fputs(",\"status\":\"queued\",\"event_id\":", stdout);
    fc_cli_print_json_string(stdout, id); puts("}");
  }
  return 0;
}

static int do_schedule(int argc, char **argv) {
  const char *in_str = NULL;
  long long in_ms = 0;
  long long next_review_at_ms;
  const char *affair_id = NULL;
  FcCliOutputMode mode;
  fc_cli_output_mode_init(&mode);
  for (int i = 0; i < argc; ++i) {
    int mode_rc = fc_cli_output_mode_arg(&mode, argv[i], "fclaw affair schedule");
    if (mode_rc < 0) return 1;
    if (mode_rc > 0) continue;
    if (strcmp(argv[i], "--in") == 0 && i + 1 < argc) in_str = argv[++i];
    else if (!affair_id) affair_id = argv[i];
    else { usage(); return 1; }
  }
  if (!affair_id || !in_str) { usage(); return 1; }
  if (fc_parse_duration_ms(in_str, &in_ms) != 0) {
    fprintf(stderr, "affair: invalid --in duration: %s\n", in_str);
    return 1;
  }
  next_review_at_ms = timing_system_wall_ms() + in_ms;
  if (rt_affair_set_next_review_at(affair_id, next_review_at_ms) != 0) {
    fprintf(stderr, "affair: schedule failed for %s\n", affair_id);
    return 1;
  }
  if (mode.human)
    printf(COLOR_GREEN_BRIGHT "%s" COLOR_RESET " next_review_at -> %lldms (in %lldms)\n",
           affair_id, next_review_at_ms, in_ms);
  else {
    fputs("{\"affair_id\":", stdout); fc_cli_print_json_string(stdout, affair_id);
    printf(",\"next_review_at_ms\":%lld,\"in_ms\":%lld}\n",
           next_review_at_ms, in_ms);
  }
  return 0;
}

int rt_affair_main(int argc, char **argv) {
  if (argc < 1) { usage(); return 1; }
  if (strcmp(argv[0], "create") == 0)  return do_create(argc - 1, argv + 1);
  if (strcmp(argv[0], "list") == 0)    return do_list(argc - 1, argv + 1);
  if (strcmp(argv[0], "pause") == 0)   return do_status(argc - 1, argv + 1, "paused");
  if (strcmp(argv[0], "resume") == 0)  return do_status(argc - 1, argv + 1, "active");
  if (strcmp(argv[0], "close") == 0)   return do_status(argc - 1, argv + 1, "closed");
  if (strcmp(argv[0], "review") == 0)  return do_review(argc - 1, argv + 1);
  if (strcmp(argv[0], "schedule") == 0) return do_schedule(argc - 1, argv + 1);
  usage();
  return 1;
}
