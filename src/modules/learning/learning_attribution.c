/* learning_attribution.c: measured credit for a capability (S2).
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <aimee/learning/attribution.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Tasks a single attribution pass will pair. The grid is per-task-per-arm, so
 * this bounds work, not correctness: beyond it the pairing simply stops
 * growing, which can only shrink a claim, never inflate one. */
#define ATTRIBUTION_MAX_TASKS 512

/* A cell's pass rate. Tasks are usually run once per arm, but `--runs N`
 * repeats them, so this is a rate rather than a boolean. */
static double cell_rate(const learning_ablation_cell_t *c)
{
   return c->total > 0 ? (double)c->passed / (double)c->total : 0.0;
}

static const learning_ablation_cell_t *find_cell(const learning_ablation_cell_t *cells, int n,
                                                 const char *task, const char *ablation)
{
   for (int i = 0; i < n; i++)
      if (strcmp(cells[i].task_name, task) == 0 && strcmp(cells[i].ablation, ablation) == 0)
         return &cells[i];
   return NULL;
}

static int arm_index(learning_attribution_t *out, int count, const char *ablation)
{
   for (int i = 0; i < count; i++)
      if (strcmp(out[i].ablation, ablation) == 0)
         return i;
   return -1;
}

int learning_attribution_compute(const learning_ablation_cell_t *cells, int n,
                                 learning_attribution_t *out, int max)
{
   if (!cells || n < 0 || !out || max <= 0)
      return -1;

   int arms = 0;
   for (int i = 0; i < n; i++)
   {
      const char *ab = cells[i].ablation;
      if (!ab[0] || strcmp(ab, LEARNING_ATTRIBUTION_BASELINE) == 0)
         continue; /* the baseline is what everything else is measured against */
      if (arm_index(out, arms, ab) >= 0 || arms >= max)
         continue;
      memset(&out[arms], 0, sizeof(out[arms]));
      snprintf(out[arms].ablation, sizeof(out[arms].ablation), "%s", ab);
      arms++;
   }

   /* Pair each arm's tasks against the baseline's. Sums stay in doubles
    * because a cell is a rate, not a count of passes. */
   for (int a = 0; a < arms; a++)
   {
      double base_sum = 0.0, arm_sum = 0.0;
      int paired = 0;
      for (int i = 0; i < n && paired < ATTRIBUTION_MAX_TASKS; i++)
      {
         if (strcmp(cells[i].ablation, out[a].ablation) != 0)
            continue;
         const learning_ablation_cell_t *base =
             find_cell(cells, n, cells[i].task_name, LEARNING_ATTRIBUTION_BASELINE);
         if (!base || base->total <= 0 || cells[i].total <= 0)
            continue; /* no counterfactual for this task: it cannot be paired */
         base_sum += cell_rate(base);
         arm_sum += cell_rate(&cells[i]);
         paired++;
      }

      out[a].tasks_compared = paired;
      out[a].baseline_passed = (int)(base_sum + 0.5);
      out[a].arm_passed = (int)(arm_sum + 0.5);
      if (paired >= LEARNING_ATTRIBUTION_MIN_TASKS)
      {
         out[a].delta = (base_sum - arm_sum) / (double)paired;
         out[a].attributable = 1;
      }
      else
      {
         /* Too thin to carry a claim. delta stays 0 so a caller that ignores
          * `attributable` reads "no measured effect" rather than a number
          * built from one or two runs. */
         out[a].delta = 0.0;
         out[a].attributable = 0;
      }
   }
   return arms;
}
