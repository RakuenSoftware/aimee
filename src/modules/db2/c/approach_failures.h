/* db2/approach_failures.h: approach-level negative knowledge — DB2 subsystem.
 *
 * A SIBLING of anti_patterns, deliberately not an extension of it. That table's
 * matcher is lexical over (file_path + " " + command) and its hot rows drive a
 * BLOCKING escalation path; mixing advisory, goal-shaped rows into it would put
 * fuzzy matches on a path that refuses work. These rows are advisory only: they
 * are recalled when a plan is being formed, and they never block anything.
 *
 * A row says: this APPROACH, against a goal of this SHAPE, failed this way.
 * Recall is by goal similarity, so a near-identical goal surfaces the approach
 * that already failed for it.
 *
 * Pure domain API. No backend types; SQL lives in approach_failures.c.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_DB2_APPROACH_FAILURES_H
#define DEC_DB2_APPROACH_FAILURES_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define APPROACH_SIGNATURE_LEN 33 /* 32 hex + NUL */
#define APPROACH_TEXT_LEN      512
#define APPROACH_TOKENS_LEN    512
#define APPROACH_MODE_LEN      256
#define APPROACH_SOURCE_LEN    32
#define APPROACH_REF_LEN       128

/* No DB2 connection: the store is absent, not broken. */
#define DB2_APPROACH_UNAVAILABLE (-2)

   typedef struct
   {
      int64_t id;
      char goal_signature[APPROACH_SIGNATURE_LEN];
      char goal_text[APPROACH_TEXT_LEN];
      char goal_tokens[APPROACH_TOKENS_LEN];
      char approach_signature[APPROACH_SIGNATURE_LEN];
      char approach_text[APPROACH_TEXT_LEN];
      char failure_mode[APPROACH_MODE_LEN];
      char source[APPROACH_SOURCE_LEN];
      char source_ref[APPROACH_REF_LEN];
      int64_t occurrences;
      char created_at[32];
      char updated_at[32];
      /* Filled by recall only: goal-token overlap in [0,1] with the query. */
      double similarity;
   } approach_failure_t;

   /* Record that `approach` failed against `goal`. The (goal_signature,
    * approach_signature) pair is the identity: a repeat bumps occurrences
    * rather than inserting a near-duplicate. `goal_tokens` is the normalised,
    * space-separated token set the recall side matches on — the caller
    * computes it, because normalisation is policy.
    * Returns 0 on success, -1 on bad args / SQL / connection error. */
   int db2_approach_failure_record(const char *goal_signature, const char *goal_text,
                                   const char *goal_tokens, const char *approach_signature,
                                   const char *approach_text, const char *failure_mode,
                                   const char *source, const char *source_ref);

   /* Candidate rows for a goal, newest and most-repeated first. The caller
    * scores and filters by similarity — this returns the pool, since token
    * overlap is policy and does not belong in SQL. `must_contain` narrows the
    * pool to rows whose goal_tokens contain that token (pass NULL/"" for no
    * narrowing); it is an optimisation, not the filter.
    *
    * Returns rows written (capped at max), -1 on bad args / SQL error, or
    * DB2_APPROACH_UNAVAILABLE when there is no DB2 connection at all. Those
    * are different answers: an installation with no knowledge service KNOWS
    * NOTHING, which is not the same as a query that failed, and a caller
    * asking "what have we already tried?" must be able to tell them apart. */
   int db2_approach_failure_candidates(const char *must_contain, approach_failure_t *out, int max);

   /* Exact-identity lookup. Returns 1 on hit, 0 on miss, -1 on error. */
   int db2_approach_failure_get(const char *goal_signature, const char *approach_signature,
                                approach_failure_t *out);

   /* Total rows held. Returns -1 on error. */
   int64_t db2_approach_failure_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_APPROACH_FAILURES_H */
