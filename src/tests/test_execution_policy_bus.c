/* The C side transports and applies a Go execution-policy verdict; it never
 * grows an authorization fallback of its own. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "agent_exec.h"
#include "computer_use.h"
#include "request_context.h"
#include <ouroboros/module_protocol.h>
#include "headers/module_json_call.h"
#include "modules/workspace/workspace_provider.h"
#include <stdlib.h>

void policy_action_attempt(const char *attempt)
{
   (void)attempt;
}
int policy_action_inherit(const char *session)
{
   (void)session;
   return 0;
}

static int g_available;
static const char *g_reply;
static ouro_module_call_result_t g_result;
static int g_calls;
static uint32_t expected_categories;
static void check_categories(const cJSON *request)
{
   const cJSON *array = cJSON_GetObjectItemCaseSensitive(request, "policy_categories");
   if (!expected_categories)
   {
      assert(!array);
      return;
   }
   assert(cJSON_IsArray(array));
   uint32_t mask = 0;
   const cJSON *label = NULL;
   cJSON_ArrayForEach(label, array)
   {
      assert(cJSON_IsString(label));
      uint32_t found = 0;
      for (uint32_t bit = 1; bit <= AIMEE_TOOL_POLICY_CONTROL; bit <<= 1)
         if (!strcmp(label->valuestring, aimee_tool_policy_name(bit)))
            found = bit;
      assert(found && !(mask & found));
      mask |= found;
   }
   assert(mask == expected_categories);
}

int obs_bus_module_available(uint32_t event_kind)
{
   assert(event_kind == 8449u);
   return g_available;
}

const char *ouro_module_call_result_name(ouro_module_call_result_t result)
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
                              size_t max_body, int timeout_ms, ouro_module_call_result_t *result)
{
   assert(event_kind == 8449u && stage_id == 1u);
   assert(max_body == OURO_MODULE_MESSAGE_MAX_BODY && timeout_ms == 5000);
   check_categories(request);
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
static int test_job = 7;
static workspace_provider_t test_provider = {.kind = WS_PROVIDER_SHARED};
static char issued_binding[4096];
static int test_freshness;
static int check_session_calls;
const workspace_provider_t *workspace_provider_active(void)
{
   return &test_provider;
}
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
const char *run_cmd_get_cwd(void)
{
   return "/tmp";
}
int agent_get_durable_job_id(void)
{
   return test_job;
}
size_t agent_tool_output_cap(void)
{
   return 4096;
}
int db1_session_exploration_apply(const char *principal, const char *sid, const char *request,
                                  char *reply, size_t reply_len)
{
   assert(strcmp(principal, "webuser:alice") == 0 && strcmp(sid, "session") == 0);
   cJSON *body = cJSON_Parse(request);
   assert(cJSON_IsObject(body));
   const char *operation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "operation"));
   if (operation && strcmp(operation, "prepare") == 0 && owner_reply)
   {
      cJSON *contract = cJSON_CreateObject();
      cJSON *result = cJSON_CreateObject();
      cJSON *binding = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(body, "binding"), 1);
      cJSON_ReplaceItemInObjectCaseSensitive(binding, "worktree_generation",
                                             cJSON_CreateString("host-observed-generation"));
      assert(cJSON_PrintPreallocated(binding, issued_binding, sizeof(issued_binding), 0));
      cJSON_AddItemToObject(contract, "binding", binding);
      cJSON_AddItemToObject(result, "contract", contract);
      assert(cJSON_PrintPreallocated(result, reply, (int)reply_len, 0));
      cJSON_Delete(result);
      cJSON_Delete(body);
      owner_calls++;
      return 0;
   }
   if (operation && strcmp(operation, "bind_session") == 0 && owner_reply)
   {
      assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "canonical_path")),
                    "/tmp") == 0);
      cJSON *result = cJSON_CreateObject();
      cJSON_AddItemToObject(result, "binding", cJSON_Parse(issued_binding));
      assert(cJSON_PrintPreallocated(result, reply, (int)reply_len, 0));
      cJSON_Delete(result);
      cJSON_Delete(body);
      owner_calls++;
      return 0;
   }
   if (operation && strcmp(operation, "check_session") == 0)
   {
      check_session_calls++;
      const cJSON *index = cJSON_GetObjectItemCaseSensitive(body, "index_observation");
      const cJSON *owner = cJSON_GetObjectItemCaseSensitive(body, "owner_observation");
      if (test_provider.kind == WS_PROVIDER_SHARED && test_freshness)
      {
         assert(cJSON_GetObjectItemCaseSensitive(index, "http_status")->valueint == 200);
         assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(owner, "generation_only")));
      }
      else
         assert(!index && !owner);
   }
   check_categories(body);
   cJSON_Delete(body);
   owner_calls++;
   if (!owner_reply)
      return -1;
   snprintf(reply, reply_len, "%s", owner_reply);
   return 0;
}

char *kb_client_index_generation_check(const char *project, const char *generation, int *status_out)
{
   (void)project;
   (void)generation;
   if (status_out)
      *status_out = test_freshness ? 200 : 0;
   return test_freshness ? strdup("{\"status\":\"ok\",\"project\":\"project\"}") : NULL;
}

int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *request,
                                                    int timeout_ms, cJSON **response)
{
   assert(strcmp(method, "memory.runtime") == 0 && timeout_ms == 500);
   const char *operation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "operation"));
   assert(!strcmp(operation, "exploration-owner-observe") ||
          !strcmp(operation, "exploration-owner-generation"));
   *response =
       test_freshness
           ? cJSON_Parse("{\"status\":\"ok\",\"generation_only\":true,\"memory_owner\":\"m1\"}")
           : NULL;
   return test_freshness ? 1 : 0;
}

static int gate_calls, gate_stage, gate_clears;
static int source_gate_probe(const char *tool, const char *args, const char *attempt,
                             uint32_t categories, int stage, char *reason, size_t cap)
{
   (void)categories;
   (void)reason;
   (void)cap;
   if (stage == 2)
   {
      gate_clears++;
      return 0;
   }
   assert(!strcmp(tool, "write_file") && !strcmp(args, "{\"path\":\"/workspace/target\"}") &&
          !strcmp(attempt, "host-attempt"));
   gate_calls++;
   gate_stage = stage;
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

   g_result = OURO_MODULE_CALL_DEADLINE_EXCEEDED;
   g_reply = NULL;
   assert(policy_check_tool("read_file", "filesystem", "{}", reason, sizeof reason) == -1);
   assert(strstr(reason, "test_result"));

   g_result = OURO_MODULE_CALL_OK;
   g_reply = "{\"allowed\":true,\"reason\":\"policy permits\"}";
   assert(policy_check_tool("read_file", "filesystem", "{}", reason, sizeof reason) == 0);
   assert(strcmp(reason, "policy permits") == 0);
   aimee_tool_metadata_t metadata = {AIMEE_TOOL_CLASS_READ, AIMEE_TOOL_POLICY_READ,
                                     AIMEE_TOOL_POLICY_READ | AIMEE_TOOL_POLICY_FILESYSTEM};
   expected_categories = metadata.categories;
   assert(policy_check_tool_metadata_attempt("read_file", &metadata, "operator_label", "{}",
                                             "metadata-attempt", reason, sizeof reason) == 0);
   int before = g_calls;
   metadata.categories |= 1u << 8;
   assert(policy_check_tool_metadata_attempt("read_file", &metadata, "read", "{}",
                                             "invalid-metadata", reason, sizeof reason) == -1);
   assert(g_calls == before);
   metadata.categories = expected_categories;
   expected_categories = 0;

   g_reply = "{\"allowed\":false,\"reason\":\"policy denies\"}";
   assert(policy_check_tool("write_file", "filesystem", "{}", reason, sizeof reason) == -1);
   assert(strcmp(reason, "policy denies") == 0);

   g_reply = "{\"allowed\":\"yes\",\"reason\":\"bad schema\"}";
   assert(policy_check_tool("read_file", "none", "{}", reason, sizeof reason) == -1);
   assert(strstr(reason, "invalid decision"));

   context_active = 1;
   snprintf(test_context.principal, sizeof(test_context.principal), "alice");
   test_context.vault_transport = ATTEST_WEBCHAT_TRUSTED;
   snprintf(test_context.vault_principal, sizeof(test_context.vault_principal), "webuser:alice");
   snprintf(test_context.request_id, sizeof(test_context.request_id), "request");
   cJSON *offer = cJSON_Parse("{\"memory_owner\":\"m1\",\"index_generation\":\"i1\"}");
   assert(policy_prepare_exploration(offer, "session", "/project", "project") == 0);
   cJSON_Delete(offer);
   assert(owner_calls == 1 && test_context.exploration_binding[0]);
   assert(strstr(test_context.exploration_binding, "host-observed-generation"));
   cJSON *parent_binding = cJSON_Parse(test_context.exploration_binding);
   assert(
       strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parent_binding, "budget_task")),
              "job:7") == 0);
   cJSON_Delete(parent_binding);
   test_job = 8;
   offer = cJSON_Parse("{\"memory_owner\":\"m1\",\"index_generation\":\"i1\"}");
   assert(policy_prepare_exploration(offer, "session", "/project", "project") == 0);
   cJSON_Delete(offer);
   parent_binding = cJSON_Parse(test_context.exploration_binding);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parent_binding, "task")),
                 "job:8") == 0);
   assert(
       strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parent_binding, "budget_task")),
              "job:7") == 0);
   cJSON_Delete(parent_binding);
   owner_calls = 1; /* Subsequent admission assertions count from this checkpoint. */
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
   expected_categories = metadata.categories;
   assert(policy_check_tool_metadata_attempt("read_file", &metadata, "operator_label", "{}",
                                             "adaptive-metadata", reason, sizeof reason) == 0);
   expected_categories = 0;
   owner_reply = NULL;
   assert(policy_check_tool_attempt("bash", "filesystem", "{\"command\":\"rg foo\"}", "c", reason,
                                    sizeof reason) == -1);
   /* Adaptive issuer failure falls back to baseline only without operator ceilings. */
   g_reply =
       "{\"allowed\":true,\"reason\":\"baseline allows\",\"exploration\":{\"mode\":\"observe\"}}";
   assert(policy_check_tool_attempt("bash", "filesystem", "{\"command\":\"rg foo\"}", "d", reason,
                                    sizeof reason) == 0);
   test_job = 0;
   owner_reply = "{\"allowed\":true,\"reason\":\"host_recorded_indexed_fallback\"}";
   offer = cJSON_Parse("{\"memory_owner\":\"m1\",\"index_generation\":\"7\"}");
   assert(policy_prepare_exploration(offer, "session", "/project", "project") == 0);
   cJSON_Delete(offer);
   test_freshness = 1;
   assert(policy_check_session_tool("session", "bash", "{\"command\":\"rg foo\"}", "hook-fallback",
                                    reason, sizeof(reason)) == 0);
   owner_reply =
       "{\"allowed\":false,\"reason\":\"adaptive_exploration_budget_exhausted\","
       "\"exploration\":{\"alternatives\":[\"find_symbol\",\"context_contract_expand\"]}}";
   assert(policy_check_session_tool("session", "bash", "{\"command\":\"rg foo\"}", "hook-deny",
                                    reason, sizeof(reason)) == -1);
   assert(strstr(reason, "adaptive_exploration_budget_exhausted"));
   cJSON *detail = cJSON_Parse(reason);
   assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(detail, "alternatives")));
   cJSON_Delete(detail);
   test_provider.kind = WS_PROVIDER_DETACHED;
   owner_reply = "{\"allowed\":true,\"reason\":\"contract_invalidated\"}";
   assert(policy_bind_session_exploration("session") == -1);
   assert(policy_check_session_tool("session", "bash", "{\"command\":\"rg foo\"}", "hook-detached",
                                    reason, sizeof(reason)) == 0);
   assert(check_session_calls == 3);
   policy_set_unknown_effect_gate(source_gate_probe);
   g_reply = "{\"allowed\":true,\"reason\":\"baseline allows\"}";
   assert(!policy_check_tool_attempt("write_file", "filesystem", "{\"path\":\"/workspace/target\"}",
                                     "host-attempt", reason, sizeof reason));
   assert(gate_calls == 1 && gate_stage == 0 && gate_clears > 0);
   assert(!policy_recheck_action_tool_attempt("write_file", "filesystem",
                                              "{\"path\":\"/workspace/target\"}", "host-attempt", 0,
                                              reason, sizeof reason));
   assert(gate_calls == 2 && gate_stage == 1);
   g_reply = "{\"allowed\":false,\"reason\":\"baseline refuses\"}";
   assert(policy_recheck_action_tool_attempt("write_file", "filesystem",
                                             "{\"path\":\"/workspace/target\"}", "host-attempt", 0,
                                             reason, sizeof reason) < 0);
   assert(gate_calls == 2);
   policy_set_unknown_effect_gate(NULL);
   puts("test_execution_policy_bus: OK");
   return 0;
}
