/* Memory is an internal bus service. The same current Make target now tests
 * the thin owner client and guarantees HTTP is never an owner fallback.
 * The former HTTP-mock suite/source closure is retained explicitly in private
 * experiments/legacy-aimee-go/fixtures/pre-thin-memory-http-20261026. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_client.h"
#include "runtime_secret.h"
#include "support/mock_agent_http.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int owner_mode, owner_calls, http_calls;
/* An enrolled owner-to-owner mTLS link to a separately deployed KB. */
static int mtls_enabled, mtls_calls;
static char mtls_path[256];
int kb_client_mtls_configured(void)
{
   return mtls_enabled;
}
char *kb_client_mtls_request_timeout_with_type(const char *method, const char *path,
                                               const char *body, const char *content_type,
                                               int timeout_ms, int *status_out)
{
   (void)content_type;
   (void)timeout_ms;
   assert(mtls_enabled && !strcmp(method, "POST") && body);
   mtls_calls++;
   snprintf(mtls_path, sizeof mtls_path, "%s", path);
   *status_out = 200;
   return strdup("{\"status\":\"ok\",\"recall\":{\"active_context\":[]}}");
}
char *kb_client_mtls_request_timeout(const char *method, const char *path, const char *body,
                                     int timeout_ms, int *status_out)
{
   return kb_client_mtls_request_timeout_with_type(method, path, body, NULL, timeout_ms,
                                                   status_out);
}
static const char *expected_method, *owner_reply;
static int expected_timeout;
static cJSON *observed_arguments;
int config_present(void)
{
   return 0;
}
int config_integrity_enabled(void)
{
   return 1;
}
int config_integrity_dry_run(void)
{
   return 0;
}

/* Controlled SDK transport peer; actual SDK codecs and broker authorization
 * have their own tests. This peer never invokes the HTTP mock. */
