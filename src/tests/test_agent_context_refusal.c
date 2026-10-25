#include "support/ir_text_request.h"
/* Exercise the production context builder and native initial-dispatch boundary.
 * External module and HTTP transports are fixtures. Memory decisions are supplied
 * as Go-owner protocol replies; this test does not reimplement memory policy. */
#include "aimee.h"
#include "agent_exec.h"
#include "util.h"
#include <aimee/tools/agent_tools.h>
#include "modules/tools/tool_catalog_data.h"
#include <aimee/module-runtime/pre_llm_hook.h>
#include <aimee/ir/session_event.h>
#include "config.h"
#include "cJSON.h"
#include "platform_test_util.h"
#include "support/delegate_role_seam_stub.h"
#include "http_retry.h"
#include "request_context.h"
#include "aimee_sha256.h"
#include "kb_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The canonical tool-free fixture installs its own log sink. Reaching a live
 * history/learning transport would violate the no-context/no-feedback contract. */
cJSON *aimee_module_command_call_context(uint32_t event, uint32_t stage, const char *verb,
                                         const cJSON *args, const cJSON *context)
{
   (void)event;
   (void)stage;
   (void)verb;
   (void)args;
   (void)context;
   abort();
}
int server_learning_project_from_cwd(const char *cwd, char *out, size_t capacity)
{
   (void)cwd;
   (void)out;
   (void)capacity;
   abort(); /* This fixture owns no learning source and must never request one. */
}
int server_native_history_load(const char *session, const char *actor, const char *turn,
                               int running, cJSON **messages, cJSON **receipts)
{
   (void)session;
   (void)actor;
   (void)turn;
   (void)running;
   (void)messages;
   (void)receipts;
   abort();
}

static int metadata_available = 1;
static int fixture_metadata(const char *name, aimee_tool_metadata_t *out)
{
   if (!metadata_available)
      return -1;
   *out = aimee_tools_catalog_metadata(name);
   return 0;
}

static int terminal_intents, terminal_denials;
static int capture_terminal_denial(const aimee_session_event_t *event, void *context)
{
   (void)context;
   if (event->kind != AIMEE_SESSION_MESSAGE)
      return 0;
   for (int i = 0; i < event->message->n_blocks; i++)
   {
      const aimee_block_t *block = &event->message->blocks[i];
      if (block->type == AIMEE_BLK_TOOL_USE)
      {
         assert(!strcmp(block->tool_id, "terminal-denied-call"));
         terminal_intents++;
      }
      if (block->type == AIMEE_BLK_TOOL_RESULT)
      {
         assert(!strcmp(block->tool_id, "terminal-denied-call") && block->tool_is_error);
         char *text = cJSON_PrintUnformatted(block->tool_result);
         assert(text && strstr(text, "not dispatched"));
         free(text);
         terminal_denials++;
      }
   }
   return 0;
}

static int wide_parallel, wide_uses, wide_results;
static int identity_case, identity_results;
static char identity_path[4096];
static int capture_identity_result(const aimee_session_event_t *event, void *context)
{
   (void)context;
   if (event->kind != AIMEE_SESSION_MESSAGE)
      return 0;
   for (int i = 0; i < event->message->n_blocks; i++)
   {
      const aimee_block_t *block = &event->message->blocks[i];
      if (block->type != AIMEE_BLK_TOOL_RESULT)
         continue;
      assert(!strcmp(block->tool_id, "identity-call"));
      char *result = cJSON_PrintUnformatted(block->tool_result);
      assert(result);
      /* This fixture intentionally has no execution-policy owner. The exact
       * advertised control reaches that independent gate; foreign/unknown calls
       * must be refused earlier, before policy or tool execution. */
      if (identity_case == 2)
         assert(block->tool_is_error && strstr(result, "blocked by policy") &&
                strstr(result, metadata_available ? "CAPABILITY_ABSENT" : "metadata unavailable"));
      else
         assert(block->tool_is_error && strstr(result, "not advertised") &&
                !strstr(result, "IDENTITY_READ_CONTROL"));
      free(result);
      identity_results++;
   }
   return 0;
}
static int wide_capture(const aimee_session_event_t *event, void *context)
{
   if (event->kind != AIMEE_SESSION_MESSAGE)
      return 0;
   cJSON *pending = context;
   for (int i = 0; i < event->message->n_blocks; i++)
   {
      const aimee_block_t *block = &event->message->blocks[i];
      if (block->type == AIMEE_BLK_TOOL_USE)
      {
         assert(strlen(block->tool_id) > 64 &&
                !cJSON_GetObjectItemCaseSensitive(pending, block->tool_id));
         assert(cJSON_AddTrueToObject(pending, block->tool_id));
         wide_uses++;
      }
      else if (block->type == AIMEE_BLK_TOOL_RESULT)
      {
         assert(cJSON_GetObjectItemCaseSensitive(pending, block->tool_id));
         cJSON_DeleteItemFromObjectCaseSensitive(pending, block->tool_id);
         wide_results++;
      }
   }
   return 0;
}

