#include "support/ir_text_request.h"
/* Exercise the production context builder and native initial-dispatch boundary.
 * External module and HTTP transports are fixtures. Memory decisions are supplied
 * as Go-owner protocol replies; this test does not reimplement memory policy. */
#include "aimee.h"
#include "agent_exec.h"
#include "agent_admission.h"
#include "util.h"
#include <aimee/tools/agent_tools.h>
#include "modules/tools/tool_catalog_data.h"
#include <aimee/module-runtime/pre_llm_hook.h>
#include <aimee/ir/session_event.h>
#include <aimee/translation/aimee_backend.h>
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
static const char *projection_text_override;
static int recalls;
static int refuse_after;
static int provider_calls;
static int visibility_query_case;
static char last_ingress_query[4096];
static int final_response;
static int usage_mixed;
static const char *simple_finish;
static int simple_tools, simple_media, simple_trailing;
static int bootstrap_case, bootstrap_evidence_seen;
static int malformed_projection;
static int transport_refuse;
static int expect_hook_prompt, hook_calls;
static int owned_generation_case, owned_generation_inputs;
static int rich_generation_case;
static int responses_controls_case;
static int other_controls_case;
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
   if (other_controls_case)
   {
      cJSON *sent = cJSON_ParseWithLengthOpts(body, body_len, NULL, 0);
      assert(sent && !cJSON_HasObjectItem(sent, "reasoning") &&
             !cJSON_HasObjectItem(sent, "reasoning_summary") &&
             cJSON_HasObjectItem(sent, "metadata") && cJSON_HasObjectItem(sent, "service_tier"));
      assert(strstr(body, "other provider task") && strstr(body, "retained evidence"));
      assert(cJSON_GetNumberValue(cJSON_GetObjectItem(sent, "max_tokens")) == 128);
      assert(cJSON_GetNumberValue(cJSON_GetObjectItem(sent, "top_p")) == 0.8);
      cJSON_Delete(sent);
      *response_buf = strdup(
          other_controls_case == 2
              ? "{\"id\":\"other-controls\",\"type\":\"message\",\"role\":\"assistant\",\"stop_"
                "reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"Other controls "
                "complete.\"}]}"
              : "{\"id\":\"other-controls\",\"choices\":[{\"finish_reason\":\"stop\",\"message\":{"
                "\"role\":\"assistant\",\"content\":\"Other controls complete.\"}}]}");
      return 200;
   }
   if (responses_controls_case)
   {
      cJSON *sent = cJSON_ParseWithLengthOpts(body, body_len, NULL, 0);
      assert(sent && cJSON_HasObjectItem(sent, "service_tier") &&
             cJSON_IsFalse(cJSON_GetObjectItem(sent, "parallel_tool_calls")));
      assert(!cJSON_HasObjectItem(sent, "top_p") && !cJSON_HasObjectItem(sent, "top_k") &&
             !cJSON_HasObjectItem(sent, "metadata") && !cJSON_HasObjectItem(sent, "stop") &&
             !cJSON_HasObjectItem(sent, "max_output_tokens") &&
             !cJSON_HasObjectItem(sent, "temperature") && !cJSON_HasObjectItem(sent, "thinking") &&
             !cJSON_HasObjectItem(sent, "prompt_cache_retention") &&
             !cJSON_HasObjectItem(sent, "prompt_cache_options"));
      assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(sent, "prompt_cache_key")),
                     "owned-cache-scope"));
      assert(strstr(body, "immutable control task") && strstr(body, "separate control evidence"));
      cJSON_Delete(sent);
      *response_buf =
          strdup("{\"id\":\"controls\",\"object\":\"response\",\"status\":\"completed\","
                 "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
                 "\"type\":\"output_text\",\"text\":\"Controls fixture completed.\"}]}]}");
      return 200;
   }
   if (visibility_query_case)
   {
      assert(!strstr((const char *)body, "HIDDEN_QUERY_SECRET"));
      if (visibility_query_case == 1)
         assert(strstr((const char *)body, "visible primary task"));
   }
   if (rich_generation_case)
   {
      if (rich_generation_case != 3)
         assert(agent_admission_agent_active("context-refusal-fixture") == 1);
      cJSON *sent = cJSON_ParseWithLengthOpts(body, body_len, NULL, 0);
      assert(sent);
      if (rich_generation_case == 3)
         assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(sent, "tools")) > 0);
      else
         assert(!cJSON_GetObjectItemCaseSensitive(sent, "tools"));
      char *encoded = cJSON_PrintUnformatted(sent);
      assert(encoded);
      assert(strstr(encoded, "immutable cron task") && strstr(encoded, "script evidence") &&
             strstr(encoded, "prior evidence"));
      assert(!strstr(encoded, "private reasoning") && !strstr(encoded, "unrelated signature"));
      if (rich_generation_case >= 2)
         assert(strstr(encoded, "prior user task") && strstr(encoded, "prior assistant response") &&
                strstr(encoded, "second policy block"));
      if (rich_generation_case == 3)
      {
         assert(
             !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sent, "stop")), "END"));
         assert(cJSON_GetObjectItemCaseSensitive(sent, "max_tokens")->valueint == 256);
         assert(cJSON_GetObjectItemCaseSensitive(sent, "temperature")->valuedouble == 0.3);
         assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(sent, "min_p")) == 0.07);
         assert(strstr((const char *)body, "9007199254740993123456789"));
         cJSON *wire_messages = cJSON_GetObjectItemCaseSensitive(sent, "messages"), *wire_message;
         int memory_in_user = 0, host_policy_in_system = 0;
         cJSON_ArrayForEach(wire_message, wire_messages)
         {
            const char *role =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(wire_message, "role"));
            char *wire_text = cJSON_PrintUnformatted(wire_message);
            assert(role && wire_text);
            if (strstr(wire_text, "Go owner retained complete context."))
            {
               assert(!strcmp(role, "user"));
               memory_in_user++;
            }
            if (!strcmp(role, "system"))
               assert(!strstr(wire_text, "Go owner retained complete context."));
            if (strstr(wire_text, "Use paths relative to this workspace"))
            {
               assert(!strcmp(role, "system"));
               host_policy_in_system++;
            }
            free(wire_text);
         }
         assert(memory_in_user == 1 && host_policy_in_system == 1);
      }
      free(encoded);
      cJSON_Delete(sent);
   }
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
      if (simple_trailing)
      {
         size_t length = strlen(*response_buf);
         char *extended = realloc(*response_buf, length + sizeof " trailing");
         assert(extended);
         *response_buf = extended;
         memcpy(extended + length, " trailing", sizeof " trailing");
      }
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
   snprintf(last_ingress_query, sizeof last_ingress_query, "%s", query ? query : "");
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
         const char *text = projection_text_override
                                ? projection_text_override
                                : "# Recall\nGo owner retained complete context.\n";
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

