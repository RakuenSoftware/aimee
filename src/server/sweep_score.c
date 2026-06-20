/* sweep_score.c: the mechanical deletion test (rule-of-three + distribution +
 * independence + shared-state). See headers/sweep.h. */
#include "sweep.h"

#include <stdio.h>

void sweep_score_cfg_defaults(sweep_score_cfg_t *cfg)
{
   if (!cfg)
      return;
   cfg->min_callers = 3;
   cfg->min_distinct_files = 2;
   cfg->shared_state_tolerance = 1;
}

sweep_rank_t sweep_score(const sweep_edges_t *e, const sweep_score_cfg_t *cfg, char *reason,
                         size_t rcap)
{
   sweep_score_cfg_t def;
   if (!cfg)
   {
      sweep_score_cfg_defaults(&def);
      cfg = &def;
   }
   if (reason && rcap)
      reason[0] = '\0';
   if (!e)
   {
      if (reason)
         snprintf(reason, rcap, "no edges");
      return SWEEP_REJECT;
   }

   /* Below the count is not a seam at all. */
   if (e->caller_count < cfg->min_callers)
   {
      if (reason)
         snprintf(reason, rcap, "only %d caller(s), need >=%d", e->caller_count, cfg->min_callers);
      return SWEEP_REJECT;
   }

   /* Clears the count but a quality predicate fails -> worth-exploring, never
    * silently Strong (so a human looks before it ranks as a clean seam). */
   if (e->common_caller)
   {
      if (reason)
         snprintf(reason, rcap, "callers not independent (common upstream)");
      return SWEEP_WORTH;
   }
   if (e->distinct_files < cfg->min_distinct_files)
   {
      if (reason)
         snprintf(reason, rcap, "callers in only %d file(s), need >=%d (single-file inflation)",
                  e->distinct_files, cfg->min_distinct_files);
      return SWEEP_WORTH;
   }
   if (e->shared_state > cfg->shared_state_tolerance)
   {
      if (reason)
         snprintf(reason, rcap, "%d shared deps > tolerance %d (over-coupled)", e->shared_state,
                  cfg->shared_state_tolerance);
      return SWEEP_WORTH;
   }

   if (reason)
      snprintf(reason, rcap, "%d independent callers across %d files, shared<=%d", e->caller_count,
               e->distinct_files, cfg->shared_state_tolerance);
   return SWEEP_STRONG;
}