static int capture_failure_mode, capture_events;
static int refusing_capture(const aimee_session_event_t *event, void *context)
{
   (void)context;
   capture_events++;
   if (capture_failure_mode == 1)
      return -1;
   return event->kind == AIMEE_SESSION_MESSAGE && !strcmp(event->message->role, "tool") ? -1 : 0;
}

static const char *recall_reply;
static int recalls;
static int refuse_after;
static int provider_calls;
static int final_response;
static int usage_mixed;
static const char *simple_finish;
static int simple_tools, simple_media;
static int bootstrap_case, bootstrap_evidence_seen;
static int malformed_projection;
static int transport_refuse;
static int expect_hook_prompt, hook_calls;
static int owned_generation_case, owned_generation_inputs;
static int fallback_hook_context(const char *user, char *out, size_t capacity, void *context)
{
   (void)context;
   assert(!strcmp(user, "original prompt"));
   hook_calls++;
   snprintf(out, capacity, "single-hook-context");
   return 0;
}

int http_retry_post_context_bytes(const char *url, const char *auth_header, const void *body,
                                  size_t body_len, char **response_buf, int timeout_ms,
                                  const char *extra_headers, int max_attempts, int base_ms,
                                  int max_ms, const char *provider, const char *model,
                                  const char *session_id)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)body_len;
   (void)timeout_ms;
   (void)extra_headers;
   (void)max_attempts;
   (void)base_ms;
   (void)max_ms;
   (void)provider;
   (void)model;
   (void)session_id;
   provider_calls++;
   if (owned_generation_case)
   {
      cJSON *sent = cJSON_ParseWithLengthOpts(body, body_len, NULL, 0);
      assert(sent);
      assert(cJSON_GetObjectItem(sent, "max_tokens")->valueint == 256);
      assert(cJSON_GetObjectItem(sent, "temperature")->valuedouble == 0.3);
      assert(!cJSON_GetObjectItem(sent, "tools"));
      char *encoded = cJSON_PrintUnformatted(sent);
      assert(encoded && strstr(encoded, "original prompt") &&
             !strstr(encoded, "single-hook-context"));
      free(encoded);
      cJSON_Delete(sent);
   }
   if (simple_finish)
   {
      char response[512];
      snprintf(response, sizeof response,
               "{\"model\":\"terminal-fixture\",\"choices\":[{\"finish_reason\":\"%s\","
               "\"message\":{\"role\":\"assistant\",\"content\":\"Partial plausible verdict.\"}}],"
               "\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}",
               simple_finish);
      cJSON *root = cJSON_Parse(response);
      assert(root);
      if (simple_media)
      {
         cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
         cJSON *message = cJSON_GetObjectItem(choice, "message");
         cJSON *content = cJSON_Parse(
             "[{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,YQ==\"},"
             "\"fixture_sidecar\":{\"opaque\":\"preserve\"}}]");
         assert(content);
         if (simple_media == 2)
         {
            cJSON *text = cJSON_CreateObject();
            cJSON_AddStringToObject(text, "type", "text");
            cJSON_AddStringToObject(text, "text", "Completed visual answer.");
            cJSON_AddItemToArray(content, text);
         }
         cJSON_ReplaceItemInObject(message, "content", content);
      }
      if (simple_tools)
      {
         cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
         cJSON *message = cJSON_GetObjectItem(choice, "message");
         cJSON *calls = cJSON_AddArrayToObject(message, "tool_calls");
         cJSON *call = cJSON_CreateObject();
         cJSON_AddItemToArray(calls, call);
         cJSON_AddStringToObject(call, "id", "terminal-denied-call");
         cJSON_AddStringToObject(call, "type", "function");
         cJSON *function = cJSON_AddObjectToObject(call, "function");
         cJSON_AddStringToObject(function, "name", "read_file");
         cJSON_AddStringToObject(function, "arguments", "{\"path\":\"must-not-run\"}");
      }
      *response_buf = cJSON_PrintUnformatted(root);
      cJSON_Delete(root);
      return 200;
   }
   if (bootstrap_case)
   {
      char *request = strndup(body, body_len);
      assert(request);
      if (strstr(request, "[AIMEE REPOSITORY EVIDENCE FALLBACK]"))
         bootstrap_evidence_seen = 1;
      free(request);
      *response_buf = strdup("{\"id\":\"bootstrap-response\",\"object\":\"response\","
                             "\"status\":\"completed\",\"output\":[{\"type\":\"message\","
                             "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                             "\"text\":\"Fixture final without repository evidence.\"}]}]}");
      assert(*response_buf);
      return 200;
   }
   if (identity_case)
   {
      cJSON *reply = cJSON_Parse("{\"id\":\"identity-response\",\"object\":\"response\","
                                 "\"status\":\"completed\",\"output\":[]}");
      assert(reply);
      cJSON *output = cJSON_GetObjectItemCaseSensitive(reply, "output");
      if (provider_calls == 1)
      {
         cJSON *call = cJSON_CreateObject(), *args = cJSON_CreateObject();
         assert(call && args);
         assert(cJSON_AddStringToObject(args, "path", identity_path));
         char *arguments = cJSON_PrintUnformatted(args);
         assert(arguments);
         assert(cJSON_AddStringToObject(call, "type", "function_call"));
         assert(cJSON_AddStringToObject(call, "call_id", "identity-call"));
         assert(cJSON_AddStringToObject(call, "name",
                                        identity_case == 3 ? "unadvertised" : "read_file"));
         if (identity_case == 1)
            assert(cJSON_AddStringToObject(call, "namespace", "foreign"));
         assert(cJSON_AddStringToObject(call, "arguments", arguments));
         assert(cJSON_AddItemToArray(output, call));
         free(arguments);
         cJSON_Delete(args);
      }
      else
      {
         assert(provider_calls == 2);
         cJSON *message =
             cJSON_Parse("{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
                         "\"type\":\"output_text\",\"text\":\"Identity fixture completed.\"}]}");
         assert(message && cJSON_AddItemToArray(output, message));
      }
      if (usage_mixed == 3 || (usage_mixed == 1 && provider_calls == 2) ||
          (usage_mixed == 2 && provider_calls == 1))
      {
         cJSON *usage = cJSON_AddObjectToObject(reply, "usage");
         assert(usage);
         assert(cJSON_AddNumberToObject(usage, "input_tokens", usage_mixed == 3 ? 0 : 7));
         assert(cJSON_AddNumberToObject(usage, "output_tokens", usage_mixed == 3 ? 0 : 3));
      }
      *response_buf = cJSON_PrintUnformatted(reply);
      cJSON_Delete(reply);
      assert(*response_buf);
      return 200;
   }
   if (wide_parallel && provider_calls == 1)
   {
      cJSON *reply = cJSON_Parse("{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{"
                                 "\"role\":\"assistant\",\"tool_calls\":[]}}]}");
      assert(reply);
      cJSON *message = cJSON_GetObjectItem(
          cJSON_GetArrayItem(cJSON_GetObjectItem(reply, "choices"), 0), "message");
      for (int i = 0; i < 17; i++)
      {
         cJSON *call = cJSON_CreateObject();
         char id[280], arguments[96];
         memset(id, 'x', 256);
         snprintf(id + 256, sizeof id - 256, "-%d", i);
         snprintf(arguments, sizeof arguments, "{\"path\":\"absent-wide-fixture-%d\"}", i);
         cJSON_AddStringToObject(call, "id", id);
         cJSON_AddStringToObject(call, "type", "function");
         cJSON *function = cJSON_AddObjectToObject(call, "function");
         cJSON_AddStringToObject(function, "name", "read_file");
         cJSON_AddStringToObject(function, "arguments", arguments);
         cJSON_AddItemToArray(cJSON_GetObjectItem(message, "tool_calls"), call);
      }
      *response_buf = cJSON_PrintUnformatted(reply);
      cJSON_Delete(reply);
      assert(*response_buf);
      return 200;
   }
   if (final_response)
   {
      *response_buf = strdup("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{"
                             "\"role\":\"assistant\",\"content\":\"Fixture completed.\"}}]}");
      return 200;
   }
   assert(provider_calls <= 5); /* a sixth dispatch would use stale context */
   char reply[1024];
   snprintf(reply, sizeof(reply),
            "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\","
            "\"tool_calls\":[{\"id\":\"call-%d\",\"type\":\"function\",\"function\":{\"name\":"
            "\"read_file\","
            "\"arguments\":\"{\\\"path\\\":\\\"absent-fixture-%d\\\"}\"}}]}}]}",
            provider_calls, provider_calls);
   *response_buf = strdup(reply);
   return 200;
}

