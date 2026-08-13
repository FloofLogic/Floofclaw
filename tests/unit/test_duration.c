#include "test_support.h"

#include "../../runtime/support/duration.h"

int duration_parser_preserves_the_shared_cli_and_action_grammar(void) {
  static const struct {
    const char *text;
    long long expected;
  } accepted[] = {
    { "1", 1 },
    { "2ms", 2 },
    { "3s", 3000 },
    { "4m", 240000 },
    { "5min", 300000 },
    { "6h", 21600000 },
    { "7d", 604800000 }
  };
  static const char *const rejected[] = {
    "", "0", "-1s", "1.5s", "1w", "ms", "1second"
  };
  long long value = 0;
  int rc = 0;
  for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); ++i) {
    value = -1;
    rc |= expect(fc_parse_duration_ms(accepted[i].text, &value) == 0 &&
                 value == accepted[i].expected,
                 "shared duration grammar preserves accepted units");
  }
  for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i)
    rc |= expect(fc_parse_duration_ms(rejected[i], &value) != 0,
                 "shared duration grammar rejects malformed values");
  rc |= expect(fc_parse_duration_ms(NULL, &value) != 0 &&
               fc_parse_duration_ms("1s", NULL) != 0,
               "shared duration parser rejects missing arguments");
  return rc;
}
