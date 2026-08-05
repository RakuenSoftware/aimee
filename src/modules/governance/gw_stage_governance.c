/* gw_stage_governance.c -- see gw_stage_governance.h. */
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "gw_stage_governance.h"

#include "aimee.h" /* size macros for agent_types.h */
#include "agent_protocol.h"
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

static int apply_decision(parsed_response_t *parsed, const aimee_governance_decision_t *decision)
{
   int original_count = parsed->call_count;
   int write_index = 0;
   for (int i = 0; i < original_count; ++i)
   {
      if ((decision->keep_mask & (1u << (unsigned)i)) == 0)
      {
         free(parsed->calls[i].arguments);
         parsed->calls[i].arguments = NULL;
         continue;
      }
      if (write_index != i)
         parsed->calls[write_index] = parsed->calls[i];
      write_index++;
   }
   parsed->call_count = write_index;
   snprintf(parsed->stop_reason, sizeof(parsed->stop_reason), "%s", decision->stop_reason);
   return original_count - write_index;
}

static int fail_closed(parsed_response_t *parsed)
{
   aimee_governance_decision_t decision;
   memset(&decision, 0, sizeof(decision));
   decision.drop_count = (uint32_t)parsed->call_count;
   snprintf(decision.stop_reason, sizeof(decision.stop_reason), "%s", "end_turn");
   return apply_decision(parsed, &decision);
}

/* The governance response stage applies a decision returned by the event-bus
 * module. Parsed-response compaction and ownership stay local, but policy
 * evaluation has no in-process fallback. */
static gw_response_stage_result_t governance_stage(gw_response_ctx_t *ctx, void *ud)
{
   governance_stage_ctx_t *stage = ud;
   parsed_response_t *parsed = ctx->resp;
   if (parsed->call_count < 0)
      parsed->call_count = 0;
   uint32_t call_count = parsed->call_count > 0 ? (uint32_t)parsed->call_count : 0;
   const char *tool_names[AIMEE_GOVERNANCE_TOOL_COUNT_MAX];
   aimee_governance_decision_t decision;
   memset(&decision, 0, sizeof(decision));

   if (call_count > AIMEE_GOVERNANCE_TOOL_COUNT_MAX)
   {
      parsed->call_count = AIMEE_GOVERNANCE_TOOL_COUNT_MAX;
      int drops = fail_closed(parsed);
      gw_response_stage_result_t r = {GW_RSTAGE_OK, drops};
      return r;
   }
   for (uint32_t i = 0; i < call_count; ++i)
      tool_names[i] = parsed->calls[i].name;

   if (!g_decision_provider ||
       g_decision_provider(stage->policy_active, tool_names, call_count, parsed->stop_reason,
                           &decision) != 0 ||
       !decision_valid(&decision, call_count))
   {
      int drops = call_count > 0 ? fail_closed(parsed) : 0;
      gw_response_stage_result_t r = {GW_RSTAGE_OK, drops};
      return r;
   }

   int drops = apply_decision(parsed, &decision);
   gw_response_stage_result_t r = {GW_RSTAGE_OK, drops > 0 ? drops : 0};
   return r;
}

int gw_response_run_governance(struct parsed_response *parsed, int enabled, int policy_active)
{
   if (!parsed)
      return 0;
   governance_stage_ctx_t stage = {.policy_active = policy_active ? 1 : 0};
   gw_response_ctx_t ctx;
   ctx.resp = parsed;
   gw_response_stage_slot_t slots[] = {
       {"governance", governance_stage, &stage, enabled},
   };
   gw_response_stage_t stages[2];
   int n = gw_response_registry_build(slots, sizeof(slots) / sizeof(slots[0]), stages, 2);
   if (n < 0)
      return 0; /* static 1-slot catalog cannot fail; fail-safe: no governance */
   gw_response_stage_result_t res = gw_response_pipeline_run(&ctx, stages, (size_t)n);
   return res.interventions;
}
