/* gateway_pipeline.c: the request-stage runner for the universal gateway (P2a).
 * Deliberately dependency-free — it only sequences stage callbacks over the IR and
 * never touches `raw`/`driver`/`ag`, so it stays a core module both the proxy
 * ingresses and the agentic path can link. See gateway_pipeline.h. */
#include "gateway_pipeline.h"

int gw_pipeline_run_request(gw_request_t *r, const gw_stage_t *stages, size_t n)
{
   int total = 0;
   size_t i;

   if (!r || !stages)
      return 0;
   for (i = 0; i < n; i++)
   {
      int rc;

      if (!stages[i].fn)
         continue;
      rc = stages[i].fn(r, stages[i].ud);
      if (rc < 0)
         return rc; /* a failed transform short-circuits; do not forward a
                     * half-altered request */
      total += rc;
   }
   return total;
}
