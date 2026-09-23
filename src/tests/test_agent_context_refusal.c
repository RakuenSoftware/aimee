/* Exercise the production context builder and native initial-dispatch boundary.
 * External module and HTTP transports are fixtures. Memory decisions are supplied
 * as Go-owner protocol replies; this test does not reimplement memory policy. */
#include "aimee.h"
#include "agent_exec.h"
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

static const char *recall_reply;
static int recalls;
static int refuse_after;
static int provider_calls;
static int final_response;
static int malformed_projection;
static int transport_refuse;

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

int http_retry_post_guarded_bytes(const char *url, const char *auth_header, const void *body,
                                  size_t body_len, char **response_buf, int timeout_ms,
                                  const char *extra_headers, int max_attempts, int base_ms,
                                  int max_ms, const char *provider, const char *model,
                                  const char *session_id, http_retry_admit_cb_t admit_retry)
{
   assert(admit_retry);
   if (transport_refuse)
   {
      provider_calls++;
      if (transport_refuse == 2 && provider_calls == 1)
      {
         *response_buf = strdup("{}");
         return 400; /* enter the model fallback before its retry is refused */
      }
      assert(request_context_refuse_assembly("stale_context") == 0);
      assert(admit_retry() != 0);
      *response_buf = NULL;
      return HTTP_RETRY_ADMISSION_REFUSED;
   }
   return http_retry_post_context_bytes(url, auth_header, body, body_len, response_buf, timeout_ms,
                                        extra_headers, max_attempts, base_ms, max_ms, provider,
                                        model, session_id);
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

int main(void)
{
   delegate_role_seam_install();
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
      agent_result_t result;
      int rc = agent_execute_with_tools_for_role(&agent, &network, "review", "original prompt",
                                                 "task", 128, 0, &result);
      assert(rc == AGENT_RC_CONTEXT_REFUSED);
      assert(!result.success && result.turns == 0 && result.tool_calls == 0);
      assert(strcmp(result.stop_reason, "context_refused") == 0);
      assert(strstr(result.error, "memory context refused"));
      assert(!agent_rc_should_try_another(rc, result.error));
      free(result.response);
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
      agent_result_t invalid_result;
      int invalid_rc = agent_execute_with_tools_for_role(
          &agent, &network, "review", "original prompt", "task", 128, 0, &invalid_result);
      assert(invalid_rc == AGENT_RC_CONTEXT_REFUSED && provider_calls == 0);
      assert(strstr(invalid_result.error, "invalid_projection"));
      free(invalid_result.response);
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
   agent_result_t result;
   int rc = agent_execute_with_tools_for_role(&agent, &network, NULL, "original prompt", "task",
                                              128, 0, &result);
   if (rc != AGENT_RC_CONTEXT_REFUSED)
      fprintf(stderr, "unexpected loop result: rc=%d calls=%d recalls=%d error=%s\n", rc,
              provider_calls, recalls, result.error);
   assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success);
   assert(recalls == 2 && provider_calls == 5);
   assert(strcmp(result.stop_reason, "context_refused") == 0);
   assert(strstr(result.error, "protected_context_overflow"));
   assert(!agent_rc_should_try_another(rc, result.error));
   free(result.response);
   recalls = provider_calls = refuse_after = 0;
   final_response = 1;
   recall_reply = "{\"status\":\"ok\",\"recall\":{}}";
   /* An HTTP worker can inherit an earlier ingress refusal. Even a successful
    * native recall must not clear it or dispatch through another provider. */
   request_context_t inherited = {0};
   request_context_set(&inherited);
   assert(request_context_refuse_assembly("protected_context_overflow") == 0);
   rc = agent_execute_with_tools_for_role(&agent, &network, NULL, "original prompt", "task", 128, 0,
                                          &result);
   assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success && provider_calls == 0);
   assert(strcmp(result.stop_reason, "context_refused") == 0);
   assert(strcmp(result.error, "protected_context_overflow") == 0);
   assert(!agent_rc_should_try_another(rc, result.error));
   free(result.response);
   request_context_clear();
   recalls = 0;
   rc = agent_execute_with_tools_for_role(&agent, &network, NULL, "original prompt", "task", 128, 0,
                                          &result);
   assert(rc == 0 && result.success && !result.error[0]);
   assert(recalls == 1 && provider_calls == 1);
   assert(result.response && strstr(result.response, "Fixture completed."));
   free(result.response);
   for (int fallback = 0; fallback <= 1; fallback++)
   {
      request_context_t retry_context = {0};
      request_context_set(&retry_context);
      recalls = provider_calls = 0;
      transport_refuse = fallback ? 2 : 1;
      snprintf(agent.fallback_model, sizeof(agent.fallback_model), "%s",
               fallback ? "fallback" : "");
      rc = agent_execute_with_tools_for_role(&agent, &network, NULL, "original prompt", "task", 128,
                                             0, &result);
      assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success);
      assert(provider_calls == (fallback ? 2 : 1));
      assert(strcmp(result.stop_reason, "context_refused") == 0);
      assert(strcmp(result.error, "stale_context") == 0);
      assert(!agent_rc_should_try_another(rc, result.error));
      free(result.response);
      /* The non-tool execution path must preserve the same refusal contract. */
      request_context_set(&retry_context);
      provider_calls = 0;
      rc = agent_execute(&agent, NULL, "original prompt", 128, 0, &result);
      assert(rc == AGENT_RC_CONTEXT_REFUSED && !result.success);
      assert(provider_calls == (fallback ? 2 : 1));
      assert(strcmp(result.stop_reason, "context_refused") == 0);
      assert(strcmp(result.error, "stale_context") == 0);
      assert(!agent_rc_should_try_another(rc, result.error));
      free(result.response);
      request_context_clear();
   }
   puts("agent context refusal: passed");
   return 0;
}
