/* gw_stage_governance.c -- see gw_stage_governance.h. */
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "gw_stage_governance.h"

#include <aimee/ir/aimee_ir.h>
#include "gw_response_registry.h"

static gw_governance_decision_provider_fn g_decision_provider;

void gw_response_governance_register_provider(gw_governance_decision_provider_fn provider)
{
   g_decision_provider = provider;
}

int gw_response_governance_enabled(void)
{
   const char *v = getenv("AIMEE_STAGE_GOVERNANCE");
   if (!v || !v[0])
      return 0;
   return strcasecmp(v, "1") == 0 || strcasecmp(v, "on") == 0 || strcasecmp(v, "true") == 0 ||
          strcasecmp(v, "yes") == 0;
}

typedef struct
{
   int policy_active;
} governance_stage_ctx_t;

static int decision_valid(const aimee_governance_decision_t *decision, uint32_t call_count)
{
   if (!decision || call_count > AIMEE_GOVERNANCE_TOOL_COUNT_MAX)
      return 0;
   uint32_t allowed_mask = aimee_governance_mask_for_count(call_count);
   size_t reason_len = strnlen(decision->stop_reason, sizeof(decision->stop_reason));
   return reason_len <= AIMEE_GOVERNANCE_STOP_REASON_MAX &&
          (decision->keep_mask & ~allowed_mask) == 0 && decision->drop_count <= call_count &&
          aimee_governance_popcount(decision->keep_mask) + decision->drop_count == call_count;
}

static int apply_ir_governance(aimee_response_t *response, int policy_active)
{
   if (!response)
      return 0;
   int count = aimee_ir_response_tool_count(response);
   if (!count)
      return 0;
   int removed = 0;
   /* The policy wire carries a bounded mask; it does not limit the IR response.
    * Decide and remove backwards so later chunks cannot shift earlier ordinals. */
   for (int end = count; end > 0;)
   {
      int size =
          end > (int)AIMEE_GOVERNANCE_TOOL_COUNT_MAX ? (int)AIMEE_GOVERNANCE_TOOL_COUNT_MAX : end;
      int start = end - size;
      const char *names[AIMEE_GOVERNANCE_TOOL_COUNT_MAX];
      for (int i = 0; i < size; i++)
         names[i] = aimee_ir_response_tool(response, start + i)->tool_name;
      aimee_governance_decision_t decision = {0};
      /* Policy consumes the canonical state. Opaque native metadata belongs
       * to replay/audit and must not overflow the bounded policy vocabulary. */
      const char *reason = aimee_stop_reason_name(response->stop_reason);
      int valid =
          g_decision_provider &&
          g_decision_provider(policy_active, names, (uint32_t)size, reason, &decision) == 0 &&
          decision_valid(&decision, (uint32_t)size);
      for (int i = size - 1; i >= 0; i--)
         if (!valid || !(decision.keep_mask & (1u << (unsigned)i)))
         {
            aimee_ir_response_drop_tool(response, start + i);
            removed++;
         }
      end = start;
   }
   if (removed && response->stop_reason == AIMEE_STOP_TOOL_USE &&
       !aimee_ir_response_has_tool_use(response))
   {
      response->stop_reason = AIMEE_STOP_END_TURN;
      /* This is a host terminal transition, not a new provider spelling.
       * Otherwise retain the original opaque reason, including truncation. */
      free(response->raw_stop_reason);
      response->raw_stop_reason = NULL;
   }
   return removed;
}

static gw_response_stage_result_t governance_stage(gw_response_ctx_t *ctx, void *ud)
{
   if (!ctx || !ctx->resp || !ud)
      return (gw_response_stage_result_t){GW_RSTAGE_ERROR, 0};
   governance_stage_ctx_t *stage = ud;
   int removed = apply_ir_governance(ctx->resp, stage->policy_active);
   return (gw_response_stage_result_t){GW_RSTAGE_OK, removed};
}

int gw_ir_response_run_governance(aimee_response_t *response, int enabled, int policy_active)
{
   if (!response || !enabled)
      return 0;
   governance_stage_ctx_t stage = {.policy_active = policy_active ? 1 : 0};
   gw_response_ctx_t ctx = {.resp = response};
   gw_response_stage_slot_t slots[] = {{"governance", governance_stage, &stage, enabled}};
   gw_response_stage_t stages[1];
   int n = gw_response_registry_build(slots, 1, stages, 1);
   if (n < 0)
      return -1;
   gw_response_stage_result_t result = gw_response_pipeline_run(&ctx, stages, (size_t)n);
   return result.status == GW_RSTAGE_OK ? result.interventions : -1;
}
