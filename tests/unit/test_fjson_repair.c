/* Regression tests for fjson_repair invariant + memory-safety bugs
 * found by the Wave 4.1 libFuzzer campaign. Each corresponds to a
 * saved crash seed under tests/fuzz/corpus/fjson_repair/.
 *
 * The load-bearing contract: when fjson_repair reports OK_REPAIRED or
 * OK_UNCHANGED, its output MUST strictly re-parse as JSON. And it must
 * never read past the length-delimited input span.
 */
#include "../../runtime/fjson_repair/repair.h"
#include "../../runtime/support/json.h"
#include "../test_support.h"

#include <stddef.h>
#include <string.h>

/* Run repair on a length-delimited span, and if it reports success,
 * assert the output strictly re-parses. Returns the repair rc. */
static int repair_and_check(const char *in, size_t len, int *reparse_ok) {
  char out[65536];
  size_t out_len = 0;
  FjrReport report;
  int rc;
  JsonRef j;
  memset(&report, 0, sizeof(report));
  *reparse_ok = -1;
  rc = fjson_repair(in, len, out, sizeof(out), &out_len, &report);
  if (rc == FJR_OK_REPAIRED || rc == FJR_OK_UNCHANGED)
    *reparse_ok = (json_ref_from_text(out, &j) == 0);
  return rc;
}

/* Bug 1: a comma consumed just before EOF was left dangling before the
 * auto-inserted closer, e.g. {"name":"work",} — invalid JSON reported
 * as OK_REPAIRED. */
int fjson_repair_trailing_comma_before_eof_stays_valid(void) {
  static const char in[] = "{\"calls\": [{\"name\": \"work\", ";
  int reparse_ok = -1;
  int rc = repair_and_check(in, sizeof(in) - 1, &reparse_ok);
  int r = 0;
  r |= expect(rc == FJR_OK_REPAIRED, "truncated-after-comma input is repaired");
  r |= expect(reparse_ok == 1, "repaired output strictly re-parses (no dangling comma)");
  return r;
}

/* Bug 2: a raw control character preserved inside a string produced
 * output that did not re-parse but was reported as success. The
 * self-validation backstop must downgrade it to unrepairable. */
int fjson_repair_control_char_in_string_is_not_false_success(void) {
  /* "adapter_id" value contains a raw 0x03 control byte. */
  static const char in[] =
      "{\"event_id\":\"e\",\"payload\":{\"adapter_id\":\x03\"\"}}";
  int reparse_ok = -1;
  int rc = repair_and_check(in, sizeof(in) - 1, &reparse_ok);
  int r = 0;
  /* Either it honestly declines, or (if it ever learns to escape the
   * control char) it succeeds with valid output. It must NEVER report
   * success with output that fails to re-parse. */
  r |= expect(rc == FJR_ERR_UNREPAIRABLE || reparse_ok == 1,
              "control-char input never yields false-success invalid JSON");
  return r;
}

/* Bug 3: unguarded strncmp against "true"/"false"/"null" read past the
 * length-delimited input when a literal prefix sat at the very end of
 * the buffer. This exercises a value position ending exactly at a
 * literal prefix with no trailing bytes; correctness is that it does
 * not read out of bounds (ASan build proves it) and stays contract-safe. */
int fjson_repair_literal_prefix_at_end_is_bounded(void) {
  /* Value region ends with a bare "tru" (prefix of true) and no more
   * bytes — the old code strncmp'd 4 bytes off the end. */
  static const char in[] = "{\"k\":tru";
  int reparse_ok = -1;
  int rc = repair_and_check(in, sizeof(in) - 1, &reparse_ok);
  int r = 0;
  /* Contract holds regardless of rc: if it claims success the output
   * must re-parse. The real assertion is that this does not read OOB,
   * which the ASan unit build enforces by simply running it. */
  r |= expect(rc != FJR_OK_REPAIRED || reparse_ok == 1,
              "bounded literal match keeps success outputs valid");
  return r;
}
