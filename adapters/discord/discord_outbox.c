/* Discord outbox — deliveries.jsonl tailer.
 *
 * Reads append-only deliveries from workspace/logs/deliveries.jsonl,
 * filters for entries addressed to this adapter, and enqueues message
 * chunks onto the adapter's outq. discord_rest.c pops from the queue
 * and does the actual POST. Cursor seeks to EOF on first tick to
 * prevent restart-spam (per Principle #12). */

#include "discord_internal.h"

#include "../../runtime/support/fsutil.h"
#include "../../runtime/support/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dc_delivery_for_us(DcAdapter *a, const char *line, char *channel_id, size_t channel_len,
                              char *text, size_t text_len) {
  JsonRef root, delivery, ref;
  char channel[32], adapter[64];
  if (json_ref_first_object(line, &root) != 0) return 0;
  if (json_ref_object_get_object(&root, "delivery", &delivery) != 0) return 0;
  if (json_ref_object_get_string(&delivery, "channel", channel, sizeof(channel)) != 0 || strcmp(channel, "discord") != 0) return 0;
  if (json_ref_object_get_string(&delivery, "adapter_id", adapter, sizeof(adapter)) != 0 || strcmp(adapter, a->module_id_buf) != 0) return 0;
  if (json_ref_object_get_string(&delivery, "text", text, text_len) != 0) return 0;
  if (json_ref_object_get_object(&delivery, "ref", &ref) != 0 ||
      json_ref_object_get_string(&ref, "channel_id", channel_id, channel_len) != 0) {
    /* Proactive delivery (an affair review speaking up): no inbound
     * ref exists. Deliver to the configured home channel (for a DM
     * deployment, the user's DM channel id), else the first allowed
     * guild channel. */
    if (a->home_channel_id[0])
      snprintf(channel_id, channel_len, "%s", a->home_channel_id);
    else if (a->allowed_channel_count > 0)
      snprintf(channel_id, channel_len, "%s", a->allowed_channel_ids[0]);
    else
      return 0;
  }
  return 1;
}

void dc_drain_deliveries(DcAdapter *a) {
  long long total = fs_file_size(DC_DELIVERIES_LOG);
  FILE *fp;
  /* stat, not a full read: the tick only needs the end offset, and the
   * live log runs to 64 MiB before the janitor rotates it. */
  if (!a->delivery_cursor_init) {
    a->delivery_cursor = total > 0 ? total : 0;
    a->delivery_cursor_init = 1;
    return;
  }
  if (total < 0) return;
  /* Rotation detected: the file shrank because the janitor renamed
   * the live file to .1 and the append site created a fresh empty
   * one. Start from the top of the new file. */
  if (total < a->delivery_cursor) a->delivery_cursor = 0;
  if (total <= a->delivery_cursor) return;
  fp = fopen(DC_DELIVERIES_LOG, "rb");
  if (!fp) return;
  if (fseek(fp, a->delivery_cursor, SEEK_SET) == 0) {
    char line[RT_XL];
    while (fgets(line, sizeof(line), fp)) {
      char channel_id[128] = "";
      char text[DC_REPLY_MAX] = "";
      size_t len = strlen(line);
      if (len && line[len - 1] == '\n') line[len - 1] = '\0';
      if (dc_delivery_for_us(a, line, channel_id, sizeof(channel_id), text, sizeof(text)))
        (void)dc_queue_message_chunks(a, channel_id, text);
    }
  }
  fclose(fp);
  a->delivery_cursor = total;
}
