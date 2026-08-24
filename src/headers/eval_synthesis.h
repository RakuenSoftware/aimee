/* eval_synthesis.h: the storage half of failure -> regression eval task.
 *
 * The policy (fingerprinting, task rendering, text admissibility, the
 * admission predicate) lives in modules/learning/include/aimee/learning/eval_synthesis.h and
 * is pure. This layer owns the two effects that policy must not have: the DB1
 * candidate ledger, and writing the materialised task file into a suite
 * directory where agent_eval_load_tasks() will pick it up as an ordinary task.
 *
 * Nothing here schedules itself. Observation happens on the path that already
 * confirmed the failure; admission happens when an operator or an existing
 * supervised stage asks for it.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_EVAL_SYNTHESIS_H
#define DEC_EVAL_SYNTHESIS_H 1

#include <aimee/learning/attribution.h>
#include <aimee/learning/eval_synthesis.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Record one observation of a confirmed failure against its signature,
    * inserting the candidate on first sight. `suite` names the suite the task
    * would join (NULL => "regressions"); `session_id` is what makes a repeat
    * count as reproduction rather than the same session repeating itself.
    *
    * Returns 0 when the observation was recorded, -1 on bad args / DB error,
    * and -2 when the failure text is inadmissible — the fail-closed path, which
    * stores nothing. */
   int eval_synthesis_observe(const learning_eval_failure_t *f, const char *suite,
                              const char *session_id);

   /* What one scan pass found. */
   typedef struct
   {
      int jobs_seen;    /* failed agent jobs inspected */
      int signals_seen; /* negative correction signals inspected */
      /* Failures reported to the ledger. Observation is idempotent per
       * session, so a record already on its candidate row is counted here but
       * moves no counter — a repeated sweep reports the same number and
       * changes nothing. */
      int observed;
      int rejected_text; /* refused: inadmissible text, stored nothing */
      int skipped;       /* skipped: nothing replayable, or a write error */
   } eval_synthesis_scan_stats_t;

   /* Sweep the failure ledgers of the last `window_days` and record each
    * confirmed failure as a candidate observation.
    *
    * Two sources, and the boundary between them and everything else is a
    * design finding worth stating: a synthesisable regression needs a
    * REPLAYABLE PROMPT. Failed agent jobs (`agent_jobs`, status 'failed') carry
    * one, and become tasks whose bar is simply "this must now succeed".
    * Negative signals carrying a correction (`learning_signals`) carry both a
    * prompt and a statement of what should have been said, so they become
    * `contains` checks. Records that carry neither — `agent_outcomes`
    * (role/reason only), `eval_results` (names an EXISTING suite task, and so
    * feeds retirement rather than synthesis) — are deliberately not sources
    * here: manufacturing a prompt for them would be fabrication.
    *
    * Operator-invoked; it never schedules itself. `window_days` <= 0 picks the
    * default window. `out` may be NULL. Returns the number of observations
    * recorded (>= 0), or -1 on a hard failure. */
   int eval_synthesis_scan_failures(int window_days, const char *suite,
                                    eval_synthesis_scan_stats_t *out);

   /* Admit every candidate that has met the reproduction bar, materialising
    * each as `<suite_dir>/<task_name>.json` and marking it admitted.
    *
    * The endogeneity gate is consulted ONCE for the whole pass: when it is
    * closed, nothing is admitted and 0 is returned. `min_occurrences` <= 0
    * picks LEARNING_EVAL_MIN_OCCURRENCES. `admitted_by` is recorded on each
    * row ("auto", an operator id, ...).
    *
    * Returns the number of candidates admitted (>= 0), or -1 on bad args. A
    * candidate whose file cannot be written is left in state 'candidate' and
    * skipped, so a full disk delays admission rather than losing the row. */
   int eval_synthesis_admit_pending(const char *suite_dir, const char *admitted_by,
                                    int min_occurrences);

/* Consecutive clean windows after which an admitted regression retires. The
 * suite tracks live risk, not history: a check that has not caught anything in
 * this many passes has stopped earning its place in every gate run. */
#define EVAL_SYNTHESIS_RETIRE_WINDOWS 3

   /* Score admitted regressions against the recorded results of the suite they
    * live in, then retire the ones that have gone quiet.
    *
    * For each admitted candidate: its most recent result in `eval_results`
    * decides. A pass increments passing_windows; a failure resets it to 0 —
    * the check just earned its keep. On reaching `retire_windows` the row
    * moves to 'archived' and its task file is deleted from `suite_dir`, so the
    * hot suite shrinks back.
    *
    * A candidate with no recorded result yet is left alone (never retired for
    * lack of evidence). `retire_windows` <= 0 picks the default. Returns the
    * number retired (>= 0), or -1 on bad args. */
   int eval_synthesis_retire(const char *suite_dir, int retire_windows);

   /* Read the ablation grid from DB1 and attribute it (S2). The policy is pure
    * and lives in the learning module; this is the half that may reach the
    * store. `suite_or_null` filters. Returns arms written or -1 on error. */
   int eval_attribution_for_suite(const char *suite_or_null, learning_attribution_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_EVAL_SYNTHESIS_H */
