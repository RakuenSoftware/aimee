/* kb_curator_pipeline.c: generic curator stage runner. See header. Deliberately
 * dependency-free (only calls the stage function pointers) so it is unit-testable
 * with mock stages. */
#include "kb_curator_pipeline.h"

int kb_curator_pipeline_run_pass(const kb_curator_stage_desc_t *stages, size_t n, int lane_filter,
                                 const config_t *cfg, const kb_curator_extract_opts_t *opts,
                                 void (*set_status)(const char *label))
{
   if (!stages)
      return 0;
   int did = 0;
   for (size_t i = 0; i < n; i++)
   {
      const kb_curator_stage_desc_t *st = &stages[i];
      if (!st->run)
         continue;
      if (lane_filter >= 0 && (int)st->lane != lane_filter)
         continue;
      if (st->enabled && !st->enabled(cfg))
         continue;
      int budget = st->budget > 0 ? st->budget : 1;
      for (int b = 0; b < budget; b++)
      {
         if (set_status && st->label)
            set_status(st->label);
         int r = st->run(opts);
         if (r < 0)
            return -1; /* stage error: stop the pass, signal backoff */
         if (r != 1)
            break; /* queue empty: move to the next stage */
         did = 1;
      }
   }
   return did ? 1 : 0;
}
