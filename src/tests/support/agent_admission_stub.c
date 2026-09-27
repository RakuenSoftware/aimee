/* Agent configuration tests do not install memory or action owners. Refuse any
 * native projection; absent task instructions preserve the baseline request.
 * Real admission and action behavior have dedicated owner/transport suites. */
#include "ingress_preinject.h"
#include "agent_exec.h"
#include <stddef.h>

char *ingress_preinject_task_instructions(const char *instructions, const char *query)
{
   (void)instructions;
   (void)query;
   return NULL;
}
int ingress_preinject_accept_native_projection(const cJSON *projection)
{
   (void)projection;
   return -1;
}
void policy_action_attempt(const char *attempt)
{
   (void)attempt;
}