int aimee_module_commands_owner_action_raw(const char *method, const cJSON *arguments, int timeout,
                                           char **reply)
{
   assert(expected_method && !strcmp(method, expected_method));
   assert(timeout == expected_timeout && cJSON_IsObject(arguments));
   owner_calls++;
   cJSON_Delete(observed_arguments);
   observed_arguments = cJSON_Duplicate(arguments, 1);
   *reply = owner_mode == 1 && owner_reply ? strdup(owner_reply) : NULL;
   return owner_mode;
}
static int business_http(const char *url, const char *auth, const char *body, char **response,
                         int timeout, const char *extra)
{
   (void)body;
   (void)timeout;
   (void)extra;
   assert(!strstr(url, "memory")); /* Every unexpected memory HTTP call aborts. */
   assert(strstr(url, "/v1/actions/notes.list") ||
          strstr(url, "/v1/actions/evidence.document_metadata"));
   assert(auth && strstr(auth, "owned-thin-test-token"));
   http_calls++;
   *response = strdup("{\"status\":\"ok\",\"notes\":[]}");
   return 200;
}
static char *request(const char *method, const char *body, int timeout)
{
   expected_method = method;
   expected_timeout = timeout;
   return kb_v1_action_request_with_timeout(method, cJSON_Parse(body), timeout);
}
static void unavailable_is_not_empty(void)
{
   owner_mode = 0;
   owner_reply = NULL;
   char *raw = request("memory.list", "{\"scope\":\"all\"}", 731);
   cJSON *value = cJSON_Parse(raw);
   assert(value);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, "status")),
                  "unavailable"));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value, "retryable")));
   assert(!cJSON_HasObjectItem(value, "memories") && !http_calls);
   free(raw);
   cJSON_Delete(value);
   owner_mode = -1;
   raw = request("memory.list", "{}", 731);
   value = cJSON_Parse(raw);
   assert(value);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, "status")), "ok"));
   assert(!cJSON_HasObjectItem(value, "memories") && !http_calls);
   free(raw);
   cJSON_Delete(value);
   owner_mode = 1;
   owner_reply = "{\"status\":\"ok\",\"memories\":[]}";
   raw = request("memory.list", "{}", 731);
   assert(!strcmp(raw, owner_reply));
   value = cJSON_Parse(raw);
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value, "memories")) == 0);
   free(raw);
   cJSON_Delete(value);
   assert(!http_calls);
}
static void exact_raw_data_scope_and_budget(void)
{
   owner_mode = 1;
   owner_reply = "{\"status\":\"ok\",\"id\":18446744073709551615,\"native_belief\":"
                 "\"18446744073709551615\",\"text\":\"exact UTF-8 界\"}";
   char *raw = request("memory.get",
                       "{\"store\":\"kb\",\"id\":\"18446744073709551615\",\"scope\":{\"type\":"
                       "\"project\",\"value\":\"one\"}}",
                       300123);
   assert(!strcmp(raw, owner_reply));
   free(raw);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(observed_arguments, "id")),
                  "18446744073709551615"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                      cJSON_GetObjectItemCaseSensitive(observed_arguments, "scope"), "value")),
                  "one"));
   assert(!http_calls);
   owner_reply = "{\"status\":\"error\",\"kind\":\"permission_denied\",\"http_status\":403,"
                 "\"message\":\"actual service refusal\"}";
   raw = request("memory.store", "{\"content\":\"fixture\"}", 731);
   assert(!strcmp(raw, owner_reply));
   free(raw);
   /* An actual selected-owner projection refusal remains authoritative: do not
    * reclassify it as the missing optional dependency above. */
   owner_reply = "{\"status\":\"error\",\"kind\":\"invalid_projection\","
                 "\"message\":\"selected owner refused projection\"}";
   raw = request("memory.assemble_typed_context", "{\"query\":\"fixture\"}", 731);
   assert(!strcmp(raw, owner_reply) && !http_calls);
   free(raw);
   owner_reply = "{} trailing";
   raw = request("memory.serve", "{\"store\":\"user\"}", 731);
   assert(raw && !strstr(raw, "trailing") && !http_calls);
   free(raw);
}
static void every_memory_namespace_refuses_fallback(void)
{
   static const char *methods[] = {"memory.serve",
                                   "memory.claim_card",
                                   "memory.recall",
                                   "memory.get",
                                   "memory.store",
                                   "memory.delete",
                                   "memory.prospective_list",
                                   "memory.directive_create",
                                   "memory.not_implemented",
                                   "memory.",
                                   "session_briefing.commitments",
                                   "session_briefing.directives",
                                   "evidence.emit_retrieval_event",
                                   "evidence.merge_retrieval_event",
                                   "evidence.trace_retrieval_event",
                                   "evidence.provenance_retrieval_event",
                                   "evidence.fidelity_retrieval_event",
                                   "ranker.emit_event",
                                   "ranker.record_outcome"};
   owner_mode = 0;
   for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++)
   {
      char *raw = request(methods[i], "{}", 731);
      assert(raw && strstr(raw, "memory bus service unavailable or unsupported"));
      cJSON *value = cJSON_Parse(raw);
      assert(value &&
             !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, "status")),
                     "unavailable"));
      assert(!cJSON_HasObjectItem(value, "memories") && !http_calls);
      cJSON_Delete(value);
      free(raw);
   }
   assert(!http_calls);
}
static void unrelated_business_http_is_not_removed(void)
{
   owner_mode = 0;
   char *raw = request("notes.list", "{}", 731);
   assert(raw && strstr(raw, "\"notes\":[]"));
   free(raw);
   assert(http_calls == 1);
   raw = request("evidence.document_metadata", "{}", 731);
   assert(raw);
   free(raw);
   assert(http_calls == 2);
}
/* The one memory transport exception: a Server enrolled into a separate KB
 * over mTLS reaches that KB's shared memory owner-to-owner. A bearer URL never
 * carries memory, and an installed owner still answers first. */
static void enrolled_mtls_link_carries_shared_memory(void)
{
   owner_mode = 0;
   mtls_enabled = 1;
   mtls_calls = http_calls = 0;
   char *raw = request("memory.recall", "{}", 731);
   assert(raw && strstr(raw, "\"recall\""));
   free(raw);
   assert(mtls_calls == 1 && !strcmp(mtls_path, "/v1/actions/memory.recall") && !http_calls);
   mtls_enabled = 0;
   raw = request("memory.recall", "{}", 731);
   assert(raw && strstr(raw, "memory bus service unavailable or unsupported"));
   free(raw);
   assert(mtls_calls == 1 && !http_calls);
}
int main(void)
{
   assert(!setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1));
   assert(!runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "owned-thin-test-token"));
   mock_agent_http_set_post_handler(business_http);
   unavailable_is_not_empty();
   exact_raw_data_scope_and_budget();
   every_memory_namespace_refuses_fallback();
   enrolled_mtls_link_carries_shared_memory();
   unrelated_business_http_is_not_removed();
   assert(owner_calls >= 17);
   cJSON_Delete(observed_arguments);
   mock_agent_http_reset();
   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   puts("memory thin client: unavailable differs from empty, exact raw data/scope/budget, no "
        "memory HTTP fallback; business transport unchanged");
   return 0;
}
