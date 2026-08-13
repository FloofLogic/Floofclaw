#include "../../runtime/fjson_repair/repair.h"
#include "../../runtime/support/json.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_INPUT_CAP (64U * 1024U)
#define FUZZ_OUTPUT_CAP (FUZZ_INPUT_CAP * 2U + 256U)

static char g_output[FUZZ_OUTPUT_CAP];
static char g_second[FUZZ_OUTPUT_CAP];

static void require(int condition) {
  if (!condition) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FjrReport first_report;
  FjrReport second_report;
  size_t first_len = 0;
  size_t second_len = 0;
  int first;
  int second;
  JsonRef strict;

  if (!data || size > FUZZ_INPUT_CAP) return 0;
  first = fjson_repair((const char *)data, size, g_output,
                       sizeof(g_output), &first_len, &first_report);
  second = fjson_repair((const char *)data, size, g_second,
                        sizeof(g_second), &second_len, &second_report);

  require(first == second);
  require(first_len == second_len);
  require(memcmp(g_output, g_second, first_len + 1U) == 0);
  require(memcmp(&first_report, &second_report, sizeof(first_report)) == 0);

  if (first == FJR_OK_UNCHANGED || first == FJR_OK_REPAIRED) {
    require(first_len < sizeof(g_output));
    require(g_output[first_len] == '\0');
    require(json_ref_from_text(g_output, &strict) == 0);
    require(first_report.candidate_offset <= size);
    require(first_report.candidate_len <= size - first_report.candidate_offset);
    if (first == FJR_OK_UNCHANGED) {
      require(first_len == size);
      require(memcmp(g_output, data, size) == 0);
    }
  }
  return 0;
}
