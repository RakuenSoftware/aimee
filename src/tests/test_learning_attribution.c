/* test_learning_attribution.c: measured credit for a capability (S2).
 *
 * The arithmetic is the whole claim, so it is tested directly on grids rather
 * than through a database: pairing, the sample floor, the sign of the effect,
 * and the cases where the honest answer is "we cannot tell".
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <aimee/learning/attribution.h>

static learning_ablation_cell_t cell(const char *task, const char *ablation, int passed, int total)
{
   learning_ablation_cell_t c;
   memset(&c, 0, sizeof(c));
   snprintf(c.task_name, sizeof(c.task_name), "%s", task);
   snprintf(c.ablation, sizeof(c.ablation), "%s", ablation);
   c.passed = passed;
   c.total = total;
   return c;
}

static const learning_attribution_t *arm_named(const learning_attribution_t *arms, int n,
                                               const char *name)
{
   for (int i = 0; i < n; i++)
      if (strcmp(arms[i].ablation, name) == 0)
         return &arms[i];
   return NULL;
}

static void test_a_capability_that_carries_its_weight(void)
{
   /* Three tasks. With everything on, all pass. With rescue removed, all fail.
    * That is the clearest possible signal that rescue is doing the work. */
   learning_ablation_cell_t g[] = {
       cell("t1", "full", 1, 1),      cell("t2", "full", 1, 1),      cell("t3", "full", 1, 1),
       cell("t1", "no_rescue", 0, 1), cell("t2", "no_rescue", 0, 1), cell("t3", "no_rescue", 0, 1),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   assert(n == 1); /* the baseline is not an arm */
   const learning_attribution_t *a = arm_named(arms, n, "no_rescue");
   assert(a != NULL);
   assert(a->attributable == 1);
   assert(a->tasks_compared == 3);
   assert(fabs(a->delta - 1.0) < 1e-9);
}