/* This fixture isolates agent refusal propagation. The native ingress fixture
 * and deployment test exercise the real Go plan and durable WORM adapter. */
char *ingress_preinject_task_instructions(const char *instructions, const char *query)
{
   (void)instructions;
   (void)query;
   return NULL;
}
void policy_action_attempt(const char *attempt)
{
   (void)attempt;
}

int ingress_preinject_accept_native_projection(const cJSON *projection)
{
   assert(cJSON_IsObject(projection));
   return 0;
}

int ingress_preinject_prepare_attempt(const void *body, size_t length, const char *route,
                                      const char *provider, const char *model, char attempt[33])
{
   const request_context_t *context = request_context_get();
   assert(context && (context->memory_source_release[0] || context->memory_receipt_required));
   assert(body && length && route && provider && model);
   attempt[0] = '\0';
   return 0;
}

int http_retry_post_observed_bytes(const char *url, const char *auth_header, const void *body,
                                   size_t body_len, char **response_buf, int timeout_ms,
                                   const char *extra_headers, int max_attempts, int base_ms,
                                   int max_ms, const char *provider, const char *model,
                                   const char *session_id, http_retry_admit_cb_t admit_retry,
                                   const http_retry_observer_t *observer)
{
   assert(!admit_retry && observer && observer->before && observer->after);
   if (expect_hook_prompt)
      assert(body && body_len == strlen(body) && strstr(body, "single-hook-context"));
   assert(observer->before(observer->context, body, body_len) == 0);
   if (transport_refuse)
   {
      provider_calls++;
      if (transport_refuse == 2 && provider_calls == 1)
      {
         *response_buf = strdup("{}");
         observer->after(observer->context, 400, *response_buf, 2);
         return 400;
      }
      assert(request_context_refuse_assembly("stale_context") == 0);
      assert(observer->before(observer->context, body, body_len) != 0);
      *response_buf = NULL;
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   int status = http_retry_post_context_bytes(url, auth_header, body, body_len, response_buf,
                                              timeout_ms, extra_headers, max_attempts, base_ms,
                                              max_ms, provider, model, session_id);
   observer->after(observer->context, status, *response_buf,
                   *response_buf ? strlen(*response_buf) : 0);
   return status;
}

int http_retry_post_context(const char *url, const char *auth_header, const char *body,
                            char **response_buf, int timeout_ms, const char *extra_headers,
                            int max_attempts, int base_ms, int max_ms, const char *provider,
                            const char *model, const char *session_id)
{
   return http_retry_post_context_bytes(url, auth_header, body, body ? strlen(body) : 0,
                                        response_buf, timeout_ms, extra_headers, max_attempts,
                                        base_ms, max_ms, provider, model, session_id);
}

int http_retry_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers, int max_attempts, int base_ms,
                    int max_ms)
{
   return http_retry_post_context(url, auth_header, body, response_buf, timeout_ms, extra_headers,
                                  max_attempts, base_ms, max_ms, NULL, NULL, NULL);
}

