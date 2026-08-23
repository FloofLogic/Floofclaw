/* Channel adapters tail workspace/logs/deliveries.jsonl by byte cursor.
 *
 * All three used to read the ENTIRE log every tick just to learn its
 * length, then reopen it from the cursor. Rotation is at 64 MiB and the
 * foreground reactor ticks at ~16.7 Hz, so the worst case was three 64 MB
 * read+malloc pairs per tick to answer a question stat() answers for free.
 * The contract these pin: an idle tick costs nothing, whatever the log
 * weighs, and a rotation is still noticed. */

#include "test_support.h"

#include "../../adapters/discord/discord_internal.h"
#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/heap_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAILER_LOG "workspace/logs/deliveries.jsonl"

/* One delivery line addressed to `adapter_id`. */
static int append_delivery(const char *adapter_id, const char *text) {
  char line[1024];
  char *existing = NULL;
  char *joined = NULL;
  int rc;
  snprintf(line, sizeof(line),
           "{\"run_id\":\"run_001\",\"request_id\":\"actionreq_1\","
           "\"delivery\":{\"delivery_id\":\"deliv_run_001_000001\","
           "\"origin_event_id\":\"bus_000001\",\"channel\":\"discord\","
           "\"adapter_id\":\"%s\",\"ref\":{\"channel_id\":\"99\"},"
           "\"text\":\"%s\"}}\n",
           adapter_id, text);
  if (test_read_file(TAILER_LOG, &existing) != 0) existing = NULL;
  joined = (char *)malloc((existing ? strlen(existing) : 0) +
                          strlen(line) + 1U);
  if (!joined) { free(existing); return -1; }
  joined[0] = '\0';
  if (existing) strcat(joined, existing);
  strcat(joined, line);
  rc = test_write_file(TAILER_LOG, joined);
  free(existing);
  free(joined);
  return rc;
}

/* A log big enough that reading it would be unmistakable in the counters. */
static int write_bulk_log(size_t lines) {
  char *blob;
  size_t i, pos = 0;
  const char *line =
      "{\"run_id\":\"run_000\",\"request_id\":\"actionreq_0\","
      "\"delivery\":{\"delivery_id\":\"deliv_bulk\",\"channel\":\"other\","
      "\"adapter_id\":\"someone_else\",\"text\":\"bulk\"}}\n";
  size_t len = strlen(line);
  int rc;
  blob = (char *)malloc(len * lines + 1U);
  if (!blob) return -1;
  for (i = 0; i < lines; ++i) { memcpy(blob + pos, line, len); pos += len; }
  blob[pos] = '\0';
  rc = test_write_file(TAILER_LOG, blob);
  free(blob);
  return rc;
}

int adapter_delivery_tailer_ticks_without_reading_the_log(void) {
  DcAdapter *a = NULL;
  FcHeapStats before, after;
  long long big_size;
  int rc = 0;

  rc |= test_reset_workspace();
  a = (DcAdapter *)calloc(1, sizeof(*a));
  rc |= expect(a != NULL, "allocate the tailer fixture adapter");
  if (!a) return rc;
  snprintf(a->module_id_buf, sizeof(a->module_id_buf), "discord");
  snprintf(a->home_channel_id, sizeof(a->home_channel_id), "99");

  /* ~2 MB of history the adapter has already passed. */
  rc |= expect(write_bulk_log(16000) == 0, "write a bulk delivery log");
  big_size = fs_file_size(TAILER_LOG);
  rc |= expect(big_size > 1000000, "the bulk log is large enough to notice");

  /* First tick seeks to EOF (Principle 12: no restart spam). */
  dc_drain_deliveries(a);
  rc |= expect(a->delivery_cursor == big_size,
               "the first tick parks the cursor at end of file");
  rc |= expect(a->out_count == 0,
               "history before the cursor is never delivered");

  /* Idle ticks over that same large log must not allocate at all. */
  fc_heap_snapshot(&before);
  for (int i = 0; i < 8; ++i) dc_drain_deliveries(a);
  fc_heap_snapshot(&after);
  rc |= expect(after.mallocs == before.mallocs &&
                   after.callocs == before.callocs &&
                   after.reallocs == before.reallocs &&
                   after.strdups == before.strdups,
               "an idle tick allocates nothing regardless of log size");
  rc |= expect(a->delivery_cursor == big_size,
               "an idle tick leaves the cursor alone");

  /* A new delivery is still picked up from the cursor. */
  rc |= expect(append_delivery("discord", "hello there") == 0,
               "append a delivery for this adapter");
  dc_drain_deliveries(a);
  rc |= expect(a->out_count == 1, "the appended delivery is queued");
  rc |= expect(a->delivery_cursor == fs_file_size(TAILER_LOG),
               "the cursor advances to the new end of file");

  /* Rotation: the janitor renames the live file and the append site
   * creates a fresh one, so the size shrinks below the cursor. */
  a->out_count = 0;
  a->out_head = 0;
  rc |= expect(test_remove_path(TAILER_LOG) == 0,
               "rotate the delivery log away");
  rc |= expect(append_delivery("discord", "after rotation") == 0,
               "append a delivery to the fresh log");
  dc_drain_deliveries(a);
  rc |= expect(a->delivery_cursor == fs_file_size(TAILER_LOG),
               "rotation resets the cursor to the new file");
  rc |= expect(a->out_count == 1,
               "the first delivery after rotation is not lost");

  /* A missing log is not a rotation: hold the cursor and deliver nothing. */
  a->out_count = 0;
  a->out_head = 0;
  rc |= expect(test_remove_path(TAILER_LOG) == 0, "remove the delivery log");
  dc_drain_deliveries(a);
  rc |= expect(a->out_count == 0, "a missing log delivers nothing");

  free(a);
  return rc;
}
