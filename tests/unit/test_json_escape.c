#include "../../runtime/support/json.h"
#include "../test_support.h"

#include <string.h>

/* Found by the make-robustness fuzz gate (2026-07-20): json_escape's
 * truncation paths returned -1 without NUL-terminating, so callers that
 * ignore the return code embedded stale stack bytes in state JSON. The
 * contract now: out is always a NUL-terminated, valid escaped string,
 * truncated only at complete-escape boundaries. */
int json_escape_truncation_is_terminated_and_valid(void) {
  char src[128];
  char out[32];
  int rc = 0;

  memset(src, '"', sizeof(src) - 1);
  src[sizeof(src) - 1] = '\0';
  memset(out, 'X', sizeof(out));
  rc |= expect(json_escape(src, out, sizeof(out)) != 0,
               "oversized quote escape reports truncation");
  rc |= expect(memchr(out, '\0', sizeof(out)) != NULL,
               "truncated escape output is NUL-terminated");
  rc |= expect(strlen(out) % 2 == 0,
               "quote escape truncates only at complete \\\" pairs");

  memset(src, '\x01', sizeof(src) - 1);
  src[sizeof(src) - 1] = '\0';
  memset(out, 'X', sizeof(out));
  rc |= expect(json_escape(src, out, sizeof(out)) != 0,
               "oversized control-char escape reports truncation");
  rc |= expect(memchr(out, '\0', sizeof(out)) != NULL,
               "control-char truncation is NUL-terminated");
  rc |= expect(strlen(out) % 6 == 0,
               "control-char escape truncates only at complete \\u00xx units");

  return rc;
}