static void test_a_capability_that_earns_nothing(void)
{
   /* Removing it changes nothing. delta 0 with attributable=1 is a real
    * finding — it says the counterfactual was run and the capability did not
    * matter on these tasks. */
   learning_ablation_cell_t g[] = {
       cell("t1", "full", 1, 1),     cell("t2", "full", 0, 1),     cell("t3", "full", 1, 1),
       cell("t1", "no_retry", 1, 1), cell("t2", "no_retry", 0, 1), cell("t3", "no_retry", 1, 1),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   const learning_attribution_t *a = arm_named(arms, n, "no_retry");
   assert(a != NULL);
   assert(a->attributable == 1);
   assert(fabs(a->delta) < 1e-9);
}

static void test_a_capability_that_hurts(void)
{
   /* The runs were BETTER without it. A negative delta must survive: hiding it
    * would make the measurement one-directional, which is how a metric turns
    * into an advocacy tool. */
   learning_ablation_cell_t g[] = {
       cell("t1", "full", 0, 1),         cell("t2", "full", 0, 1),
       cell("t3", "full", 0, 1),         cell("t1", "no_normalize", 1, 1),
       cell("t2", "no_normalize", 1, 1), cell("t3", "no_normalize", 1, 1),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   const learning_attribution_t *a = arm_named(arms, n, "no_normalize");
   assert(a != NULL);
   assert(a->attributable == 1);
   assert(a->delta < 0.0);
}

static void test_too_few_pairs_is_not_a_finding(void)
{
   /* Two paired tasks, a perfect-looking effect, and no claim: a capability
    * judged on two runs is judged on noise. delta stays 0 so a caller that
    * ignores `attributable` cannot read a number built from nothing. */
   learning_ablation_cell_t g[] = {
       cell("t1", "full", 1, 1),
       cell("t2", "full", 1, 1),
       cell("t1", "no_rescue", 0, 1),
       cell("t2", "no_rescue", 0, 1),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   const learning_attribution_t *a = arm_named(arms, n, "no_rescue");
   assert(a != NULL);
   assert(a->tasks_compared == 2);
   assert(a->attributable == 0);
   assert(a->delta == 0.0);
}

static void test_comparison_is_paired_not_marginal(void)
{
   /* The trap this exists to avoid. The arm was run on an EASY task the
    * baseline never saw, and skipped the hard ones. Comparing marginal pass
    * rates (baseline 1/3 = 0.33 vs arm 1/1 = 1.0) would report the capability
    * as harmful. Pairing sees only t_hard1..3, where both ran, and reports the
    * truth: not enough pairs to say, because only the hard tasks pair. */
   learning_ablation_cell_t g[] = {
       cell("t_hard1", "full", 0, 1),
       cell("t_hard2", "full", 0, 1),
       cell("t_easy", "full", 1, 1),
       cell("t_easy", "no_sampling", 1, 1),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   const learning_attribution_t *a = arm_named(arms, n, "no_sampling");
   assert(a != NULL);
   assert(a->tasks_compared == 1); /* only t_easy pairs */
   assert(a->attributable == 0);
   assert(a->delta == 0.0);
}

static void test_an_arm_with_no_counterfactual_is_visible(void)
{
   /* The arm ran; the baseline never did. Reporting the arm with
    * attributable=0 lets an operator see the counterfactual is MISSING,
    * instead of the arm silently vanishing and reading as "no effect". */
   learning_ablation_cell_t g[] = {
       cell("t1", "bare", 0, 1),
       cell("t2", "bare", 0, 1),
       cell("t3", "bare", 0, 1),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   assert(n == 1);
   assert(strcmp(arms[0].ablation, "bare") == 0);
   assert(arms[0].tasks_compared == 0);
   assert(arms[0].attributable == 0);
}

static void test_repeated_runs_are_rates_not_booleans(void)
{
   /* --runs N repeats a task per arm, so a cell is a rate. Half the baseline
    * runs pass and none of the arm's: the effect is real but only half a
    * point, and rounding it to a boolean would overstate it. */
   learning_ablation_cell_t g[] = {
       cell("t1", "full", 2, 4),       cell("t2", "full", 2, 4),
       cell("t3", "full", 2, 4),       cell("t1", "no_respond", 0, 4),
       cell("t2", "no_respond", 0, 4), cell("t3", "no_respond", 0, 4),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   const learning_attribution_t *a = arm_named(arms, n, "no_respond");
   assert(a != NULL);
   assert(a->attributable == 1);
   assert(fabs(a->delta - 0.5) < 1e-9);
}

static void test_several_arms_at_once(void)
{
   learning_ablation_cell_t g[] = {
       cell("t1", "full", 1, 1),      cell("t2", "full", 1, 1),      cell("t3", "full", 1, 1),
       cell("t1", "no_rescue", 0, 1), cell("t2", "no_rescue", 0, 1), cell("t3", "no_rescue", 0, 1),
       cell("t1", "no_retry", 1, 1),  cell("t2", "no_retry", 1, 1),  cell("t3", "no_retry", 1, 1),
   };
   learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
   int n = learning_attribution_compute(g, (int)(sizeof(g) / sizeof(g[0])), arms,
                                        LEARNING_ATTRIBUTION_MAX_ARMS);
   assert(n == 2);
   assert(arm_named(arms, n, "no_rescue")->delta > 0.0);
   assert(fabs(arm_named(arms, n, "no_retry")->delta) < 1e-9);
   assert(arm_named(arms, n, "full") == NULL);
}

int main(void)
{
   printf("learning_attribution: ");

   test_a_capability_that_carries_its_weight();
   test_a_capability_that_earns_nothing();
   test_a_capability_that_hurts();
   test_too_few_pairs_is_not_a_finding();
   test_comparison_is_paired_not_marginal();
   test_an_arm_with_no_counterfactual_is_visible();
   test_repeated_runs_are_rates_not_booleans();
   test_several_arms_at_once();

   /* An empty grid is not an error — it is a machine that has not run the
    * counterfactual yet. */
   {
      learning_attribution_t arms[LEARNING_ATTRIBUTION_MAX_ARMS];
      assert(learning_attribution_compute(NULL, 0, arms, LEARNING_ATTRIBUTION_MAX_ARMS) == -1);
      learning_ablation_cell_t none[1];
      assert(learning_attribution_compute(none, 0, arms, LEARNING_ATTRIBUTION_MAX_ARMS) == 0);
      assert(learning_attribution_compute(none, 0, NULL, 4) == -1);
      assert(learning_attribution_compute(none, 0, arms, 0) == -1);
   }

   printf("ok\n");
   return 0;
}
