/* attribution.h: measured credit for a capability, from counterfactuals the
 * harness already runs (recursive-self-improvement S2).
 *
 * Every reward in the system is a proxy. kb_bandit_recall_sufficiency_reward()
 * says so in its own comment. eval_feedback_loop() reinforces a rule on WORD
 * OVERLAP with a failed task and, separately, with a passed one — the same rule
 * bumped for opposite outcomes, with no causal claim behind either.
 *
 * The fix does not need a new experiment. `aimee eval run --ablation all`
 * already runs every task once per preset, each preset removing exactly one
 * capability, and records each outcome with its label. That is a counterfactual
 * grid: same task, one thing changed, same success check. Nothing reads it.
 * These functions read it.
 *
 * The comparison is PAIRED — only tasks where both the baseline and the ablated
 * arm have results — because comparing marginal pass rates across a different
 * task mix measures the mix, not the capability.
 *
 * Everything here is pure: it takes the grid and returns the attribution, so
 * the arithmetic is testable without a database, a harness, or a model.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */
#ifndef DEC_LEARNING_ATTRIBUTION_H
#define DEC_LEARNING_ATTRIBUTION_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The preset every other arm is measured against: nothing removed. */
#define LEARNING_ATTRIBUTION_BASELINE "full"

/* Paired tasks below this and the arm reports not-attributable. A capability
 * judged on one or two tasks is judged on noise. */
#define LEARNING_ATTRIBUTION_MIN_TASKS 3

#define LEARNING_ATTRIBUTION_MAX_ARMS 8
#define LEARNING_ABLATION_TASK_LEN    128
#define LEARNING_ABLATION_NAME_LEN    32

   /* One (task, arm) cell of the grid. The learning module declares its own
    * shape rather than including the store's row type: a module may only reach
    * a peer over the event bus, so the caller that reads DB1 converts. */
   typedef struct
   {
      char task_name[LEARNING_ABLATION_TASK_LEN];
      char ablation[LEARNING_ABLATION_NAME_LEN];
      int passed;
      int total;
   } learning_ablation_cell_t;

   typedef struct
   {
      char ablation[LEARNING_ABLATION_NAME_LEN];
      int tasks_compared;  /* tasks with results on BOTH arms */
      int baseline_passed; /* of those tasks, passes with nothing removed */
      int arm_passed;      /* of those tasks, passes with this removed */
      /* baseline pass-rate minus arm pass-rate over the paired tasks. Positive
       * means removing the capability cost us; negative means the runs were
       * better without it, which is worth knowing and must not be hidden. */
      double delta;
      /* 0 when the pairing is too thin to carry a claim. A caller must not
       * read `delta` as evidence when this is 0. */
      int attributable;
   } learning_attribution_t;

   /* Compute per-ablation attribution from an ablation grid. `cells` need not
    * be sorted. Arms with no baseline pair are still returned, with
    * attributable = 0 and delta = 0, so an operator can see that the
    * counterfactual was never run rather than reading silence as no effect.
    * Returns arms written (capped at max), or -1 on bad args. */
   int learning_attribution_compute(const learning_ablation_cell_t *cells, int n,
                                    learning_attribution_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_LEARNING_ATTRIBUTION_H */
