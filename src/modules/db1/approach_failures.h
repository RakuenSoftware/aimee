/* db1/approach_failures.h: approach-level negative knowledge — DB1 subsystem.
 *
 * A row says: this APPROACH, against a goal of this SHAPE, failed this way.
 * Recall is by goal similarity, so a near-identical goal surfaces the approach
 * that already failed for it.
 *
 * DB1, not DB2, for the reason S1's candidate ledger is: these are THIS
 * MACHINE'S observations about ITS OWN failed jobs. The source is `agent_jobs`,
 * which is DB1 and lives in the daemon; putting the store in DB2 made the
 * feature inert in the daemon's own build, which compiles DB2 out. Sharing
 * approach memory across an org would be a deliberate promotion step, not a
 * default.
 *
 * Deliberately NOT part of `anti_patterns`: that catalog's matcher is lexical
 * over (file_path + " " + command) and its hot rows drive a BLOCKING
 * escalation path. These rows are advisory and matched fuzzily, and have no
 * business on a path that refuses work.
 *
 * Pure domain API. No backend types in any signature.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_DB1_APPROACH_FAILURES_H
#define DEC_DB1_APPROACH_FAILURES_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB1_APPROACH_SIGNATURE_LEN 33 /* 32 hex + NUL */
#define DB1_APPROACH_TEXT_LEN      512
#define DB1_APPROACH_TOKENS_LEN    512
#define DB1_APPROACH_MODE_LEN      256
#define DB1_APPROACH_SOURCE_LEN    32
#define DB1_APPROACH_REF_LEN       128

   typedef struct
   {
      int64_t id;
      char goal_signature[DB1_APPROACH_SIGNATURE_LEN];
      char goal_text[DB1_APPROACH_TEXT_LEN];
      char goal_tokens[DB1_APPROACH_TOKENS_LEN];
      char approach_signature[DB1_APPROACH_SIGNATURE_LEN];
      char approach_text[DB1_APPROACH_TEXT_LEN];
      char failure_mode[DB1_APPROACH_MODE_LEN];
      char source[DB1_APPROACH_SOURCE_LEN];
      char source_ref[DB1_APPROACH_REF_LEN];
      int occurrences;
      char created_at[32];
      char updated_at[32];
   } db1_approach_failure_t;

   /* Record that `approach` failed against `goal`. The (goal_signature,
    * approach_signature) pair is the identity: a repeat bumps occurrences
    * rather than inserting a near-duplicate. `goal_tokens` is the normalised
    * token set recall matches on — the caller computes it, because
    * normalisation is policy. Returns 0 on success, -1 on bad args / error. */
   int db1_approach_failure_record(const char *goal_signature, const char *goal_text,
                                   const char *goal_tokens, const char *approach_signature,
                                   const char *approach_text, const char *failure_mode,
                                   const char *source, const char *source_ref);

   /* Candidate rows, most-repeated and most-recent first. The caller scores and
    * filters by similarity — token overlap is policy and does not belong in
    * SQL. `must_contain` narrows the pool to rows whose goal_tokens contain
    * that token (NULL/"" for no narrowing); it is an optimisation, not the
    * filter. Returns rows written (capped at max) or -1 on error. */
   int db1_approach_failure_candidates(const char *must_contain, db1_approach_failure_t *out,
                                       int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_APPROACH_FAILURES_H */
