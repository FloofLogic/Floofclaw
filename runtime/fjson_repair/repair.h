#ifndef FCLAW_FJSON_REPAIR_H
#define FCLAW_FJSON_REPAIR_H

#include <stddef.h>

typedef enum {
  FJR_OK_UNCHANGED = 0,
  FJR_OK_REPAIRED = 1,
  FJR_ERR_UNREPAIRABLE = -1,
  FJR_ERR_OUTPUT_TOO_SMALL = -2,
  FJR_ERR_AMBIGUOUS = -3
} FjrStatus;

typedef struct fjson_repair_report {
  unsigned stripped_fence : 1;
  unsigned stripped_prose : 1;
  unsigned added_missing_quote : 1;
  unsigned added_missing_comma : 1;
  unsigned removed_trailing_comma : 1;
  unsigned closed_string : 1;
  unsigned closed_object : 1;
  unsigned closed_array : 1;
  unsigned substituted_wrong_closer : 1;
  unsigned converted_single_quotes : 1;
  unsigned replaced_invalid_literal : 1;
  unsigned trimmed_trailing_junk : 1;
  unsigned stripped_comments : 1;

  int repairs;

  size_t candidate_offset;
  size_t candidate_len;
  size_t error_offset;
} FjrReport;

int fjson_repair(const char *in, size_t in_len,
                 char *out, size_t out_cap,
                 size_t *out_len,
                 struct fjson_repair_report *report);

#endif
