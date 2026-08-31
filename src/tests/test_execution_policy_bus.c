/* The C side transports and applies a Go execution-policy verdict; it never
 * grows an authorization fallback of its own. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "agent_exec.h"
#include "computer_use.h"
#include <aimee/core/event_bus/module_protocol.h>
#include "headers/module_json_call.h"

static int g_available;
static const char *g_reply;
static aimee_module_call_result_t g_result;
static int g_calls;

int obs_bus_module_available(uint32_t event_kind)
{
   assert(event_kind == 8449u);
   return g_available;
}

const char *aimee_module_call_result_name(aimee_module_call_result_t result)
{
   (void)result;
   return "test_result";
}

void computer_use_policy_from_config(computer_use_policy_t *policy)
{
   memset(policy, 0, sizeof *policy);
   policy->enabled = 1;
   snprintf(policy->default_navigation, sizeof policy->default_navigation, "%s", "approve");
   policy->allowed_domain_count = 1;
   snprintf(policy->allowed_domains[0], sizeof policy->allowed_domains[0], "%s", "localhost");
}

cJSON *aimee_module_json_call(uint32_t event_kind, uint32_t stage_id, cJSON *request,
                              size_t max_body, int timeout_ms, aimee_module_call_result_t *result)
{
   assert(event_kind == 8449u && stage_id == 1u);
   assert(max_body == AIMEE_MODULE_MESSAGE_MAX_BODY && timeout_ms == 5000);
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(request, "tool")));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(request, "arguments")));
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(request, "computer_use")));
   cJSON_Delete(request);
   g_calls++;
   *result = g_result;
   return g_reply ? cJSON_Parse(g_reply) : NULL;
}

int main(void)
{
   char reason[256];
   g_available = 0;
   assert(policy_load() == -1);
   g_available = 1;
   assert(policy_load() == 0);

   g_calls = 0;
   assert(policy_check_tool(NULL, "none", "{}", reason, sizeof reason) == -1);
   assert(g_calls == 0 && strstr(reason, "invalid"));

   g_result = AIMEE_MODULE_CALL_DEADLINE_EXCEEDED;
   g_reply = NULL;
   assert(policy_check_tool("read_file", "filesystem", "{}", reason, sizeof reason) == -1);
   assert(strstr(reason, "test_result"));

   g_result = AIMEE_MODULE_CALL_OK;
   g_reply = "{\"allowed\":true,\"reason\":\"policy permits\"}";
   assert(policy_check_tool("read_file", "filesystem", "{}", reason, sizeof reason) == 0);
   assert(strcmp(reason, "policy permits") == 0);

   g_reply = "{\"allowed\":false,\"reason\":\"policy denies\"}";
   assert(policy_check_tool("write_file", "filesystem", "{}", reason, sizeof reason) == -1);
   assert(strcmp(reason, "policy denies") == 0);

   g_reply = "{\"allowed\":\"yes\",\"reason\":\"bad schema\"}";
   assert(policy_check_tool("read_file", "none", "{}", reason, sizeof reason) == -1);
   assert(strstr(reason, "invalid decision"));

   puts("test_execution_policy_bus: OK");
   return 0;
}