static agent_config_t *fixture_config(const agent_t *agent)
{
   agent_config_t *cfg = calloc(1, sizeof *cfg);
   assert(cfg);
   cfg->agent_count = 1;
   cfg->agents[0] = *agent;
   cfg->agents[0].enabled = 1;
   cfg->agents[0].max_parallel = 1;
   snprintf(cfg->default_agent, sizeof cfg->default_agent, "%s", agent->name);
   return cfg;
}

static void test_owned_plain_generation(const agent_t *agent)
{
   agent_config_t *cfg = fixture_config(agent);
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

static void test_actual_generation_preserves_rich_canonical_request(const agent_t *agent)
{
   agent_config_t *cfg = fixture_config(agent);
   cfg->agents[0].tools_enabled = 1;
   cfg->agents[0].write_capable = 1;
   aimee_block_t policy[2] = {{.type = AIMEE_BLK_TEXT, .text = "Unattended host policy."},
                              {.type = AIMEE_BLK_TEXT, .text = "second policy block"}};
   for (int i = 0; i < 2; i++)
      aimee_ir_block_set_context(&policy[i], AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_POLICY,
                                 AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                                 0);
   aimee_block_t blocks[4] = {{.type = AIMEE_BLK_TEXT, .text = "immutable cron task"},
                              {.type = AIMEE_BLK_TEXT, .text = "script evidence"},
                              {.type = AIMEE_BLK_TEXT, .text = "prior evidence"},
                              {.type = AIMEE_BLK_THINKING,
                               .text = "private reasoning",
                               .thinking_signature = "unrelated signature"}};
   aimee_ir_block_set_context(&blocks[0], AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_ir_block_set_context(&blocks[1], AIMEE_CTX_ORIGIN_TOOL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_ir_block_set_context(&blocks[2], AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_ir_block_set_context(&blocks[3], AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNTRUSTED, AIMEE_CTX_SENS_CONFIDENTIAL, 0,
                              "source-domain", "source-scope", 42);
   aimee_block_t first = {.type = AIMEE_BLK_TEXT, .text = "prior user task"},
                 second = {.type = AIMEE_BLK_TEXT, .text = "prior assistant response"};
   aimee_ir_block_set_context(&first, AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_ir_block_set_context(&second, AIMEE_CTX_ORIGIN_MODEL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_message_t messages[3] = {{.role = "user", .blocks = &first, .n_blocks = 1},
                                  {.role = "assistant", .blocks = &second, .n_blocks = 1},
                                  {.role = "user", .blocks = blocks, .n_blocks = 4}};
   for (int history = 0; history < 2; history++)
      for (int method = 0; method < 2; method++)
      {
         aimee_request_t source = {.system = policy,
                                   .n_system = history ? 2 : 1,
                                   .messages = history ? messages : &messages[2],
                                   .n_messages = history ? 3 : 1,
                                   .has_max_tokens = 1,
                                   .max_tokens = 256,
                                   .has_temperature = 1,
                                   .temperature = 0.3,
                                   .tool_choice =
                                       method ? AIMEE_TOOL_CHOICE_NONE : AIMEE_TOOL_CHOICE_UNSET};
         cJSON *before = aimee_ir_request_json(&source);
         assert(before);
         rich_generation_case = history + 1;
         provider_calls = recalls = hook_calls = 0;
         agent_result_t result = {0};
         int rc = method ? agent_generate_request(cfg, &source, &result)
                         : agent_generate(cfg, agent->name, &source, &result);
         if (rc || !result.success)
            fprintf(stderr, "actual generation history=%d method=%d rc=%d error=%s\n", history,
                    method, rc, result.error);
         assert(!rc && result.success && provider_calls == 1 && recalls == 0 && hook_calls == 0);
         assert(!aimee_ir_response_complete_text_valid(&result.canonical_response));
         cJSON *after = aimee_ir_request_json(&source);
         assert(after && cJSON_Compare(before, after, 1));
         cJSON_Delete(before);
         cJSON_Delete(after);
         assert(cfg->agents[0].write_capable && cfg->agents[0].tools_enabled);
         agent_result_clear_response(&result);
         assert(agent_admission_global_active() == 0);
         if (!history && !method)
         {
            provider_calls = 0;
            source.stream = 1;
            assert(agent_generate(cfg, agent->name, &source, &result) < 0 && provider_calls == 0);
            source.stream = 0;
            source.model = "caller model override";
            assert(agent_generate(cfg, agent->name, &source, &result) < 0 && provider_calls == 0);
            source.model = NULL;
            cJSON *schema = cJSON_Parse("{\"type\":\"object\"}");
            assert(schema);
            aimee_tool_t tool = {.name = "write_file", .schema = schema};
            source.tools = &tool;
            source.n_tools = 1;
            assert(!aimee_ir_request_admit(&source));
            assert(agent_generate(cfg, agent->name, &source, &result) < 0 && provider_calls == 0);
            source.tools = NULL;
            source.n_tools = 0;
            cJSON_Delete(schema);
            snprintf(cfg->agents[0].backend, sizeof cfg->agents[0].backend, "%s",
                     AGENT_BACKEND_PROVIDER_CLI);
            assert(agent_generate(cfg, agent->name, &source, &result) < 0 && provider_calls == 0);
            cfg->agents[0].backend[0] = 0;
         }
      }
   cJSON *metadata = aimee_ir_json_parse_frame("{\"opaque\":9007199254740993123456789}",
                                               strlen("{\"opaque\":9007199254740993123456789}"));
   assert(metadata);
   char *stops[] = {"END"};
   aimee_request_t tools_source = {.system = policy,
                                   .n_system = 2,
                                   .messages = messages,
                                   .n_messages = 3,
                                   .has_max_tokens = 1,
                                   .max_tokens = 256,
                                   .has_temperature = 1,
                                   .temperature = 0.3,
                                   .metadata = metadata,
                                   .stop_sequences = stops,
                                   .n_stop = 1};
   /* A declared, same-model host extension must survive typed evidence and
    * privacy projection; it is not a caller model pin or native transcript. */
   aimee_request_t host_profile_source = {.frontend = AIMEE_WIRE_OPENAI_CHAT,
                                          .model = cfg->agents[0].model};
   aimee_openai_host_profile_t host_profile = {.min_p = 0.07, .repeat_penalty = -1};
   assert(!openai_ir_set_host_profile(&host_profile_source, &host_profile));
   tools_source.frontend = AIMEE_WIRE_OPENAI_CHAT;
   tools_source.raw = host_profile_source.raw;
   host_profile_source.raw = NULL;
   cJSON *before = aimee_ir_request_json(&tools_source);
   assert(before);
   rich_generation_case = 3;
   provider_calls = recalls = 0;
   agent_result_t tools_result = {0};
   int tools_rc = agent_execute_with_tools_for_role(&cfg->agents[0], &cfg->network, "review",
                                                    &tools_source, &tools_result);
   if (tools_rc || !tools_result.success)
      fprintf(stderr, "actual rich tools rc=%d calls=%d error=%s\n", tools_rc, provider_calls,
              tools_result.error);
   assert(!tools_rc && tools_result.success && provider_calls == 1);
   cJSON *after = aimee_ir_request_json(&tools_source);
   assert(after && cJSON_Compare(before, after, 1));
   cJSON_Delete(before);
   cJSON_Delete(after);
   cJSON_Delete(metadata);
   cJSON_Delete(tools_source.raw);
   agent_result_clear_response(&tools_result);
   rich_generation_case = 0;
   free(cfg);
}

typedef struct
{
   int expected, events, reject;
} control_projection_t;
static int capture_control_projection(const aimee_session_event_t *event, void *context)
{
   control_projection_t *observation = context;
   if (event->kind != AIMEE_SESSION_PROJECTION || !event->projection_request_controls)
      return 0;
   assert(event->projection_request_controls == observation->expected);
   observation->events++;
   return observation->reject;
}

static void check_control_projection(agent_config_t *cfg, aimee_request_t *source,
                                     int expected_controls)
{
   control_projection_t observation = {.expected = expected_controls};
   cJSON *before = aimee_ir_request_json(source);
   assert(before);
   agent_result_t result = {0};
   provider_calls = observation.events = 0;
   source->tool_choice = AIMEE_TOOL_CHOICE_NONE;
   assert(agent_generate_request(cfg, source, &result) == AGENT_RC_CONTEXT_REFUSED);
   source->tool_choice = AIMEE_TOOL_CHOICE_UNSET;
   assert(!provider_calls);
   agent_result_clear_response(&result);
   aimee_response_t response = {0};
   assert(agent_execute_request_with_tools(&cfg->agents[0], &cfg->network, "review", source,
                                           &response, &result) == AGENT_RC_CONTEXT_REFUSED);
   assert(!provider_calls);
   agent_result_clear_response(&result);
   aimee_response_free(&response);
   aimee_ir_session_set_sink(capture_control_projection, &observation);
   for (int tools = 0; tools <= 1; tools++)
   {
      provider_calls = observation.events = 0;
      int rc = tools ? agent_execute_with_tools_for_role(&cfg->agents[0], &cfg->network, "review",
                                                         source, &result)
                     : agent_generate(cfg, cfg->agents[0].name, source, &result);
      if (rc || !result.success || provider_calls != 1 || observation.events != 1)
         fprintf(stderr, "control provider=%s tools=%d rc=%d calls=%d events=%d error=%s\n",
                 cfg->agents[0].provider, tools, rc, provider_calls, observation.events,
                 result.error);
      assert(!rc && result.success && provider_calls == 1 && observation.events == 1);
      agent_result_clear_response(&result);
   }
   provider_calls = observation.events = 0;
   observation.reject = 1;
   assert(agent_execute_with_tools_for_role(&cfg->agents[0], &cfg->network, "review", source,
                                            &result) < 0);
   assert(!provider_calls && observation.events == 1 && aimee_ir_session_capture_failed());
   agent_result_clear_response(&result);
   observation.reject = 0;
   aimee_ir_session_set_sink(NULL, NULL);
   aimee_ir_session_begin_task();
   cJSON *after = aimee_ir_request_json(source);
   assert(after && cJSON_Compare(before, after, 1));
   cJSON_Delete(before);
   cJSON_Delete(after);
}

static void test_actual_responses_controls(const agent_t *configured)
{
   agent_config_t *cfg = fixture_config(configured);
   cfg->agents[0].tools_enabled = 1;
   snprintf(cfg->agents[0].provider, sizeof cfg->agents[0].provider, "chatgpt");
   aimee_request_t source = {0};
   assert(!aimee_ir_request_from_text(&source, "Retain original policy.", "immutable control task",
                                      128, 0.3));
   aimee_block_t *blocks = realloc(source.messages[0].blocks, 2 * sizeof *blocks);
   assert(blocks);
   source.messages[0].blocks = blocks;
   blocks[1] = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup("separate control evidence")};
   source.messages[0].n_blocks = 2;
   aimee_ir_block_set_context(&blocks[1], AIMEE_CTX_ORIGIN_TOOL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   source.has_top_p = source.has_top_k = source.has_parallel_tool_calls = 1;
   source.top_p = 0.8;
   source.top_k = 17;
   source.metadata = cJSON_Parse("{\"source\":\"preserve\"}");
   source.thinking = cJSON_Parse("{\"type\":\"disabled\"}");
   source.service_tier = strdup("auto");
   source.prompt_cache_key = strdup("owned-cache-scope");
   source.prompt_cache_retention = strdup("24h");
   source.prompt_cache_options = cJSON_Parse("{\"mode\":\"explicit\"}");
   source.stop_sequences = calloc(1, sizeof(char *));
   assert(source.stop_sequences);
   source.stop_sequences[0] = strdup("END");
   source.n_stop = 1;
   responses_controls_case = 1;
   check_control_projection(cfg, &source, 9);
   responses_controls_case = 0;
   aimee_request_free(&source);
   free(cfg);
}

static void test_actual_other_provider_controls(const agent_t *configured)
{
   for (int provider = 1; provider <= 2; provider++)
   {
      agent_config_t *cfg = fixture_config(configured);
      cfg->agents[0].tools_enabled = 1;
      snprintf(cfg->agents[0].provider, sizeof cfg->agents[0].provider, "%s",
               provider == 2 ? "anthropic" : "openai");
      aimee_request_t source = {0};
      assert(!aimee_ir_request_from_text(&source, NULL, "other provider task; retained evidence",
                                         128, 0.3));
      source.has_top_p = 1;
      source.top_p = 0.8;
      source.metadata = cJSON_Parse("{\"user_id\":\"canonical-fixture\"}");
      source.service_tier = strdup("auto");
      source.reasoning_summary = strdup("auto");
      other_controls_case = provider;
      check_control_projection(cfg, &source, 1);
      agent_result_t result = {0};
      aimee_response_t response = {0};
      free(source.reasoning_summary);
      source.reasoning_summary = NULL;
      control_projection_t observation = {0};
      aimee_ir_session_set_sink(capture_control_projection, &observation);
      provider_calls = 0;
      source.tool_choice = AIMEE_TOOL_CHOICE_NONE;
      int supported_rc = agent_generate_request(cfg, &source, &result);
      source.tool_choice = AIMEE_TOOL_CHOICE_UNSET;
      if (supported_rc || !result.success)
         fprintf(stderr, "supported controls provider=%d rc=%d calls=%d error=%s\n", provider,
                 supported_rc, provider_calls, result.error);
      assert(!supported_rc && result.success);
      assert(provider_calls == 1 && !observation.events);
      agent_result_clear_response(&result);
      provider_calls = 0;
      assert(!agent_execute_request_with_tools(&cfg->agents[0], &cfg->network, "review", &source,
                                               &response, &result) &&
             result.success);
      assert(provider_calls == 1);
      aimee_response_free(&response);
      agent_result_clear_response(&result);
      aimee_ir_session_set_sink(NULL, NULL);
      aimee_request_free(&source);
      free(cfg);
   }
   other_controls_case = 0;
}

static void test_visible_primary_and_routing_queries(const agent_t *agent)
{
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "HIDDEN_QUERY_SECRET"},
                             {.type = AIMEE_BLK_TEXT, .text = "visible primary task"}};
   aimee_ir_block_set_context(&blocks[0], AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_RESTRICTED, 1, NULL, NULL,
                              0);
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = 2};
   cJSON *before = aimee_ir_message_json(&message), *updated = NULL;
   assert(before);
   agent_result_t result = {0};
   provider_calls = 0;
   visibility_query_case = 1;
   last_ingress_query[0] = 0;
   aimee_request_t request = {.messages = &message,
                              .n_messages = 1,
                              .has_max_tokens = 1,
                              .max_tokens = 128,
                              .has_temperature = 1,
                              .temperature = 0.3};
   int rc = agent_execute_session_with_tools(agent, NULL, &request, NULL, &updated, &result);
   assert(!rc && result.success && provider_calls == 1);
   assert(!strcmp(last_ingress_query, "visible primary task"));
   cJSON *after = aimee_ir_message_json(&message);
   assert(after && cJSON_Compare(before, after, 1));
   cJSON_Delete(before);
   cJSON_Delete(after);
   cJSON_Delete(updated);
   agent_result_clear_response(&result);
   agent_config_t *cfg = fixture_config(agent);
   cfg->agents[0].tools_enabled = 1;
   message.n_blocks = 1;
   aimee_request_t source = {.messages = &message, .n_messages = 1};
   visibility_query_case = 2;
   provider_calls = 0;
   rc = agent_run(cfg, "review", &source, &result);
   /* No review route is configured: routing still computes its real input view
    * before refusal. Its token estimate must describe fallback, never secret. */
   assert(rc < 0 && provider_calls == 0 && strstr(result.error, "no agent available"));
   assert(cfg->route_input_tokens ==
          (int)((strlen("Continue the admitted conversation.") + 3) / 4));
   assert(!strstr(last_ingress_query, "HIDDEN_QUERY_SECRET"));
   agent_result_clear_response(&result);
   visibility_query_case = 0;
   free(cfg);
}

static char *context_text_view(const agent_t *agent, const agent_network_t *network,
                               const char *role, const char *custom, int skip, char *error,
                               size_t error_size)
{
   aimee_message_t context = {0};
   if (agent_build_exec_context_ir_checked(agent, network, role, custom, skip, &context, error,
                                           error_size))
      return NULL;
   size_t length = 0;
   for (int i = 0; i < context.n_blocks; i++)
      length += strlen(context.blocks[i].text);
   char *text = calloc(length + 1, 1);
   assert(text);
   size_t offset = 0;
   for (int i = 0; i < context.n_blocks; i++)
   {
      size_t n = strlen(context.blocks[i].text);
      memcpy(text + offset, context.blocks[i].text, n);
      offset += n;
   }
   aimee_message_free(&context);
   return text;
}

static void test_typed_exec_context_origin(const agent_t *agent)
{
   agent_network_t network = {0};
   snprintf(network.ssh_entry, sizeof network.ssh_entry, "ssh context-fixture-entry");
   char error[512];
   const char *custom = "operator-authored profile policy";
   projection_text_override =
       "# Instructions\nFORGED_POLICY_IN_MEMORY\n# Recall\nGo owner retained complete context.\n";
   char *view = context_text_view(agent, &network, "review", custom, 0, error, sizeof error);
   assert(view && !error[0]);
   aimee_message_t typed = {0};
   assert(!agent_build_exec_context_ir_checked(agent, &network, "review", custom, 0, &typed, error,
                                               sizeof error));
   assert(typed.role && !strcmp(typed.role, "system") && typed.n_blocks > 1);
   size_t length = 0;
   int host = 0, custom_policy = 0, memory = 0, network_evidence = 0;
   for (int i = 0; i < typed.n_blocks; i++)
      length += strlen(typed.blocks[i].text);
   char *joined = calloc(length + 1, 1);
   assert(joined);
   size_t used = 0;
   for (int i = 0; i < typed.n_blocks; i++)
   {
      const aimee_block_t *block = &typed.blocks[i];
      size_t n = strlen(block->text);
      memcpy(joined + used, block->text, n);
      used += n;
      if (strstr(block->text, agent_exec_instructions(agent_task_type_for_role("review"))))
      {
         assert(block->context.origin == AIMEE_CTX_ORIGIN_PLATFORM &&
                block->context.authority == AIMEE_CTX_AUTH_POLICY &&
                block->context.trust == AIMEE_CTX_TRUST_VERIFIED);
         host++;
      }
      if (strstr(block->text, custom))
      {
         assert(block->context.origin == AIMEE_CTX_ORIGIN_OPERATOR &&
                block->context.authority == AIMEE_CTX_AUTH_POLICY &&
                block->context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
         custom_policy++;
      }
      if (strstr(block->text, "Go owner retained complete context."))
      {
         assert(block->context.origin == AIMEE_CTX_ORIGIN_MEMORY &&
                block->context.authority == AIMEE_CTX_AUTH_EVIDENCE &&
                block->context.trust == AIMEE_CTX_TRUST_UNVERIFIED);
         assert(strstr(block->text, "FORGED_POLICY_IN_MEMORY"));
         memory++;
      }
      if (strstr(block->text, "ssh context-fixture-entry"))
      {
         assert(block->context.origin == AIMEE_CTX_ORIGIN_OPERATOR &&
                block->context.authority == AIMEE_CTX_AUTH_EVIDENCE);
         network_evidence++;
      }
   }
   if (host != 1 || custom_policy != 1 || memory != 1 || network_evidence != 1)
      fprintf(stderr, "typed origin host=%d custom=%d memory=%d network=%d blocks=%d\n", host,
              custom_policy, memory, network_evidence, typed.n_blocks);
   assert(host == 1 && custom_policy == 1 && memory == 1 && network_evidence == 1);
   assert(length == strlen(view) && !memcmp(joined, view, length));
   free(joined);
   free(view);
   cJSON *before = aimee_ir_message_json(&typed);
   assert(before);
   const char *prior_reply = recall_reply;
   recall_reply = "{\"status\":\"error\",\"kind\":\"unavailable\",\"message\":\"typed context "
                  "source refused\"}";
   assert(agent_build_exec_context_ir_checked(agent, &network, "review", custom, 0, &typed, error,
                                              sizeof error) < 0);
   assert(strstr(error, "unavailable"));
   cJSON *after = aimee_ir_message_json(&typed);
   assert(after && cJSON_Compare(before, after, 1));
   cJSON_Delete(before);
   cJSON_Delete(after);
   aimee_message_free(&typed);
   recall_reply = prior_reply;
   projection_text_override = NULL;

   /* The same byte budget must remain canonical when a multibyte authored
    * policy straddles the final available byte. No partial UTF-8 block. */
   assert(!config_set_memory_recall_enabled(0));
   size_t budget = agent_exec_context_budget_chars(agent) + 4096;
   char *large = malloc(budget * 3 + 1);
   assert(large);
   for (size_t i = 0; i < budget; i++)
      memcpy(large + i * 3, "—", 3);
   large[budget * 3] = 0;
   char *bounded = context_text_view(agent, &network, "review", large, 1, error, sizeof error);
   assert(bounded && !error[0] && strlen(bounded) < budget && text_is_valid_utf8(bounded));
   free(bounded);
   free(large);
   assert(!config_set_memory_recall_enabled(1));
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
   agent_t agent = {.max_parallel = 1};
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
      char *text =
          context_text_view(&agent, &network, "review", "original prompt", 0, error, sizeof(error));
      assert(text == NULL);
      assert(strstr(error, "memory context refused"));
      aimee_message_t refused = {0};
      assert(agent_build_exec_context_ir_checked(&agent, &network, "review", "original prompt", 0,
                                                 &refused, error, sizeof error) < 0);
      assert(!refused.role && !refused.blocks);
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
   char *text =
       context_text_view(&agent, &network, "review", "original prompt", 0, error, sizeof(error));
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
   text = context_text_view(&agent, &network, "review", NULL, 0, error, sizeof(error));
   assert(text && !error[0] && strstr(text, "Go owner retained complete context."));
   free(text);
   /* A configured but unavailable KB is different: never silently downgrade
    * to private recall after losing the shared owner's required constraints. */
   assert(config_set_kb_mode("remote") == 0);
   int before_missing_connection = recalls;
   text = context_text_view(&agent, &network, "review", NULL, 0, error, sizeof(error));
   assert(!text && strstr(error, "unavailable") && recalls == before_missing_connection);
   assert(config_set_kb_mode("") == 0);
   assert(platform_setenv("AIMEE_KB_API_URL", "http://127.0.0.1:1") == 0);
   assert(kb_client_connection_configured());
   int before_shared_refusal = recalls;
   text = context_text_view(&agent, &network, "review", NULL, 0, error, sizeof(error));
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
   assert_ir_text(&result.canonical_response, "Fixture completed.");
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
   /* Both real HTTP execution paths must reject bytes after a complete JSON value. */
   simple_finish = "stop";
   simple_trailing = 1;
   for (int tools = 0; tools <= 1; tools++)
   {
      provider_calls = 0;
      rc = tools ? fixture_agent_role_text(&agent, &network, NULL, "original prompt", "task", 128,
                                           0, &result)
                 : fixture_agent_execute_text(&agent, NULL, "original prompt", 128, 0, &result);
      assert(rc != 0 && !result.success && provider_calls == 1);
      agent_result_clear_response(&result);
   }
   simple_trailing = 0;
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
   test_actual_generation_preserves_rich_canonical_request(&agent);
   test_actual_responses_controls(&agent);
   test_actual_other_provider_controls(&agent);
   test_visible_primary_and_routing_queries(&agent);
   test_typed_exec_context_origin(&agent);
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
      if (rc || !result.success || provider_calls != 2 || identity_results != 1)
         fprintf(stderr, "identity case=%d rc=%d calls=%d results=%d error=%s\n", identity_case, rc,
                 provider_calls, identity_results, result.error);
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
