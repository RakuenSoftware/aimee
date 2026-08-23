/* approach_memory.h: negative knowledge about APPROACHES, not commands
 * (recursive-self-improvement S3).
 *
 * The existing anti_patterns catalog answers "is this command dangerous?" by
 * matching phrases in a path or argv. It cannot answer the more expensive
 * question: "have we already tried this way of doing this, and did it work?"
 * Nothing records that an approach to a goal was attempted and failed, so the
 * same dead end is re-walked at full cost every time.
 *
 * This is that record. A row is (goal, approach, failure mode); recall is by
 * GOAL SIMILARITY, so a near-identical goal surfaces the approach that already
 * failed for it. Recall is advisory context for planning — it never blocks, and
 * it never touches the anti-pattern blocking path.
 *
 * Similarity here is token overlap over normalised goal text, NOT an embedding.
 * That is a deliberate limit: the embedder lives behind the knowledge service,
 * and plan-time recall must work on an installation that has no KB reachable.
 * Overlap is weaker than an embedding at paraphrase and exactly as good at the
 * case that matters most — the same goal, worded slightly differently.
 *
 * The scoring half is pure and unit-testable; only the record/recall calls
 * touch storage. */
#ifndef DEC_LEARNING_APPROACH_MEMORY_H
#define DEC_LEARNING_APPROACH_MEMORY_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define APPROACH_MEM_SIGNATURE_LEN 33  /* 32 hex + NUL */
#define APPROACH_MEM_TOKENS_LEN    512
#define APPROACH_MEM_MAX_RECALL    8

/* Goal similarity at or above this counts as "a goal like this one". Set from
 * the shape of the failure it is meant to catch: re-attempting the same task
 * with wording drift, not finding loosely related work. */
#define APPROACH_MEM_MIN_SIMILARITY 0.5

   /* Normalise free text into the space-separated token set used for both
    * signatures and overlap: case-folded, every run of non-alphanumerics
    * becomes one space, duplicate tokens dropped, very short tokens dropped,
    * order preserved. Writes "" for NULL/empty input. Pure. */
   void learning_approach_tokens(const char *text, char *out, size_t out_len);

   /* Stable 32-hex fingerprint of normalised text. Two goals that normalise to
    * the same token set share a signature. Returns 0, or -1 on bad args. */
   int learning_approach_signature(const char *text, char *out, size_t out_len);

   /* Jaccard overlap of two token sets as produced by
    * learning_approach_tokens: |intersection| / |union|, in [0,1]. Two empty
    * sets overlap 0, not 1 — "nothing in common with nothing" must not read as
    * a perfect match. Pure. */
   double learning_approach_overlap(const char *tokens_a, const char *tokens_b);

   /* Record that `approach` failed against `goal`. Returns 0 on success, -1 on
    * bad args / storage error. */
   int learning_approach_record_failure(const char *goal, const char *approach,
                                        const char *failure_mode, const char *source,
                                        const char *source_ref);

   /* One recalled dead end. */
   typedef struct
   {
      char goal_text[512];
      char approach_text[512];
      char failure_mode[256];
      char source_ref[128];
      long long occurrences;
      double similarity;
   } learning_approach_hit_t;

   /* Approaches that already failed against a goal like this one, best match
    * first, filtered to similarity >= APPROACH_MEM_MIN_SIMILARITY. Returns
    * rows written (capped at max), 0 when nothing is similar enough, or -1 on
    * bad args / storage error. */
   int learning_approach_recall(const char *goal, learning_approach_hit_t *out, int max);

   /* Render recalled dead ends as a short advisory block for a plan-time
    * prompt, or write "" when there is nothing to say. Never blocks and never
    * instructs — it reports what was tried and what happened.
    *
    * WHICH form is rendered is a measurable decision, not a constant: the
    * LEARNING_POLICY_PLAN_ADVISORY arms decide between saying nothing, one
    * line, or the full block (S6). `arm_out` receives the arm that was used
    * (may be NULL). Returns the number of hits rendered — 0 when the chosen
    * arm says nothing, which is a legitimate outcome and not a failure. */
   int learning_approach_render(const char *goal, char *out, size_t out_len, char *arm_out,
                                size_t arm_out_len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_LEARNING_APPROACH_MEMORY_H */