int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result)
{
   (void)timeout_ms;
   *result = NULL;
   const char *operation =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "operation"));
   if (strcmp(method, "memory.runtime") == 0 && operation &&
       strcmp(operation, "personal-recall") == 0)
   {
      recalls++;
      *result = cJSON_CreateObject();
      cJSON_AddStringToObject(*result, "status", "ok");
      const char *raw = refuse_after && recalls < refuse_after ? "{\"status\":\"ok\",\"recall\":{}}"
                                                               : recall_reply;
      cJSON *envelope = cJSON_Parse(raw);
      const char *status =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(envelope, "status"));
      if (status && !strcmp(status, "ok"))
      {
         const cJSON *limit = cJSON_GetObjectItemCaseSensitive(args, "native_context_bytes");
         assert(cJSON_IsNumber(limit) && limit->valuedouble > 0);
         const char *text = "# Recall\nGo owner retained complete context.\n";
         char digest[72] = "sha256:";
         assert(aimee_sha256_hex(text, strlen(text), digest + 7) == 0);
         cJSON *projection = cJSON_AddObjectToObject(envelope, "native_context");
         cJSON_AddNumberToObject(projection, "schema_version", 1);
         cJSON_AddStringToObject(projection, "text", text);
         cJSON_AddNumberToObject(projection, "rendered_bytes", (double)strlen(text));
         cJSON_AddItemToObject(projection, "max_context_bytes", cJSON_Duplicate(limit, 1));
         cJSON_AddStringToObject(projection, "digest", digest);
         cJSON *ids = cJSON_AddArrayToObject(projection, "retained_reminder_ids");
         cJSON_AddItemToArray(ids, cJSON_CreateString("9223372036854775807"));
         switch (malformed_projection)
         {
         case 1:
            cJSON_ReplaceItemInObjectCaseSensitive(projection, "digest",
                                                   cJSON_CreateString("sha256:bad"));
            break;
         case 2:
            cJSON_ReplaceItemInObjectCaseSensitive(projection, "rendered_bytes",
                                                   cJSON_CreateNumber(1));
            break;
         case 3:
            cJSON_ReplaceItemInObjectCaseSensitive(projection, "max_context_bytes",
                                                   cJSON_CreateNumber(0));
            break;
         case 4:
            cJSON_ReplaceItemInObjectCaseSensitive(projection, "schema_version",
                                                   cJSON_CreateNumber(2));
            break;
         case 5:
            cJSON_AddItemToArray(ids, cJSON_CreateNumber(1));
            break;
         case 6:
            cJSON_AddItemToArray(ids, cJSON_CreateString("9223372036854775808"));
            break;
         case 7:
            cJSON_AddItemToArray(ids, cJSON_CreateString("0"));
            break;
         case 8:
            cJSON_AddItemToArray(ids, cJSON_CreateString("01"));
            break;
         }
      }
      char *body = cJSON_PrintUnformatted(envelope);
      cJSON_AddStringToObject(*result, "json", body);
      free(body);
      cJSON_Delete(envelope);
      return 1;
   }
   return 0;
}

int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   return aimee_module_commands_dispatch_internal_timeout(method, args, 0, result);
}

int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   return aimee_module_commands_dispatch_internal(method, args, result);
}

static int fixture_classifier(const char *name, int *classification)
{
   *classification = (int)aimee_tools_catalog_metadata(name).classification;
   return 0;
}

