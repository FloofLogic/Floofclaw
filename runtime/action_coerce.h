#ifndef FCLAW_RUNTIME_ACTION_COERCE_H
#define FCLAW_RUNTIME_ACTION_COERCE_H

#include <stddef.h>

/* Conservative primitive coercion of an action-args object against its
 * action.json args_schema, applied BEFORE validation so a mediocre
 * model's type-sloppy-but-semantically-right call still fires:
 *
 *   schema string  <- number / bool       (stringify)
 *   schema integer/number <- numeric string (numberify; integer
 *                                            rejects a fractional part)
 *   schema boolean <- "true"/"false"/1/0   (boolify)
 *
 * Only top-level properties with a primitive schema type are touched.
 * Objects, arrays, unknown keys, missing required keys, and anything
 * that already validates are left byte-identical. It never fabricates
 * a value or "fixes" a structurally-wrong call — a too-aggressive
 * coercer would turn a visible rejection into a silent wrong action,
 * which is worse.
 *
 * args_inout is rewritten in place (compact JSON) only if a coercion
 * was applied. Returns 1 if it changed args_inout, 0 if unchanged,
 * -1 on error (args_inout left untouched on error). */
int rt_action_coerce_args(const char *args_schema,
                         char *args_inout, size_t cap);

#endif
