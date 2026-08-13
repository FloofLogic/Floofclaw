#ifndef FCLAW_SUPPORT_LOG_ROTATION_H
#define FCLAW_SUPPORT_LOG_ROTATION_H

/* Shared file-rotation primitives used by:
 *   - runtime/gateway/janitor_module.c (config-driven periodic rotation)
 *   - runtime/action_runtime.c fc_rotate_file (agent-invoked rotation via the
 *     rotate_file managed intrinsic — see actions/core/rotate_file/)
 *
 * The engine deliberately treats this as a MECHANISM: caller supplies
 * path, keep, and compress; the code knows nothing about who owns the
 * file or when it should rotate. See feedback_engine_mechanism_not_policy
 * (design rationale in the retired ENGINE_TODO, git history).
 *
 * Rotation ladder for path X, keep=N, compress=1:
 *   X.(N-1){,.gz}  -> X.N{,.gz}     (past keep -> delete)
 *   ...
 *   X.1{,.gz}      -> X.2{,.gz}
 *   X              -> X.1           (rename live to newest archive)
 *   spawn gzip on X.1                (non-blocking; skipped if compress=0)
 *
 * fs_append_text writers open O_APPEND | O_CREAT per call, so subsequent
 * appends to X land in a fresh inode after rotation. Adapters that hold a
 * byte cursor must detect rotation by "current size < cursor" and reset.
 *
 * Returns 0 on success, -1 on failure (nothing rotated). Failure sets
 * out_err_msg if the pointer is non-NULL — small human-readable diagnostic
 * for surfacing back to the action caller.
 */
int fc_rotate_file(const char *path, int keep, int compress,
                   char *out_err_msg, unsigned long out_err_len);

/* 1 if `gzip` is on PATH at check time. Used to fall back to
 * uncompressed rotation when the binary is missing. Non-fatal —
 * uncompressed archives still rotate. */
int fc_rotation_gzip_available(void);

#endif
