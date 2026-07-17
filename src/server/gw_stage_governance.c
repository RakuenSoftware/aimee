/* gw_stage_governance.c -- see gw_stage_governance.h. */
#include <stdlib.h>
#include <strings.h> /* strcasecmp */

#include "gw_stage_governance.h"

#include "aimee.h" /* size macros for agent_types.h */
#include "agent_protocol.h"
#include "gateway_policy.h"
#include "gw_response_registry.h"

int gw_response_governance_enabled(void)
{
   const char *v = getenv("AIMEE_STAGE_GOVERNANCE");
   if (!v || !v[0])
      return 1;
   return !(strcasecmp(v, "0") == 0 || strcasecmp(v, "off") == 0 || strcasecmp(v, "false") == 0 ||
            strcasecmp(v, "no") == 0);
}

/* The governance response stage: response tool-policing over the parsed reply. police is a
 * pure mutation (drops disallowed tool_use), internally a no-op at the default config, so
 * this always returns OK; a future rejecting governance stage would return REJECT here. */
static gw_response_stage_result_t governance_stage(gw_response_ctx_t *ctx, void *ud)
{
   (void)ud;
   int drops = gateway_policy_police_parsed_response(ctx->resp);
   gw_response_stage_result_t r = {GW_RSTAGE_OK, drops > 0 ? drops : 0};
   return r;
}

int gw_response_run_governance(struct parsed_response *parsed)
{
   if (!parsed)
      return 0;
   gw_response_ctx_t ctx;
   ctx.resp = parsed;
   gw_response_stage_slot_t slots[] = {
       {"governance", governance_stage, NULL, gw_response_governance_enabled()},
   };
   gw_response_stage_t stages[2];
   int n = gw_response_registry_build(slots, sizeof(slots) / sizeof(slots[0]), stages, 2);
   if (n < 0)
      return 0; /* static 1-slot catalog cannot fail; fail-safe: no governance */
   gw_response_stage_result_t res = gw_response_pipeline_run(&ctx, stages, (size_t)n);
   return res.interventions;
}
