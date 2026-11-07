/* Queue retry/backoff UTC helpers; no model command execution surface. */
#ifndef KB_CURATOR_SIDECAR_H
#define KB_CURATOR_SIDECAR_H
#include <stddef.h>

/* --- retry backoff ---------------------------------------------------------
 *
 * A failed curator job used to go straight back to status='pending' with no
 * delay, so the claim query re-grabbed it on the very next poll. A job failing
 * for a persistent reason (a wedged sidecar, a bad document, an unreachable LLM)
 * therefore spun at full speed through its whole attempt budget in milliseconds,
 * burning the drain thread and filling last_error — which is how ~5,300 jobs
 * died in minutes when a tier filled up.
 *
 * The delay is written to the job row (next_attempt_at) rather than slept in the
 * worker: a sleep would tie up a drain thread that could be doing other jobs, and
 * would be lost across a restart.
 *
 * Exponential, jittered, clamped. Attempt 1 waits ~base, attempt N waits
 * ~base * 2^(N-1), capped. Jitter breaks the thundering herd when a whole batch
 * fails together on a shared cause (the ENOSPC case: every job failed at once and
 * would otherwise retry in lockstep forever). */
#define KB_CURATOR_RETRY_BASE_S 30
#define KB_CURATOR_RETRY_MAX_S  3600

/* Seconds to wait before attempt `attempts`+1. attempts <= 0 is treated as 1.
 * Deterministic (no jitter) so it can be unit-tested; jitter is applied by
 * kb_curator_next_attempt_at. */
int kb_curator_retry_delay_seconds(int attempts);

/* Canonical KB_STORE UTC text ("YYYY-MM-DD HH:MM:SS") for now + the backoff for
 * `attempts`, with up to +/-10%% jitter. Written into next_attempt_at.
 *
 * Computed in C and bound as a parameter rather than built with the pg_now_text()
 * SQL helper: that helper is postgres-side, and these same statements run against
 * the sqlite test shim. A bound string compares identically on both, because the
 * canonical format sorts lexicographically. */
void kb_curator_next_attempt_at(int attempts, char *out, size_t outlen);

/* Canonical KB_STORE UTC text for "now" — the right-hand side of the claim query's
 * next_attempt_at comparison. Same reasoning as above. */
void kb_curator_now_text(char *out, size_t outlen);

#endif /* KB_CURATOR_SIDECAR_H */
