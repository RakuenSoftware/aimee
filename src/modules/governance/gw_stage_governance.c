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
   /* The module answers in one vocabulary whatever wire the response came from, so
    * the string is for the audit; the canonical value is what emitters render. It
    * only changes the reason when every call was dropped (the turn now ends) or the
    * upstream gave none (surviving calls make it a tool turn). */
   int had_reason = parsed->stop_reason[0] != '\0';
   snprintf(parsed->stop_reason, sizeof(parsed->stop_reason), "%s", decision->stop_reason);
   if (write_index == 0)
      parsed->stop = AIMEE_STOP_END_TURN;
   else if (!had_reason)
      parsed->stop = AIMEE_STOP_TOOL_USE;
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

int gw_ir_response_run_governance(aimee_response_t *response, int enabled, int policy_active)
{
   if (!response || !enabled)
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
      const char *reason = response->raw_stop_reason
                               ? response->raw_stop_reason
                               : aimee_stop_reason_name(response->stop_reason);
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
   if (removed)
   {
      if (response->stop_reason == AIMEE_STOP_TOOL_USE)
         response->stop_reason =
             aimee_ir_response_has_tool_use(response) ? AIMEE_STOP_TOOL_USE : AIMEE_STOP_END_TURN;
      free(response->raw_stop_reason);
      response->raw_stop_reason = strdup(aimee_stop_reason_name(response->stop_reason));
   }
   return removed;
}
