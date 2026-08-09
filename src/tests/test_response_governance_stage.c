/* test_response_governance_stage.c -- production governance response seam.
 * Proves that enabled decisions use the registered event-bus bridge, disabled
 * decisions do not call it, and bridge failures deny pending tool calls rather
 * than falling back to local policy evaluation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_protocol.h"
#include "gw_stage_governance.h"

static int g_provider_calls;
static int g_policy_active;
static int g_provider_mode;

static int denied_tool(const char *name)
{
   return strcmp(name, "Agent") == 0 || strcmp(name, "spawn_agent") == 0 ||
          strcmp(name, "RemoteTrigger") == 0 || strcmp(name, "Task") == 0;
}

static int fake_event_bus_provider(int policy_active, const char *const *tool_names,
                                   uint32_t tool_count, const char *stop_reason,
                                   aimee_governance_decision_t *decision)
{
   g_provider_calls++;
   g_policy_active = policy_active;
   if (g_provider_mode == 1)
      return -1;
   memset(decision, 0, sizeof(*decision));
   if (g_provider_mode == 2)
   {
      decision->keep_mask = UINT32_MAX;
      return 0;
   }

   decision->keep_mask = aimee_governance_mask_for_count(tool_count);
   snprintf(decision->stop_reason, sizeof(decision->stop_reason), "%s",
            stop_reason ? stop_reason : "");
   if (!policy_active)
      return 0;

   decision->keep_mask = 0;
   for (uint32_t i = 0; i < tool_count; ++i)
   {
      if (denied_tool(tool_names[i]))
         decision->drop_count++;
      else
         decision->keep_mask |= 1u << i;
   }
   uint32_t kept = tool_count - decision->drop_count;
   if (!decision->stop_reason[0] || kept == 0)
      snprintf(decision->stop_reason, sizeof(decision->stop_reason), "%s",
               kept > 0 ? "tool_use" : "end_turn");
   return 0;
}

static char *copy_string(const char *value)
{
   size_t len = strlen(value) + 1;
   char *copy = malloc(len);
   assert(copy != NULL);
   memcpy(copy, value, len);
   return copy;
}

static void seed_call(parsed_response_t *parsed, int index, const char *name)
{
   snprintf(parsed->calls[index].name, sizeof(parsed->calls[index].name), "%s", name);
   parsed->calls[index].arguments = copy_string("{}");
}

int main(void)
{
   /* The stage and separately supervised process must share the descriptor's
    * default-OFF posture. Otherwise an unspecified config enables this stage
    * without starting the module, and the fail-closed provider path drops every
    * ordinary tool call. Legacy env activation remains an explicit opt-in. */
   unsetenv("AIMEE_STAGE_GOVERNANCE");
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "0", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "off", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "false", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "1", 1);
   assert(gw_response_governance_enabled() == 1);
   setenv("AIMEE_STAGE_GOVERNANCE", "yes", 1);
   assert(gw_response_governance_enabled() == 1);
   setenv("AIMEE_STAGE_GOVERNANCE", "nope", 1);
   assert(gw_response_governance_enabled() == 0); /* invalid values do not opt in */

   /* Regression: the exact unspecified/default path used by delegate responses
    * must bypass governance when no process/provider is present. This is the
    * production failure that previously became synthetic timeout results. */
   unsetenv("AIMEE_STAGE_GOVERNANCE");
   gw_response_governance_register_provider(NULL);
   {
      parsed_response_t pr;
      memset(&pr, 0, sizeof(pr));
      pr.call_count = 1;
      seed_call(&pr, 0, "read_file");
      assert(gw_response_run_governance(&pr, gw_response_governance_enabled(), 1) == 0);
      assert(pr.call_count == 1);
      assert(strcmp(pr.calls[0].name, "read_file") == 0);
      free(pr.calls[0].arguments);
   }

   gw_response_governance_register_provider(fake_event_bus_provider);

   /* Enabled: the event-bus provider decides; the C seam only applies its mask. */
   {
      parsed_response_t pr;
      memset(&pr, 0, sizeof(pr));
      pr.call_count = 2;
      seed_call(&pr, 0, "Agent");
      seed_call(&pr, 1, "read_file");
      g_provider_calls = g_provider_mode = 0;
      assert(gw_response_run_governance(&pr, 1, 1) == 1);
      assert(g_provider_calls == 1 && g_policy_active == 1);
      assert(pr.call_count == 1);
      assert(strcmp(pr.calls[0].name, "read_file") == 0);
      assert(strcmp(pr.stop_reason, "tool_use") == 0);
      free(pr.calls[0].arguments);
   }

   /* Policy-off still crosses the module boundary and returns an explicit keep-all decision. */
   {
      parsed_response_t pr;
      memset(&pr, 0, sizeof(pr));
      pr.call_count = 1;
      seed_call(&pr, 0, "Agent");
      g_provider_calls = g_provider_mode = 0;
      assert(gw_response_run_governance(&pr, 1, 0) == 0);
      assert(g_provider_calls == 1 && g_policy_active == 0);
      assert(pr.call_count == 1);
      free(pr.calls[0].arguments);
   }

   /* Disabled module: no event-bus call and no mutation. */
   {
      parsed_response_t pr;
      memset(&pr, 0, sizeof(pr));
      pr.call_count = 1;
      seed_call(&pr, 0, "Agent");
      g_provider_calls = 0;
      assert(gw_response_run_governance(&pr, 0, 1) == 0);
      assert(g_provider_calls == 0 && pr.call_count == 1);
      free(pr.calls[0].arguments);
   }

   /* An unavailable or malformed module answer fails closed; local policing is never used. */
   for (g_provider_mode = 1; g_provider_mode <= 2; ++g_provider_mode)
   {
      parsed_response_t pr;
      memset(&pr, 0, sizeof(pr));
      pr.call_count = 1;
      seed_call(&pr, 0, "read_file");
      assert(gw_response_run_governance(&pr, 1, 1) == 1);
      assert(pr.call_count == 0);
      assert(strcmp(pr.stop_reason, "end_turn") == 0);
   }

   gw_response_governance_register_provider(NULL);
   {
      parsed_response_t pr;
      memset(&pr, 0, sizeof(pr));
      pr.call_count = 1;
      seed_call(&pr, 0, "read_file");
      assert(gw_response_run_governance(&pr, 1, 1) == 1);
      assert(pr.call_count == 0);
   }

   /* A corrupt oversized count is bounded before fail-closed cleanup. */
   {
      parsed_response_t pr;
      memset(&pr, 0, sizeof(pr));
      for (int i = 0; i < AGENT_MAX_TOOL_CALLS; ++i)
         seed_call(&pr, i, "read_file");
      pr.call_count = AGENT_MAX_TOOL_CALLS + 1;
      assert(gw_response_run_governance(&pr, 1, 1) == AGENT_MAX_TOOL_CALLS);
      assert(pr.call_count == 0);
   }

   /* NULL-safe. */
   assert(gw_response_run_governance(NULL, 1, 1) == 0);

   printf("ok\n");
   return 0;
}
