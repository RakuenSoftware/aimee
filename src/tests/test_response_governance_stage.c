/* test_response_governance_stage.c -- Slice 2 of the response seam: governance as a
 * togglable module. Proves the toggle THROUGH the run helper: enabled -> police runs,
 * disabled -> police is not called. police is stubbed (counting) so the test stays light. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "agent_protocol.h"
#include "gw_stage_governance.h"

/* Counting stub for gateway_policy_police_parsed_response (avoids the guardrails graph). */
static int g_police_calls;
int gateway_policy_police_parsed_response(parsed_response_t *p)
{
   (void)p;
   g_police_calls++;
   return 3; /* pretend 3 tool_use drops */
}

int main(void)
{
   /* toggle logic: default-ON; explicit disable tokens; full-token match (not first byte). */
   unsetenv("AIMEE_STAGE_GOVERNANCE");
   assert(gw_response_governance_enabled() == 1);
   setenv("AIMEE_STAGE_GOVERNANCE", "0", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "off", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "false", 1);
   assert(gw_response_governance_enabled() == 0);
   setenv("AIMEE_STAGE_GOVERNANCE", "nope", 1);
   assert(gw_response_governance_enabled() == 1); /* full-token, not first byte */

   parsed_response_t pr;
   memset(&pr, 0, sizeof(pr));

   /* ENABLED (default): governance runs -> police called, drop count returned. */
   unsetenv("AIMEE_STAGE_GOVERNANCE");
   g_police_calls = 0;
   assert(gw_response_run_governance(&pr) == 3);
   assert(g_police_calls == 1);

   /* DISABLED: governance stage omitted -> police NOT called, 0 returned. */
   setenv("AIMEE_STAGE_GOVERNANCE", "0", 1);
   g_police_calls = 0;
   assert(gw_response_run_governance(&pr) == 0);
   assert(g_police_calls == 0);

   /* NULL-safe. */
   assert(gw_response_run_governance(NULL) == 0);

   printf("ok\n");
   return 0;
}
