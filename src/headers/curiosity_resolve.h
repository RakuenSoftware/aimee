/* curiosity_resolve.h: draining the backlog nobody drains
 * (recursive-self-improvement S4).
 *
 * The curiosity backlog records what aimee knows it does not know —
 * missing_fact, contradiction, stale_fact, weak_coverage,
 * unverified_assumption. Items go in from several places. Nothing takes them
 * out except a human typing `aimee memory curiosity resolve <id>`, so the
 * backlog only ever grows, and a gap that has since been answered sits there
 * looking like an open question.
 *
 * This pass asks, for each open item, whether the gap is still a gap — and
 * closes the ones that are not.
 *
 * Two design constraints it is built around:
 *
 *   - It is OPERATOR-INVOKED and never schedules itself. A periodic worker
 *     that scores and rewrites stored state without a named journey is exactly
 *     what the background-curator removal deleted and now forbids by lint.
 *   - The evidence probe is INSTALLED, not assumed. Whether a topic is now
 *     covered is a knowledge-service question; this layer must not invent an
 *     answer, so it takes a probe and does nothing useful without one. That
 *     also makes the pass testable without a corpus.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_CURIOSITY_RESOLVE_H
#define DEC_CURIOSITY_RESOLVE_H 1

#ifdef __cplusplus
extern "C"
{
#endif

/* Items one invocation will look at. A budget rather than "the whole backlog"
 * because each probe may reach the knowledge service, and an operator asking a
 * question should not accidentally start an unbounded job. */
#define CURIOSITY_RESOLVE_DEFAULT_BUDGET 25

   /* Verdict from an evidence probe. */
   typedef enum
   {
      CURIOSITY_EVIDENCE_NONE = 0,    /* still a gap: leave it open */
      CURIOSITY_EVIDENCE_FOUND = 1,   /* answered since: resolve it */
      CURIOSITY_EVIDENCE_UNKNOWN = -1 /* could not tell: leave it open, count separately */
   } curiosity_evidence_t;

   /* Does the corpus now answer this gap? `gap_type` is the canonical name;
    * `subject` is the entity or topic the item is about. Must be side-effect
    * free with respect to the backlog — it answers, it does not resolve. */
   typedef curiosity_evidence_t (*curiosity_probe_fn)(const char *gap_type, const char *subject,
                                                      const char *evidence);

   /* Install the probe. NULL clears it, after which a pass is a no-op that
    * reports why rather than resolving anything on a guess. */
   void curiosity_resolve_register_probe(curiosity_probe_fn probe);

   typedef struct
   {
      int considered; /* open items looked at (bounded by the budget) */
      int resolved;   /* gaps the probe found answers for */
      int still_open; /* probe says the gap stands */
      int unknown;    /* probe could not tell */
      int skipped;    /* gap types this pass does not handle */
      int budget;     /* the budget actually applied */
      int no_probe;   /* 1 when nothing ran because no probe is installed */
   } curiosity_resolve_stats_t;

   /* Walk up to `budget` open items and resolve the ones whose gap the probe
    * says is answered. `budget` <= 0 picks the default.
    *
    * Only `unverified_assumption` and `weak_coverage` are handled: both ask
    * "is there support for this?", which a probe can answer. `contradiction`
    * and `stale_fact` need a judgement about which of two claims is right, and
    * closing those on a coverage probe would silently pick a winner — they are
    * counted as skipped rather than guessed at.
    *
    * Returns the number resolved (>= 0), or -1 on bad args / storage error. */
   int curiosity_resolve_pass(int budget, curiosity_resolve_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CURIOSITY_RESOLVE_H */
