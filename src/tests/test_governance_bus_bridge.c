/* Exercise the production bridge's owned identity projection and reply validation. */
#include "../server/module_stage_adapters.c"

#include <assert.h>
#include <stdio.h>

static const char *expected_name, *expected_namespace;
static int owner_calls, owner_failure, retired_reply;

ouro_module_call_result_t
obs_bus_module_call(uint32_t event_kind, uint32_t stage_id, uint64_t trace_id, uint64_t deadline_ns,
                    const void *request_body, uint32_t request_len, void *response_body,
                    uint32_t response_capacity, uint32_t *response_len,
                    ouro_module_cancelled_fn cancelled, void *cancel_context)
{
   (void)cancelled;
   (void)cancel_context;
   assert(event_kind == AIMEE_GOVERNANCE_EVENT_EVALUATE);
   assert(stage_id == AIMEE_GOVERNANCE_STAGE_EVALUATE);
   assert(trace_id && deadline_ns);
   owner_calls++;
   char *encoded = malloc((size_t)request_len + 1);
   assert(encoded);
   memcpy(encoded, request_body, request_len);
   encoded[request_len] = 0;
   cJSON *request = cJSON_Parse(encoded);
   free(encoded);
   assert(request);
   assert(cJSON_GetObjectItemCaseSensitive(request, "version")->valueint == 2);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "policy_active")));
   assert(
       !strcmp(cJSON_GetObjectItemCaseSensitive(request, "stop_reason")->valuestring, "tool_use"));
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(request, "tools");
   assert(cJSON_GetArraySize(tools) == 1);
   cJSON *tool = cJSON_GetArrayItem(tools, 0);
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(tool, "name")->valuestring, expected_name));
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(tool, "namespace")->valuestring,
                  expected_namespace));
   cJSON_Delete(request);
   if (owner_failure)
      return OURO_MODULE_CALL_CAPABILITY_ABSENT;
   assert(aimee_governance_response_encode(1, 0, "tool_use", response_body, response_capacity) ==
          0);
   if (retired_reply)
      aimee_governance_put_u32((uint8_t *)response_body + 4, 1);
   *response_len = AIMEE_GOVERNANCE_RESPONSE_LEN;
   return OURO_MODULE_CALL_OK;
}

int main(void)
{
   char name[1801], namespace_name[1101];
   memset(name, 'n', sizeof name - 1);
   memset(namespace_name, 's', sizeof namespace_name - 1);
   name[sizeof name - 1] = namespace_name[sizeof namespace_name - 1] = 0;
   expected_name = name;
   expected_namespace = namespace_name;
   aimee_tool_identity_view_t tool = {name, namespace_name};
   aimee_governance_decision_t decision;
   assert(governance_evaluate(1, &tool, 1, "tool_use", &decision) == 0);
   assert(owner_calls == 1 && decision.keep_mask == 1 && decision.drop_count == 0);
   owner_failure = 1;
   assert(governance_evaluate(1, &tool, 1, "tool_use", &decision) != 0);
   owner_failure = 0;
   retired_reply = 1;
   assert(governance_evaluate(1, &tool, 1, "tool_use", &decision) != 0);
   int before = owner_calls;
   assert(governance_evaluate(1, &tool, 17, "tool_use", &decision) != 0);
   tool.name = "";
   assert(governance_evaluate(1, &tool, 1, "tool_use", &decision) != 0);
   assert(owner_calls == before);
   puts("governance bus bridge: complete identities and atomic refusal passed");
   return 0;
}
