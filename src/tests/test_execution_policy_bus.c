/* The C side transports and applies a Go execution-policy verdict; it never
 * grows an authorization fallback of its own. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "agent_exec.h"
#include "computer_use.h"
#include "request_context.h"
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

static request_context_t test_context;
static int context_active;
static int owner_calls;
static const char *owner_reply = "{\"status\":\"ok\"}";
const request_context_t *request_context_get(void)
{
   return context_active ? &test_context : NULL;
}
int request_context_set_exploration_binding(const char *binding)
{
   snprintf(test_context.exploration_binding, sizeof(test_context.exploration_binding), "%s",
            binding);
   return 0;
}
int agent_get_durable_job_id(void)
{
   return 7;
}
size_t agent_tool_output_cap(void)
{
   return 4096;
}
int db1_session_exploration_apply(const char *principal, const char *sid, const char *request,
                                  char *reply, size_t reply_len)
{
   assert(strcmp(principal, "alice") == 0 && strcmp(sid, "session") == 0);
   cJSON *body = cJSON_Parse(request);
   assert(cJSON_IsObject(body));
   cJSON_Delete(body);
   owner_calls++;
   if (!owner_reply)
      return -1;
   snprintf(reply, reply_len, "%s", owner_reply);
   return 0;
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

   context_active = 1;
   snprintf(test_context.principal, sizeof(test_context.principal), "alice");
   snprintf(test_context.request_id, sizeof(test_context.request_id), "request");
   cJSON *offer = cJSON_Parse("{\"memory_owner\":\"m1\",\"index_generation\":\"i1\"}");
   assert(policy_prepare_exploration(offer, "session", "/project", "project") == 0);
   cJSON_Delete(offer);
   assert(owner_calls == 1 && test_context.exploration_binding[0]);
   g_reply = "{\"allowed\":false,\"reason\":\"baseline denies\"}";
   assert(policy_check_tool_attempt("bash", "filesystem", "{}", "a", reason, sizeof reason) == -1);
   assert(owner_calls == 1);
   g_reply = "{\"allowed\":true,\"reason\":\"baseline "
             "allows\",\"exploration\":{\"mode\":\"observe\",\"accounting_required\":true}}";
   owner_reply = "{\"allowed\":false,\"reason\":\"operator_exploration_budget_exhausted\"}";
   assert(policy_check_tool_attempt("bash", "filesystem", "{\"command\":\"rg foo\"}", "a", reason,
                                    sizeof reason) == -1);
   assert(owner_calls == 2 && strstr(reason, "budget_exhausted"));
   owner_reply = "{\"allowed\":true,\"reason\":\"observed\"}";
   assert(policy_check_tool_attempt("bash", "filesystem", "{\"command\":\"rg foo\"}", "b", reason,
                                    sizeof reason) == 0);
   owner_reply = NULL;
   assert(policy_check_tool_attempt("bash", "filesystem", "{\"command\":\"rg foo\"}", "c", reason,
                                    sizeof reason) == -1);
   /* Adaptive issuer failure falls back to baseline only without operator ceilings. */
   g_reply =
       "{\"allowed\":true,\"reason\":\"baseline allows\",\"exploration\":{\"mode\":\"observe\"}}";
   assert(policy_check_tool_attempt("bash", "filesystem", "{\"command\":\"rg foo\"}", "d", reason,
                                    sizeof reason) == 0);
   puts("test_execution_policy_bus: OK");
   return 0;
}
