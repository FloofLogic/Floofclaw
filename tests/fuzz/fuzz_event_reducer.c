#include "../../runtime/runtime.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define FUZZ_INPUT_CAP (64U * 1024U)

static char g_event[FUZZ_INPUT_CAP + 2U];
static char g_state[RT_XL];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  RtReplayReport report;
  JsonRef state;
  if (!data || size > FUZZ_INPUT_CAP) return 0;

  /* Exercise exactly one committed JSONL record. Replace embedded line/NUL
   * boundaries so every input byte reaches the record parser. */
  for (size_t i = 0; i < size; ++i) {
    uint8_t ch = data[i];
    g_event[i] = (ch == 0 || ch == '\n' || ch == '\r') ? ' ' : (char)ch;
  }
  g_event[size] = '\n';
  g_event[size + 1U] = '\0';

  if (rt_reduce_event_log_text_report(g_event, g_state, sizeof(g_state),
                                      &report) == 0 &&
      json_ref_top_object(g_state, &state) != 0)
    abort();
  return 0;
}
