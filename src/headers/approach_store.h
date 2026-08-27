/* approach_store.h: the storage half of approach-level negative knowledge (S3).
 *
 * The scoring half — tokenisation, signatures, goal overlap — is pure and lives
 * in the learning module (aimee/learning/approach_memory.h). This layer owns
 * the effects: reading and writing the DB1 rows, and choosing how much to say
 * at plan time.
 *
 * The split is not decoration. A module may only reach a peer over the event
 * bus, and these rows are DB1 — this machine's observations about its own
 * failed jobs. Putting the store behind the learning module made the feature
 * inert in the daemon, which builds with DB2 compiled out; the live run caught
 * exactly that.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_APPROACH_STORE_H
#define DEC_APPROACH_STORE_H 1

#include <aimee/learning/approach_memory.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Record that `approach` failed against `goal`. Returns 0 on success, -1 on
    * bad args / storage error. A goal with no topic words cannot be recalled
    * against and is refused rather than stored unreachably. */
   int approach_store_record(const char *goal, const char *approach, const char *failure_mode,
                             const char *source, const char *source_ref);

   /* Record the controller's specific failed approach in the shared KB. The
    * source_ref is provenance, not a recall boundary: later authorized users,
    * sessions, and models on the same KB can reuse the lesson when their goal
    * is sufficiently similar. The approach text is stable so a retry of the
    * same goal reinforces one row instead of creating prose variants. */
   int approach_store_record_no_progress(const char *goal, const char *failure_mode,
                                         const char *source_ref);

   /* Approaches that already failed against a goal like this one, best match
    * first, filtered to APPROACH_MEM_MIN_SIMILARITY. Returns rows written
    * (capped at max), 0 when nothing is similar enough or the store is empty,
    * or -1 on bad args / storage error. */
   int approach_store_recall(const char *goal, learning_approach_hit_t *out, int max);

   /* Render recalled dead ends as a plan-time advisory block, or write "" when
    * there is nothing to say. Never blocks and never instructs — it reports
    * what was tried and what happened.
    *
    * WHICH form is rendered is a measurable decision (S6): the
    * LEARNING_POLICY_PLAN_ADVISORY arms choose between saying nothing, one
    * line, or the full block. `arm_out` receives the arm used (may be NULL).
    * Returns the number of hits rendered — 0 when the chosen arm says nothing,
    * which is a legitimate outcome and not a failure. */
   int approach_store_render(const char *goal, char *out, size_t out_len, char *arm_out,
                             size_t arm_out_len);

   /* Heap-allocated system-prompt block for a retry, or NULL when no similar
    * failed approach is known/the selected advisory arm is off. Caller frees. */
   char *approach_store_retry_context(const char *goal);

#ifdef __cplusplus
}
#endif

#endif /* DEC_APPROACH_STORE_H */
