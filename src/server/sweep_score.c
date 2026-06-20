/* sweep_score.c: the mechanical deletion test (rule-of-three + distribution +
 * independence + shared-state). See headers/sweep.h. */
#include "sweep.h"

#include <stdio.h>
#include <string.h>

sweep_edges_t sweep_edges_from_callers(const caller_hit_t *callers, int n, int blast_deps)
{
   sweep_edges_t e = {0, 0, blast_deps < 0 ? 0 : blast_deps, 0};
   if (!callers || n <= 0)
      return e;
   e.caller_count = n;
   int distinct_files = 0, distinct_callers = 0, named = 0;
   for (int i = 0; i < n; i++)
   {
      int seen_f = 0, seen_c = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(callers[i].file_path, callers[j].file_path) == 0)
            seen_f = 1;
         if (callers[i].caller[0] && strcmp(callers[i].caller, callers[j].caller) == 0)
            seen_c = 1;
      }
      if (!seen_f)
         distinct_files++;
      if (callers[i].caller[0])
      {
         named++;
         if (!seen_c)
            distinct_callers++;
      }
   }
   e.distinct_files = distinct_files;
   /* a funnel only when EVERY caller is named and they are all the same function;
    * a mix with file-scope (empty-name) callers is not a funnel. */
   e.common_caller = (named == n && distinct_callers == 1 && n > 1) ? 1 : 0;
   return e;
}

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
