#include "server_native_delegate.h"
#include "server_native_history.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "server_delegate_input.h"
#include "request_context.h"
#include <aimee/audit/obs_bus.h>
#include <aimee/ir/session_event.h>
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static aimee_request_t bound_request;
static int ran, allow = 1, terminal = 1, loaded, seen_tools;
static request_context_t actor = {.vault_principal = "previous-owner"};
static const agent_request_cancel_scope_t *cancel_scope;
static agent_request_creds_t credentials;
static char directory[MAX_PATH_LEN];
static agent_t registration;
int agent_load_config(agent_config_t *config)
{
   (void)config;
   memset(&registration, 0, sizeof registration);
   strcpy(registration.name, "native-model");
   registration.enabled = 1;
   registration.max_turns = 3;
   return 0;
}
agent_t *agent_find(agent_config_t *config, const char *name)
{
   (void)config;
   assert(!strcmp(name, "native-model"));
   return allow ? &registration : NULL;
}
int agent_has_role(const agent_t *agent, const char *role)
{
   return agent == &registration && !strcmp(role, "review");
}
void request_context_snapshot(request_context_snapshot_t *out)
{
   *out = (request_context_snapshot_t){.active = 1, .value = actor};
}
void request_context_restore(const request_context_snapshot_t *saved)
{
   actor = saved->value;
}
void request_context_set(const request_context_t *context)
{
   actor = *context;
}
void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   *out = credentials;
}
void agent_request_creds_restore(const agent_request_creds_t *value)
{
   credentials = *value;
}
const char *run_cmd_get_cwd(void)
{
   return directory;
}
void run_cmd_set_cwd(const char *path)
{
   snprintf(directory, sizeof directory, "%s", path ? path : "");
}
int db1_server_session_create(const char *session, const char *kind, const char *principal)
{
   assert(!strncmp(session, "delegate-", 9) && !strcmp(kind, "delegate") &&
          !strcmp(principal, "server"));
   return 0;
}
static int persona_known = 1;
int persona_exists(const char *workdir, const char *persona)
{
   assert(!strcmp(workdir, "/owned/task") && !strcmp(persona, "architect"));
   return persona_known;
}
int delegate_assemble_policy_ir(int has_source_policy, const char *role, const char *cwd,
                                const char *persona, const char *worktree, aimee_block_t *out)
{
   assert(has_source_policy == server_delegate_input_has_policy(&bound_request));
   assert(!strcmp(role, "review") && !strcmp(cwd, "/owned/task") && !strcmp(worktree, cwd));
   assert(persona && !strcmp(persona, "architect"));
   *out =
       (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("host-composed role/persona policy")};
   aimee_ir_block_set_context(out, AIMEE_CTX_ORIGIN_OPERATOR, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   return out->text ? 0 : -1;
}
static void assert_effective_source(const aimee_request_t *request)
{
   assert(request->n_system == bound_request.n_system + 1 &&
          !strcmp(request->system[0].text, "host-composed role/persona policy"));
   assert(request->system[0].context.origin == AIMEE_CTX_ORIGIN_OPERATOR &&
          request->system[0].context.authority == AIMEE_CTX_AUTH_POLICY &&
          request->system[0].context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
   aimee_request_t original = *request;
   original.system++;
   original.n_system--;
   original.mutated = bound_request.mutated;
   assert(aimee_ir_request_equal(&original, &bound_request));
}
const request_context_t *request_context_get(void)
{
   return &actor;
}
const agent_request_cancel_scope_t *agent_request_cancel_scope_current(void)
{
   return cancel_scope;
}
const agent_request_cancel_scope_t *
agent_request_cancel_scope_bind(const agent_request_cancel_scope_t *s)
{
   const agent_request_cancel_scope_t *old = cancel_scope;
   cancel_scope = s;
   return old;
}
int agent_request_with_tools_supported(const aimee_request_t *r)
{
   return !r->n_tools && !r->metadata;
}
int agent_run_native_request_for_role(agent_config_t *config, const agent_t *selected,
                                      const char *role, int tools, const aimee_request_t *request,
                                      aimee_response_t *response, agent_result_t *result)
{
   assert(selected->max_turns == 3 && server_native_delegate_active());
   assert(!strcmp(actor.principal, "server") && !strcmp(actor.vault_principal, "server") &&
          actor.vault_transport == ATTEST_NONE && !actor.trusted);
   assert(!strcmp(directory, "/owned/task"));
   assert_effective_source(request);
   ran++;
   assert(!strcmp(role, "review") && tools == seen_tools && cancel_scope);
   assert(config->tool_loop_timeout_ms_cap > 0 && config->tool_loop_timeout_ms_cap <= 60000);
   strcpy(result->canonical_session_id, "actual-child");
   assert(!aimee_ir_response_from_text(response, "actual result", NULL));
   response->usage_in = 7;
   response->usage_out = 2;
   response->stop_reason = terminal == 1 || terminal == 3 ? AIMEE_STOP_END_TURN
                           : terminal == 2                ? AIMEE_STOP_ERROR
                                                          : AIMEE_STOP_CONTENT_FILTER;
   if (terminal == 3)
   {
      free(response->content[0].text);
      response->content[0] = (aimee_block_t){.type = AIMEE_BLK_IMAGE,
                                             .media_type = strdup("image/png"),
                                             .media_ref = strdup("data:image/png;base64,eA==")};
   }
   if (terminal == 2)
   {
      response->error_message = strdup("native model generation turn cap reached");
      strcpy(result->stop_reason, "turn_cap");
      result->turns = 4;
   }
   if (!terminal)
   {
      response->refusal = calloc(1, sizeof *response->refusal);
      response->refusal->category = strdup("unknown-provider-code");
   }
   result->success = terminal == 1 || terminal == 3;
   result->response = strdup("POISONED DISPLAY MUST NOT BECOME HISTORY");
   return terminal == 1 || terminal == 3 ? 0 : -1;
}
int server_native_history_load(const char *session, const char *principal, const char *turn,
                               int running, cJSON **history, cJSON **receipts)
{
   loaded++;
   assert(!strcmp(session, "actual-child") && !strcmp(principal, "server") && !turn && !running);
   *history = cJSON_Parse("[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"actual "
                          "request\"}]},{\"role\":\"assistant\",\"blocks\":[{\"type\":\"text\","
                          "\"text\":\"actual result\"}]}]");
   if (terminal == 3)
   {
      cJSON_Delete(*history);
      *history = cJSON_Parse(
          "[{\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"actual "
          "request\"}]},{\"role\":\"assistant\",\"blocks\":[{\"type\":\"image\",\"media_type\":"
          "\"image/png\",\"media_ref\":\"data:image/png;base64,eA==\"}]}]");
   }
   *receipts = cJSON_CreateArray();
   return 0;
}
static cJSON *request(void)
{
   return cJSON_Parse(
       "{\"version\":3,\"operation\":\"work-item-1\",\"role\":\"review\","
       "\"persona\":\"architect\","
       "\"model\":\"native-model\",\"workdir\":\"/owned/"
       "task\",\"tools\":false,\"max_turns\":3,\"execution_timeout_ms\":60000,\"ir\":{\"version\":"
       "1,\"source_wire\":\"unspecified\",\"cache_policy\":\"native\",\"system\":[],\"messages\":[{"
       "\"role\":\"user\",\"blocks\":[{\"type\":\"text\",\"text\":\"actual "
       "request\"}]}],\"tools\":[],\"tool_choice\":{\"mode\":\"unset\"},\"stream\":false,\"stop_"
       "sequences\":[]}}");
}
static aimee_module_invocation_t invocation(void)
{
   struct timespec now;
   assert(!clock_gettime(CLOCK_MONOTONIC, &now));
   return (aimee_module_invocation_t){.stage_id = 23,
                                      .deadline_ns = (uint64_t)now.tv_sec * 1000000000u +
                                                     now.tv_nsec + 120000000000u,
                                      .source_principal_ref = 76,
                                      .source_handle = 123};
}
static cJSON *call(cJSON *request, aimee_module_invocation_t *inv, aimee_module_status_t expected)
{
   aimee_request_free(&bound_request);
   assert(!aimee_ir_request_parse(cJSON_GetObjectItemCaseSensitive(request, "ir"), &bound_request));
   seen_tools = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "tools"));
   char *body = cJSON_PrintUnformatted(request);
   uint8_t out[65536];
   uint32_t count = 0;
   assert(server_native_delegate_handle(inv, (uint8_t *)body, (uint32_t)strlen(body), out,
                                        sizeof out, &count, NULL) == expected);
   cJSON_free(body);
   cJSON_Delete(request);
   if (expected != AIMEE_MODULE_STATUS_OK)
   {
      assert(!count);
      return NULL;
   }
   assert(count);
   return aimee_ir_json_parse_frame((char *)out, count);
}
static void test_success_and_decline(void)
{
   aimee_module_invocation_t inv = invocation();
   cJSON *reply = call(request(), &inv, AIMEE_MODULE_STATUS_OK);
   assert(reply &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "status")), "done"));
   assert(ran == 1 && loaded == 1 && !seen_tools && !cancel_scope &&
          !server_native_delegate_active());
   assert(!strcmp(actor.vault_principal, "previous-owner") && !directory[0] &&
          !credentials.session_id[0]);
   char *wire = cJSON_PrintUnformatted(reply);
   assert(!strstr(wire, "POISONED DISPLAY") && strstr(wire, "actual result"));
   cJSON_free(wire);
   aimee_response_t canonical = {0};
   assert(!aimee_ir_response_parse(cJSON_GetObjectItemCaseSensitive(reply, "ir"), &canonical));
   assert(canonical.usage_in == 7);
   aimee_response_free(&canonical);
   cJSON_Delete(reply);
   terminal = 0;
   reply = call(request(), &inv, AIMEE_MODULE_STATUS_OK);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "status")), "failed"));
   assert(!aimee_ir_response_parse(cJSON_GetObjectItemCaseSensitive(reply, "ir"), &canonical));
   assert(canonical.stop_reason == AIMEE_STOP_CONTENT_FILTER && canonical.refusal &&
          !strcmp(canonical.refusal->category, "unknown-provider-code"));
   aimee_response_free(&canonical);
   cJSON_Delete(reply);
   terminal = 2;
   reply = call(request(), &inv, AIMEE_MODULE_STATUS_OK);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "status")), "failed"));
   const cJSON *termination = cJSON_GetObjectItemCaseSensitive(reply, "termination");
   assert(termination &&
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(termination, "kind")),
                  "turn_cap") &&
          cJSON_GetObjectItemCaseSensitive(termination, "max_turns")->valueint == 3 &&
          cJSON_GetObjectItemCaseSensitive(termination, "observed_turns")->valueint == 4);
   cJSON_Delete(reply);
   terminal = 3;
   reply = call(request(), &inv, AIMEE_MODULE_STATUS_OK);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(reply, "status")), "done"));
   assert(!aimee_ir_response_parse(cJSON_GetObjectItemCaseSensitive(reply, "ir"), &canonical));
   assert(canonical.n_content == 1 && canonical.content[0].type == AIMEE_BLK_IMAGE);
   aimee_response_free(&canonical);
   cJSON_Delete(reply);
   terminal = 1;
}
static void test_fences(void)
{
   const int requested_caps[] = {100, 0};
   for (size_t i = 0; i < sizeof requested_caps / sizeof *requested_caps; i++)
   {
      aimee_module_invocation_t capped_inv = invocation();
      cJSON *capped_request = request();
      cJSON_ReplaceItemInObjectCaseSensitive(capped_request, "max_turns",
                                             cJSON_CreateNumber(requested_caps[i]));
      cJSON *capped_reply = call(capped_request, &capped_inv, AIMEE_MODULE_STATUS_OK);

      cJSON_Delete(capped_reply);
   }
   int calls = ran;
   aimee_module_invocation_t inv = invocation();
   inv.source_principal_ref = 10;
   call(request(), &inv, AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(ran == calls);
   inv = invocation();
   cJSON *j = request();
   cJSON_AddStringToObject(j, "actor", "forged");
   call(j, &inv, AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(ran == calls);
   j = request();
   cJSON_AddItemToObject(cJSON_GetObjectItemCaseSensitive(j, "ir"), "metadata",
                         cJSON_CreateObject());
   call(j, &inv, AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(ran == calls);
   j = request();
   cJSON_ReplaceItemInObjectCaseSensitive(j, "execution_timeout_ms", cJSON_CreateNumber(0));
   call(j, &inv, AIMEE_MODULE_STATUS_INVALID_REQUEST);
   assert(ran == calls);
   persona_known = 0;
   call(request(), &inv, AIMEE_MODULE_STATUS_INVALID_REQUEST);
   persona_known = 1;
   assert(ran == calls && !cancel_scope);
   allow = 0;
   call(request(), &inv, AIMEE_MODULE_STATUS_INVALID_REQUEST);
   allow = 1;
   assert(ran == calls && !cancel_scope);
}
static void test_original_system_blocks_preserved(void)
{
   cJSON *j = request();
   const char *policy_text = "operator original policy 漢字";
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = (char *)policy_text},
                             {.type = AIMEE_BLK_TEXT, .text = "hidden memory evidence"}};
   aimee_ir_block_set_context(&blocks[0], AIMEE_CTX_ORIGIN_OPERATOR, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1,
                              "original-policy", "unit", 17);
   aimee_ir_block_set_context(&blocks[1], AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNTRUSTED, AIMEE_CTX_SENS_CONFIDENTIAL, 0,
                              "original-memory", "unit", 18);
   aimee_request_t source = {0};
   assert(!aimee_ir_request_parse(cJSON_GetObjectItemCaseSensitive(j, "ir"), &source));
   source.system = calloc(2, sizeof *source.system);
   assert(source.system);
   source.n_system = 2;
   for (int i = 0; i < 2; i++)
   {
      source.system[i] = blocks[i];
      source.system[i].text = strdup(blocks[i].text);
      assert(source.system[i].text);
   }
   source.has_temperature = source.has_top_p = 1;
   source.temperature = 0.17;
   source.top_p = 0.8;
   cJSON *ir = aimee_ir_request_json(&source);
   assert(ir);
   cJSON_ReplaceItemInObjectCaseSensitive(j, "ir", ir);
   aimee_module_invocation_t inv = invocation();
   cJSON *reply = call(j, &inv, AIMEE_MODULE_STATUS_OK);
   assert(reply);
   cJSON_Delete(reply);
   aimee_request_free(&source);
}

int main(void)
{
   test_success_and_decline();
   test_fences();
   test_original_system_blocks_preserved();
   aimee_request_free(&bound_request);
   puts("native delegate bridge: PASS");
   return 0;
}
