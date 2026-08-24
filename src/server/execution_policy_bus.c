/* C enforcement caller -> required Go execution-policy process over the bus. */
#include "agent_exec.h"

#include <aimee/execution-policy/module_api.h>
#include <aimee/core/event_bus/module_protocol.h>

#include "computer_use.h"
#include "headers/module_json_call.h"

#include <stdio.h>
#include <string.h>

#define EXECUTION_POLICY_TIMEOUT_MS 5000

int policy_load(void)
{
   return obs_bus_module_available(AIMEE_EXECUTION_POLICY_EVENT_TOOL) ? 0 : -1;
}

static void set_reason(char *out, size_t cap, const char *reason)
{
   if (out && cap > 0)
      snprintf(out, cap, "%s", reason ? reason : "execution policy denied the action");
}

int policy_check_tool(const char *tool_name, const char *side_effect, const char *args_json,
                      char *reason_out, size_t reason_len)
{
   if (!tool_name || !tool_name[0] || !args_json)
   {
      set_reason(reason_out, reason_len, "execution-policy input is invalid");
      return -1;
   }

   cJSON *request = cJSON_CreateObject();
   cJSON *arguments = cJSON_Parse(args_json);
   cJSON *computer = cJSON_CreateObject();
   cJSON *domains = cJSON_CreateArray();
   if (!request || !arguments || !computer || !domains)
   {
      cJSON_Delete(request);
      cJSON_Delete(arguments);
      cJSON_Delete(computer);
      cJSON_Delete(domains);
      set_reason(reason_out, reason_len, "execution-policy request could not be built");
      return -1;
   }

   computer_use_policy_t policy;
   computer_use_policy_from_config(&policy);
   cJSON_AddStringToObject(request, "tool", tool_name);
   cJSON_AddStringToObject(request, "side_effect", side_effect ? side_effect : "");
   cJSON_AddItemToObject(request, "arguments", arguments);
   cJSON_AddBoolToObject(computer, "enabled", policy.enabled);
   cJSON_AddStringToObject(computer, "default_navigation", policy.default_navigation);
   cJSON_AddBoolToObject(computer, "redact_sensitive_screenshots",
                         policy.redact_sensitive_screenshots);
   for (int i = 0; i < policy.allowed_domain_count; ++i)
      cJSON_AddItemToArray(domains, cJSON_CreateString(policy.allowed_domains[i]));
   cJSON_AddItemToObject(computer, "allowed_domains", domains);
   cJSON_AddItemToObject(request, "computer_use", computer);

   aimee_module_call_result_t result = AIMEE_MODULE_CALL_INTERNAL;
   cJSON *response = aimee_module_json_call(
       AIMEE_EXECUTION_POLICY_EVENT_TOOL, AIMEE_EXECUTION_POLICY_STAGE_TOOL, request,
       AIMEE_MODULE_MESSAGE_MAX_BODY, EXECUTION_POLICY_TIMEOUT_MS, &result);
   if (!response)
   {
      char failure[160];
      snprintf(failure, sizeof failure, "execution-policy module failed: %s",
               aimee_module_call_result_name(result));
      set_reason(reason_out, reason_len, failure);
      return -1; /* required authorization is fail-closed */
   }

   const cJSON *allowed = cJSON_GetObjectItemCaseSensitive(response, "allowed");
   const cJSON *reason = cJSON_GetObjectItemCaseSensitive(response, "reason");
   if (!cJSON_IsBool(allowed) || !cJSON_IsString(reason) || strlen(reason->valuestring) > 255)
   {
      cJSON_Delete(response);
      set_reason(reason_out, reason_len, "execution-policy returned an invalid decision");
      return -1;
   }
   int permit = cJSON_IsTrue(allowed);
   set_reason(reason_out, reason_len, reason->valuestring);
   cJSON_Delete(response);
   return permit ? 0 : -1;
}