static int capture_owned_generation(const aimee_session_event_t *event, void *context)
{
   (void)context;
   if (event->kind == AIMEE_SESSION_MESSAGE && !strcmp(event->message->role, "user"))
   {
      assert(event->message->n_blocks == 1);
      const aimee_block_t *block = &event->message->blocks[0];
      assert(block->context.origin == AIMEE_CTX_ORIGIN_RETRIEVAL);
      assert(block->context.authority == AIMEE_CTX_AUTH_EVIDENCE);
      assert(block->context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
      assert(!strcmp(block->text, "original prompt"));
      owned_generation_inputs++;
   }
   return 0;
}

static void test_owned_plain_generation(const agent_t *agent)
{
   agent_config_t *cfg = calloc(1, sizeof *cfg);
   assert(cfg);
   cfg->agent_count = 1;
   cfg->agents[0] = *agent;
   cfg->agents[0].enabled = 1;
   cfg->agents[0].max_parallel = 1;
   snprintf(cfg->default_agent, sizeof cfg->default_agent, "%s", agent->name);
   aimee_block_t policy = {.type = AIMEE_BLK_TEXT,
                           .text = "Use only the evidence.",
                           .context = {.origin = AIMEE_CTX_ORIGIN_USER,
                                       .authority = AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                                       .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                       .model_visible = 1}};
   aimee_block_t evidence = {.type = AIMEE_BLK_TEXT,
                             .text = "original prompt",
                             .context = {.origin = AIMEE_CTX_ORIGIN_RETRIEVAL,
                                         .authority = AIMEE_CTX_AUTH_EVIDENCE,
                                         .trust = AIMEE_CTX_TRUST_UNVERIFIED,
                                         .model_visible = 1}};
   aimee_message_t message = {.role = "user", .blocks = &evidence, .n_blocks = 1};
   aimee_request_t input = {.system = &policy,
                            .n_system = 1,
                            .messages = &message,
                            .n_messages = 1,
                            .tool_choice = AIMEE_TOOL_CHOICE_NONE,
                            .max_tokens = 256,
                            .has_max_tokens = 1,
                            .temperature = 0.3,
                            .has_temperature = 1};
   agent_result_t result = {0};
   owned_generation_case = 1;
   owned_generation_inputs = provider_calls = recalls = hook_calls = 0;
   assert(plugin_chook_register_pre_llm(fallback_hook_context, NULL) >= 0);
   aimee_ir_session_set_sink(capture_owned_generation, NULL);
   int rc = agent_generate_request(cfg, &input, &result);
   aimee_ir_session_set_sink(NULL, NULL);
   plugin_chook_reset();
   owned_generation_case = 0;
   assert(rc == 0 && result.success &&
          !aimee_ir_response_complete_text_valid(&result.canonical_response));
   assert(provider_calls == 1 && recalls == 0 && hook_calls == 0 && owned_generation_inputs == 1);
   agent_result_clear_response(&result);
   free(cfg);
}

int main(void)
{
   delegate_role_seam_install();
   agent_tools_register_metadata(fixture_metadata);
   char home[4096];
   snprintf(home, sizeof(home), "%s/aimee-context-refusal-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home));
   assert(platform_setenv("AIMEE_HOME", home) == 0);
   assert(platform_unsetenv("AIMEE_CONFIG_PATH") == 0);
   assert(platform_setenv("AIMEE_CONTEXT_NO_KB", "1") == 0);
   assert(config_set_memory_recall_enabled(1) == 0);
   agent_t agent = {0};
   agent_network_t network = {0};
   snprintf(agent.name, sizeof(agent.name), "context-refusal-fixture");
   snprintf(agent.model, sizeof(agent.model), "fixture-model");
   snprintf(agent.provider, sizeof(agent.provider), "openai");
   /* Auth is deliberately unavailable. A refusal must occur before auth or
    * provider setup, without replacing the original owner diagnostic. */
   const char *replies[] = {
       "{\"status\":\"error\",\"kind\":\"protected_context_overflow\",\"message\":\"complete rules "
       "exceed allocation\"}",
       "{\"status\":\"quarantined\",\"recall\":{}}",
       "{\"status\":\"error\",\"kind\":\"unavailable\",\"message\":\"HTTP 503 unavailable\"}",
   };
   for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++)
   {
      recall_reply = replies[i];
      char error[512];
      char *text = agent_build_exec_context_checked(&agent, &network, "review", "original prompt",
                                                    0, error, sizeof(error));
      assert(text == NULL);
      assert(strstr(error, "memory context refused"));
      assert(agent_build_exec_context_for_role(&agent, &network, "review", "original prompt", 0) ==
             NULL);
      agent_result_t result = {0};
      int rc = fixture_agent_role_text(&agent, &network, "review", "original prompt", "task", 128,
                                       0, &result);
      assert(rc == AGENT_RC_CONTEXT_REFUSED);
      assert(!result.success && result.turns == 0 && result.tool_calls == 0);
      assert(strcmp(result.stop_reason, "context_refused") == 0);
      assert(strstr(result.error, "memory context refused"));
      assert(!agent_rc_should_try_another(rc, result.error));
      agent_result_clear_response(&result);
   }
   assert(recalls == 9 && provider_calls == 0);
   /* Reusing the same thread after a refusal must not poison a successful
    * assembly; the caller prompt must remain intact. */
   recall_reply = "{\"status\":\"ok\",\"recall\":{}}";
   char error[512] = "stale refusal";
   char *text = agent_build_exec_context_checked(&agent, &network, "review", "original prompt", 0,
                                                 error, sizeof(error));
   assert(text && !error[0]);
   assert(strstr(text, "original prompt"));
   assert(strstr(text, "Go owner retained complete context."));
   free(text);
   /* The host validates the complete projection before provider dispatch.
    * In particular, one valid ID must not hide a later malformed identity. */
   for (malformed_projection = 1; malformed_projection <= 8; malformed_projection++)
   {
      agent_result_t invalid_result = {0};
      int invalid_rc = fixture_agent_role_text(&agent, &network, "review", "original prompt",
                                               "task", 128, 0, &invalid_result);
      assert(invalid_rc == AGENT_RC_CONTEXT_REFUSED && provider_calls == 0);
      assert(strstr(invalid_result.error, "invalid_projection"));
      agent_result_clear_response(&invalid_result);
   }
   malformed_projection = 0;
   /* An unconfigured standalone server has an empty mode, not necessarily
    * the explicit "none" setting. It still requires its private Go owner. */
   assert(platform_unsetenv("AIMEE_CONTEXT_NO_KB") == 0);
   assert(platform_unsetenv("AIMEE_KB_API_URL") == 0);
   assert(config_set_kb_mode("") == 0);
   assert(!kb_client_connection_configured());
   text =
       agent_build_exec_context_checked(&agent, &network, "review", NULL, 0, error, sizeof(error));
   assert(text && !error[0] && strstr(text, "Go owner retained complete context."));
   free(text);
   /* A configured but unavailable KB is different: never silently downgrade
    * to private recall after losing the shared owner's required constraints. */
   assert(config_set_kb_mode("remote") == 0);
   int before_missing_connection = recalls;
   text =
       agent_build_exec_context_checked(&agent, &network, "review", NULL, 0, error, sizeof(error));
   assert(!text && strstr(error, "unavailable") && recalls == before_missing_connection);
   assert(config_set_kb_mode("") == 0);
   assert(platform_setenv("AIMEE_KB_API_URL", "http://127.0.0.1:1") == 0);
   assert(kb_client_connection_configured());
   int before_shared_refusal = recalls;
   text =
       agent_build_exec_context_checked(&agent, &network, "review", NULL, 0, error, sizeof(error));
   assert(!text && strstr(error, "unavailable") && recalls == before_shared_refusal);
   assert(platform_unsetenv("AIMEE_KB_API_URL") == 0);
   assert(platform_setenv("AIMEE_CONTEXT_NO_KB", "1") == 0);
   /* Initial assembly succeeds; a new owner refusal arrives at turn-five
    * refresh. The native loop must not send request six with stale context. */
   recalls = 0;
   refuse_after = 2;
   recall_reply = replies[0];
   snprintf(agent.auth_type, sizeof(agent.auth_type), "none");
   snprintf(agent.endpoint, sizeof(agent.endpoint), "http://context-refusal.invalid/v1");
   agent.max_turns = 8;
   agent.timeout_ms = 10000;
   agent_result_t result = {0};
   int rc =
       fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0, &result);
   if (rc != AGENT_RC_CONTEXT_REFUSED)
      fprintf(stderr, "unexpected loop result: rc=%d calls=%d recalls=%d error=%s\n", rc,
              provider_calls, recalls, result.error);
   assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success);
   assert(recalls == 2 && provider_calls == 5);
   assert(strcmp(result.stop_reason, "context_refused") == 0);
   assert(strstr(result.error, "protected_context_overflow"));
   assert(!agent_rc_should_try_another(rc, result.error));
   agent_result_clear_response(&result);
   recalls = provider_calls = refuse_after = 0;
   final_response = 1;
   recall_reply = "{\"status\":\"ok\",\"recall\":{}}";
   /* An HTTP worker can inherit an earlier ingress refusal. Even a successful
    * native recall must not clear it or dispatch through another provider. */
   request_context_t inherited = {0};
   request_context_set(&inherited);
   assert(request_context_refuse_assembly("protected_context_overflow") == 0);
   rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0, &result);
   assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success && provider_calls == 0);
   assert(strcmp(result.stop_reason, "context_refused") == 0);
   assert(strcmp(result.error, "protected_context_overflow") == 0);
   assert(!agent_rc_should_try_another(rc, result.error));
   agent_result_clear_response(&result);
   request_context_clear();
   recalls = 0;
   rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0, &result);
   assert(rc == 0 && result.success && !result.error[0]);
   assert(recalls == 1 && provider_calls == 1);
   assert(result.response && strstr(result.response, "Fixture completed."));
   agent_result_clear_response(&result);
   for (int fallback = 0; fallback <= 1; fallback++)
   {
      request_context_t retry_context = {0};
      request_context_set(&retry_context);
      recalls = provider_calls = 0;
      transport_refuse = fallback ? 2 : 1;
      snprintf(agent.fallback_model, sizeof(agent.fallback_model), "%s",
               fallback ? "fallback" : "");
      rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0,
                                   &result);
      assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success);
      assert(provider_calls == (fallback ? 2 : 1));
      assert(strcmp(result.stop_reason, "context_refused") == 0);
      assert(strcmp(result.error, "stale_context") == 0);
      assert(!agent_rc_should_try_another(rc, result.error));
      agent_result_clear_response(&result);
      /* The non-tool execution path must preserve the same refusal contract. */
      request_context_set(&retry_context);
      provider_calls = hook_calls = 0;
      expect_hook_prompt = 1;
      plugin_chook_reset();
      int hook_registered = plugin_chook_register_pre_llm(fallback_hook_context, NULL);
      assert(hook_registered == 0);
      rc = fixture_agent_execute_text(&agent, NULL, "original prompt", 128, 0, &result);
      assert(hook_calls == 1);
      plugin_chook_reset();
      expect_hook_prompt = 0;
      assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success);
      assert(provider_calls == (fallback ? 2 : 1));
      assert(strcmp(result.stop_reason, "context_refused") == 0);
      assert(strcmp(result.error, "stale_context") == 0);
      assert(!agent_rc_should_try_another(rc, result.error));
      agent_result_clear_response(&result);
      request_context_clear();
   }
   /* The real non-tool provider path must not grant success to partial text. */
   const char *incomplete_stops[] = {"content_filter", "length", "tool_calls", "unknown"};
   transport_refuse = 0;
   agent.fallback_model[0] = 0;
   for (size_t i = 0; i < sizeof incomplete_stops / sizeof *incomplete_stops; i++)
   {
      simple_finish = incomplete_stops[i];
      provider_calls = 0;
      rc = fixture_agent_execute_text(&agent, NULL, "original prompt", 128, 0, &result);
      assert(rc != 0 && !result.success && provider_calls == 1);
      assert(result.canonical_response.n_content == 1);
      assert(result.canonical_response.usage_in == 7);
      assert(result.canonical_response.usage_out == 3);
      assert(!agent_result_take_text(&result));
   }
   /* General HTTP completion retains multimedia; business projection refuses it. */
   simple_finish = "stop";
   for (simple_media = 1; simple_media <= 2; simple_media++)
   {
      simple_finish = "stop";
      provider_calls = 0;
      rc = fixture_agent_execute_text(&agent, NULL, "original prompt", 128, 0, &result);
      if (rc || !result.success || result.error[0] || provider_calls != 1)
         fprintf(stderr, "media fixture=%d rc=%d success=%d calls=%d error=%s blocks=%d stop=%d\n",
                 simple_media, rc, result.success, provider_calls, result.error,
                 result.canonical_response.n_content, result.canonical_response.stop_reason);
      assert(rc == 0 && result.success && !result.error[0] && provider_calls == 1);
      assert(!aimee_ir_response_complete_valid(&result.canonical_response));
      assert(result.canonical_response.content[0].type == AIMEE_BLK_IMAGE);
      assert(result.canonical_response.content[0].raw);
      assert(cJSON_GetObjectItem(result.canonical_response.content[0].raw, "fixture_sidecar"));
      assert(result.canonical_response.n_content == simple_media);
      assert(!agent_result_take_text(&result));
      const char *declined[] = {"length", "content_filter"};
      for (size_t i = 0; i < sizeof declined / sizeof *declined; i++)
      {
         simple_finish = declined[i];
         provider_calls = 0;
         rc = fixture_agent_execute_text(&agent, NULL, "original prompt", 128, 0, &result);
         assert(rc != 0 && !result.success && provider_calls == 1);
         assert(result.canonical_response.n_content == simple_media);
         assert(!agent_result_take_text(&result));
      }
   }
   simple_media = 0;
   /* Even actual native tool-use blocks accompanying a refused, truncated or
    * contradictory terminal state cannot reach tool execution or a retry. */
   const char *denied_tool_stops[] = {"content_filter", "length", "unknown", "stop"};
   simple_tools = 1;
   for (size_t i = 0; i < sizeof denied_tool_stops / sizeof *denied_tool_stops; i++)
   {
      simple_finish = denied_tool_stops[i];
      provider_calls = terminal_intents = terminal_denials = 0;
      aimee_ir_session_set_sink(capture_terminal_denial, NULL);
      rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0,
                                   &result);
      aimee_ir_session_set_sink(NULL, NULL);
      assert(terminal_intents == 1 && terminal_denials == 1);
      assert(rc != 0 && !result.success && provider_calls == 1 && result.tool_calls == 0);
      assert(aimee_ir_response_tool_count(&result.canonical_response) == 1);
      assert(result.canonical_response.usage_in == 7 && result.canonical_response.usage_out == 3);
      assert(!agent_result_take_text(&result));
   }
   simple_tools = 0;
   simple_finish = "stop";
   rc = fixture_agent_execute_text(&agent, NULL, "original prompt", 128, 0, &result);
   assert(rc == 0 && result.success);
   char *answer = agent_result_take_text(&result);
   assert(answer && !strcmp(answer, "Partial plausible verdict."));
   free(answer);
   test_owned_plain_generation(&agent);
   simple_finish = NULL;
   /* A durable capture loss is local and terminal, including after dispatch.
    * The attempted tool must still be counted when publishing its result fails. */
   for (int mode = 1; mode <= 2; mode++)
   {
      recalls = provider_calls = capture_events = 0;
      refuse_after = transport_refuse = 0;
      final_response = mode == 1;
      capture_failure_mode = mode;
      aimee_ir_session_set_sink(refusing_capture, NULL);
      rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0,
                                   &result);
      assert(rc == AGENT_RC_CAPTURE_FAILED && !result.success);
      assert(provider_calls == 1 && capture_events == (mode == 1 ? 1 : 4));
      assert(result.tool_calls == (mode == 1 ? 0 : 1));
      assert(!strcmp(result.stop_reason, "capture_failed"));
      assert(!agent_rc_should_try_another(rc, "quota exhausted"));
      assert(aimee_ir_session_capture_failed());
      agent_result_clear_response(&result);
      aimee_ir_session_set_sink(NULL, NULL);
      assert(!aimee_ir_session_capture_failed());
   }
   recalls = provider_calls = refuse_after = transport_refuse = 0;
   final_response = wide_parallel = 1;
   wide_uses = wide_results = 0;
   cJSON *pending = cJSON_CreateObject();
   assert(pending);
   aimee_ir_session_set_sink(wide_capture, pending);
   rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0, &result);
   assert(result.tool_calls == 17 && wide_uses == 17 && wide_results == 17);
   assert(pending->child == NULL && provider_calls <= 2);
   agent_result_clear_response(&result);
   aimee_ir_session_set_sink(NULL, NULL);
   cJSON_Delete(pending);
   wide_parallel = 0;
   snprintf(identity_path, sizeof identity_path, "%s/identity-read.txt", home);
   FILE *identity_file = fopen(identity_path, "w");
   assert(identity_file);
   assert(fputs("IDENTITY_READ_CONTROL", identity_file) >= 0);
   assert(fclose(identity_file) == 0);
   snprintf(agent.provider, sizeof agent.provider, "%s", "chatgpt");
   for (identity_case = 1; identity_case <= 3; identity_case++)
   {
      recalls = provider_calls = identity_results = 0;
      aimee_ir_session_set_sink(capture_identity_result, NULL);
      rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0,
                                   &result);
      assert(rc == 0 && result.success && provider_calls == 2 && identity_results == 1);
      assert(result.tool_calls == 1 && result.successful_tool_calls == 0);
      agent_result_clear_response(&result);
      aimee_ir_session_set_sink(NULL, NULL);
   }
   identity_case = 2;
   for (usage_mixed = 1; usage_mixed <= 3; usage_mixed++)
   {
      recalls = provider_calls = identity_results = 0;
      aimee_ir_session_set_sink(capture_identity_result, NULL);
      rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0,
                                   &result);
      assert(rc == 0 && result.success && provider_calls == 2 && identity_results == 1);
      assert(!aimee_ir_response_admit(&result.canonical_response));
      assert(agent_result_usage_is_unobserved(&result) == (usage_mixed != 3));
      assert(result.canonical_response.usage_unobserved == (usage_mixed != 3));
      assert(result.canonical_response.usage_in == 0 && result.canonical_response.usage_out == 0);
      assert(result.prompt_tokens == (usage_mixed == 3 ? 0 : 7));
      assert(result.completion_tokens == (usage_mixed == 3 ? 0 : 3));
      agent_result_clear_response(&result);
      aimee_ir_session_set_sink(NULL, NULL);
   }
   usage_mixed = 0;
   identity_case = 2;
   metadata_available = 0;
   recalls = provider_calls = identity_results = 0;
   aimee_ir_session_set_sink(capture_identity_result, NULL);
   rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0, &result);
   assert(rc == 0 && result.success && provider_calls == 2 && identity_results == 1);
   assert(result.tool_calls == 1 && result.successful_tool_calls == 0);
   agent_result_clear_response(&result);
   aimee_ir_session_set_sink(NULL, NULL);
   identity_case = 0;
   metadata_available = 1;
   bootstrap_case = 1;
   agent.require_initial_tool_call = 1;
   agent.max_turns = 1;
   provider_calls = recalls = bootstrap_evidence_seen = 0;
   agent_tools_register_classifier(fixture_classifier);
   run_cmd_set_cwd(home);
   rc = fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128, 0, &result);
   assert(result.successful_tool_calls == 0 && !bootstrap_evidence_seen);
   agent_result_clear_response(&result);
   run_cmd_set_cwd(NULL);
   agent_tools_register_classifier(NULL);
   agent_tools_register_metadata(NULL);
   identity_case = 0;
   puts("agent context refusal: passed");
   return 0;
}
