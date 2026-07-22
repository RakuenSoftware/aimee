/* rounds_to_resume.h: how long an agent spends recovering after a compaction.
 *
 * The metric. After a compaction boundary, count the turns the agent spends
 * re-deriving state it already had before the boundary, before it makes new
 * progress. Zero means the boundary cost nothing; higher means the summary
 * failed to carry something the agent needed and it went back to the tools to
 * rebuild it.
 *
 * Why it exists: aimee compacts every long session at 80% pressure and exposes
 * a pluggable context-engine registry, but measures neither. We test WHEN to
 * compact (pressure thresholds) and never HOW WELL it went. Without this number
 * every claim about compaction quality — including "the compactor loses
 * execution state" — is an assertion.
 *
 * How a round is judged:
 *   - session_compact() reports the read-only tool signatures it destroyed
 *     (rtr_begin takes them; they cannot be recovered after the delete).
 *   - Each post-boundary turn that issues tool calls is one round.
 *   - A turn whose calls are ALL matched read-only lookups is re-derivation:
 *     the agent is rebuilding what it already knew. Keep counting.
 *   - The first turn containing anything else is progress. Resolve.
 *
 * Only tools whose repetition can only mean lost context are matched, via
 * session_compact_tool_sig(). A repeated `test` after an edit is progress, not
 * re-derivation, and must never resolve to a round — see the tests.
 *
 * This is accounting, not control: nothing here feeds back into the loop. The
 * tracker observes and counts.
 */
#ifndef ROUNDS_TO_RESUME_H
#define ROUNDS_TO_RESUME_H 1

#include "session_compact.h"

typedef struct
{
   int active;   /* a boundary is being tracked */
   int resolved; /* rounds_to_resume has settled */

   int rounds_to_resume; /* non-progress rounds before the first progress turn */
   int rederived_calls;  /* matched read-only calls seen post-boundary */

   /* The pre-boundary read-only signatures, copied from the compaction result.
    * `sigs_dropped` is carried so a report can say the basis was incomplete
    * rather than silently under-counting re-derivation. */
   int sig_count;
   int sigs_dropped;
   char sigs[SESSION_COMPACT_MAX_SIGS][SESSION_COMPACT_SIG_LEN];
} rtr_tracker_t;

/* Start tracking at a compaction boundary. Safe to call on a result whose
 * `compacted` is 0 — the tracker simply stays inactive, because no boundary
 * means no lost context to recover. Re-arming on a later boundary is allowed;
 * the previous measurement should be read out first. */
void rtr_begin(rtr_tracker_t *t, const session_compact_result_t *r);

/* Observe one post-boundary turn. `names` and `args` are that turn's tool
 * calls (args may contain NULLs; count is `n`). A turn with no tool calls is
 * not a round — the agent produced text, which is neither re-derivation nor
 * tool progress.
 *
 * Returns 1 on the turn rounds_to_resume resolves, 0 otherwise. */
int rtr_observe_turn(rtr_tracker_t *t, const char *const *names, const char *const *args, int n);

#endif /* ROUNDS_TO_RESUME_H */
