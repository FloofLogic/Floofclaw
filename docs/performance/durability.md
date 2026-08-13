# Durability policy

FloofClaw uses two durability contracts for runtime files.

Atomic state writes replace the destination with a fully written and synced
same-directory temporary file. They guarantee old-or-new file contents after a
crash; a reader must not observe a partially written state file. Directory
fsync after rename is intentionally omitted, so rename durability across power
loss is filesystem-dependent.

Event-log appends use ordinary operating-system buffering until a run reaches a
terminal lifecycle event: `run_done`, `run_failed`, or `run_canceled`. The
terminal append is written and synced on the same file descriptor before that
descriptor is closed. If the append or sync fails, the run retires failed and
the runtime reports the failure directly; it does not recursively append a
second terminal event. The direct stderr diagnostic is immediate. If the
narration log is unavailable during the same filesystem outage, the first
operator narration is retained in one bounded in-memory slot and retried by
the gateway after storage becomes writable again.

A crash may lose the in-flight run's append tail; a completed run has crossed
an fsync boundary.

Other append-only logs retain operating-system buffering. This policy has no
operator knobs.

## Replay of a torn event-log tail

The terminating newline is the event record's commit marker. Replay skips an
unterminated or malformed final record and projects the same state as the log
before that record. It narrates the total as
`replay: skipped N malformed line(s)`.

A malformed record followed by another record is not a power-loss tail. Replay
continues so intact later evidence remains inspectable, but it emits and
narrates a loud corruption warning naming the first non-final malformed line.
Oversized records are handled as one malformed record rather than being split
into apparent events.

## Bus envelope IDs

Bus publishers serialize the counter read/increment/atomic-replace sequence
with an advisory lock on `workspace/bus/.counter.lock`. The lock is released by
the kernel if a publisher process exits or crashes. A claimed counter value is
committed before its envelope is written, so a failed publisher may leave an
ID gap but cannot cause another concurrent publisher to reuse that ID. Missing
counter state starts at zero; unreadable or malformed existing state fails
closed rather than restarting the sequence.
