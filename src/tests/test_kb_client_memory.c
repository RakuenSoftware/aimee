/* test_kb_client_memory.c: the kb_client memory read wrappers must
 * distinguish "kb unreachable" from "genuinely empty". Regression guard for
 * the bug where an unreachable knowledge service was reported as an empty
 * store (e.g. `aimee memory list` printing "No memories" during an outage).
 *
 * Drives the wrappers through the mocked agent_http transport: one handler
 * fails at the transport layer (unreachable), another returns a well-formed
 * ok envelope with empty arrays (healthy but empty). The contract is:
 *   unreachable -> < 0,   healthy+empty -> 0. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_client.h"
#include "kb_client_pii.h"
#include "runtime_secret.h"
#include "db1_client/caches.h"
#include "support/mock_agent_http.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The classifier itself belongs to the memory module. This suite exercises the
 * client-side no-transmit boundary with a deterministic module answer. */
static const char *screen_reply;
static int screen_transport = 1;
int aimee_module_commands_dispatch(const char *method, const cJSON *args, cJSON **result)
{
   assert(strcmp(method, "memory.screen_content") == 0);
   if (screen_reply || screen_transport != 1)
   {
      *result = screen_reply ? cJSON_Parse(screen_reply) : NULL;
      return screen_transport;
   }
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "content"));
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", "ok");
   cJSON_AddStringToObject(
       *result, "verdict",
       content && (strstr(content, "password") || strstr(content, "hunter2trustno1")) ? "reject"
                                                                                      : "allow");
   return 1;
}

static int activation_writes;
static int64_t activation_write_id;
static int64_t activation_write_turn;

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

const char *session_id(void)
{
   return "activation-client-session";
}

int db1_context_snapshot_activation(const char *session_id_arg,
                                    char (*out)[DB1_CONTEXT_ACTIVATION_ROW_LEN], int max)
{
   assert(strcmp(session_id_arg, "activation-client-session") == 0);
   assert(max >= 7);
   snprintf(out[0], DB1_CONTEXT_ACTIVATION_ROW_LEN, "0 7");
   snprintf(out[1], DB1_CONTEXT_ACTIVATION_ROW_LEN, "41 6");
   snprintf(out[2], DB1_CONTEXT_ACTIVATION_ROW_LEN, "52 3");
   snprintf(out[3], DB1_CONTEXT_ACTIVATION_ROW_LEN, "9007199254740993 6");
   snprintf(out[4], DB1_CONTEXT_ACTIVATION_ROW_LEN, "9223372036854775808 1");
   snprintf(out[5], DB1_CONTEXT_ACTIVATION_ROW_LEN, "12 3junk");
   snprintf(out[6], DB1_CONTEXT_ACTIVATION_ROW_LEN, "13 9223372036854775808");
   return 7;
}

int db1_context_snapshot_insert_turn(const char *session_id_arg, int64_t memory_id,
                                     double relevance_score, int64_t turn_index)
{
   assert(strcmp(session_id_arg, "activation-client-session") == 0);
   assert(relevance_score == 0.0);
   activation_writes++;
   activation_write_id = memory_id;
   activation_write_turn = turn_index;
   return 0;
}

static int composition_calls;
static int native_projection_bytes;
static char composition_task_hint[4097];
static int composition_transport = 1;
static const char *composition_reply;
int aimee_module_commands_dispatch_internal_timeout(const char *method, const cJSON *args,
                                                    int timeout_ms, cJSON **result)
{
   assert(timeout_ms == 60000);
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "operation")),
                 "compose-recall") == 0);
   const char *shared = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "shared_json"));
   assert(shared != NULL);
   composition_calls++;
   const cJSON *native = cJSON_GetObjectItemCaseSensitive(args, "native_context_bytes");
   native_projection_bytes = cJSON_IsNumber(native) ? native->valueint : -1;
   const char *hint = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "task_hint"));
   snprintf(composition_task_hint, sizeof(composition_task_hint), "%s", hint ? hint : "");
   *result = NULL;
   if (composition_transport != 1)
      return composition_transport;
   *result = cJSON_CreateObject();
   cJSON_AddStringToObject(*result, "status", "ok");
   cJSON_AddStringToObject(*result, "json", composition_reply ? composition_reply : shared);
   return 1;
}

/* Transport failure: no response body, sub-100 status. kb_v1_action_request
 * yields no successful envelope, so readers must report unavailability. */
static int unreachable_post_handler(const char *url, const char *auth_header, const char *body,
                                    char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = NULL;
   return -1;
}

/* Healthy kb that simply has no rows: well-formed "ok" envelope with every
 * result array empty. Readers must report 0, never < 0. */
static int empty_ok_post_handler(const char *url, const char *auth_header, const char *body,
                                 char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"memories\":[],\"facts\":[],\"results\":[],"
                             "\"conflicts\":[],\"edges\":[],\"relations\":[],\"links\":[]}");
   return 200;
}

static int expected_action_timeout;
static const char *expected_action;
static int action_refused;
static int budgeted_action_post(const char *url, const char *auth_header, const char *body,
                                char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)extra_headers;
   assert(strstr(url, expected_action));
   assert(auth_header && strstr(auth_header, "test-token"));
   assert(timeout_ms == expected_action_timeout);
   cJSON *request = cJSON_Parse(body);
   assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(request, "limit")));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "scope_context")));
   assert(strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "project")),
                 "budget-test") == 0);
   cJSON_Delete(request);
   *response_buf = strdup(action_refused ? "{\"status\":\"error\",\"message\":\"scope refused\"}"
                                         : "{\"status\":\"ok\",\"rebuilt\":12}");
   return action_refused ? 403 : 200;
}

static void test_generic_action_preserves_budget_auth_and_refusal(void)
{
   const char *actions[] = {"memory.reindex", "memory.rebuild", "memory.repair"};
   mock_agent_http_set_post_handler(budgeted_action_post);
   kb_client_memory_scope_context_set("", "budget-test", 0);
   for (int i = 0; i < 3; i++)
   {
      expected_action = actions[i];
      expected_action_timeout = (i == 0 ? 5 : 10) * 60 * 1000;
      for (action_refused = 0; action_refused <= 1; action_refused++)
      {
         cJSON *request = cJSON_CreateObject();
         cJSON_AddNumberToObject(request, "limit", 12);
         kb_client_memory_scope_context_apply(request);
         char *response =
             kb_v1_action_request_with_timeout(expected_action, request, expected_action_timeout);
         assert(response);
         assert(strstr(response, action_refused ? "scope refused" : "rebuilt"));
         free(response);
      }
   }
   kb_client_memory_scope_context_clear();
   mock_agent_http_reset();
}

static int explicit_scope_post_handler(const char *url, const char *auth_header, const char *body,
                                       char **response_buf, int timeout_ms,
                                       const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   assert(body != NULL);
   assert(strstr(body, "\"workspace\":\"explicit-workspace\"") != NULL);
   assert(strstr(body, "\"project\":\"explicit-project\"") != NULL);
   assert(strstr(body, "active-workspace") == NULL);
   assert(strstr(body, "active-project") == NULL);
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"facts\":[]}");
   return 200;
}

static int typed_context_expect_limits;
static const char *typed_context_expect_requirements;

static int typed_context_post_handler(const char *url, const char *auth_header, const char *body,
                                      char **response_buf, int timeout_ms,
                                      const char *extra_headers)
{
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   assert(url && strstr(url, "/v1/actions/memory.assemble_typed_context") != NULL);
   assert(body != NULL);
   assert(strstr(body, "\"query\":\"recover deployment\"") != NULL);
   assert(strstr(body, "enable_semantic_assertions") == NULL);
   assert(strstr(body, "enable_observations") == NULL);
   cJSON *request = cJSON_Parse(body);
   const cJSON *limits = cJSON_GetObjectItemCaseSensitive(request, "context_limits");
   if (typed_context_expect_limits)
   {
      assert(cJSON_IsObject(limits));
      assert(cJSON_GetObjectItemCaseSensitive(limits, "schema_version")->valueint == 1);
      assert(cJSON_GetObjectItemCaseSensitive(limits, "max_context_bytes")->valueint == 0);
   }
   else
      assert(!limits);
   if (typed_context_expect_requirements)
      assert(strstr(body, typed_context_expect_requirements));
   else
      assert(!cJSON_GetObjectItemCaseSensitive(request, "evidence_requirements"));
   cJSON_Delete(request);
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"revision\":9223372036854775807,\"used_tokens\":"
                             "4,\"rendered_context\":\"temporal\"}");
   return 200;
}

static int activation_recall_post_handler(const char *url, const char *auth_header,
                                          const char *body, char **response_buf, int timeout_ms,
                                          const char *extra_headers)
{
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   assert(url && strstr(url, "/v1/actions/memory.recall") != NULL);
   assert(body && strstr(body, "\"current_turn\":\"7\"") != NULL);
   assert(strstr(body, "\"memory_id\":\"41\"") != NULL);
   assert(strstr(body, "\"last_turn\":\"6\"") != NULL);
   assert(strstr(body, "\"memory_id\":\"9007199254740993\"") != NULL);
   assert(strstr(body, "9223372036854775808") == NULL);
   assert(strstr(body, "3junk") == NULL);
   if (response_buf)
      *response_buf =
          strdup("{\"status\":\"ok\",\"recall\":{\"identity\":[{\"memory_id\":73,"
                 "\"activation_managed\":true,\"text\":\"kept\"},{\"memory_id\":74,"
                 "\"text\":\"user-local\"}],"
                 "\"preferences\":[],\"active_context\":[],\"open_commitments\":[],"
                 "\"reminders\":[{\"memory_id\":99}],\"directives\":[{\"memory_id\":100}]}}");
   return 200;
}

static void test_recall_carries_and_records_production_activation(void)
{
   activation_writes = 0;
   activation_write_id = 0;
   activation_write_turn = 0;
   mock_agent_http_set_post_handler(activation_recall_post_handler);
   char *json = kb_client_memory_recall_json("activation path", 128, 0);
   assert(json != NULL);
   assert(strstr(json, "\"memory_id\":73") != NULL);
   free(json);
   assert(activation_writes == 1);
   assert(activation_write_id == 73);
   assert(activation_write_turn == 7);
   int calls = composition_calls;
   json = kb_client_memory_recall_shared_json("shared only", 128, 0);
   assert(json != NULL && composition_calls == calls);
   free(json);
   composition_reply = "{\"status\":\"ok\",\"recall\":{\"identity\":[{\"memory_id\":"
                       "9007199254740993,\"text\":\"個人\"}]}}";
   json = kb_client_memory_recall_json("composed", 128, 0);
   assert(json != NULL && strcmp(json, composition_reply) == 0);
   free(json);
   composition_reply = "{\"status\":\"error\",\"kind\":\"unavailable\"}";
   int writes = activation_writes;
   json = kb_client_memory_recall_json("failed composition", 128, 0);
   assert(json != NULL && strcmp(json, composition_reply) == 0 && activation_writes == writes);
   free(json);
   composition_reply =
       "{\"status\":\"ok\",\"recall\":{\"identity\":["
       "{\"memory_id\":9007199254740993,\"handle\":\"kb:memory:9007199254740993\",\"activation_"
       "managed\":true},"
       "{\"memory_id\":9007199254740993,\"handle\":\"kb:memory:9007199254740993\",\"activation_"
       "managed\":true},"
       "{\"memory_id\":9007199254740993,\"activation_managed\":true},"
       "{\"memory_id\":73.5,\"activation_managed\":true},"
       "{\"memory_id\":73,\"handle\":\"user:memory:73\",\"activation_managed\":true},"
       "{\"memory_id\":73,\"handle\":\"kb:memory:9223372036854775808\",\"activation_managed\":true}"
       ","
       "{\"memory_id\":73,\"handle\":\"kb:memory:073\",\"activation_managed\":true}]}}";
   json = kb_client_memory_recall_json("exact activation receipts", 128, 0);
   assert(json && strcmp(json, composition_reply) == 0);
   free(json);
   assert(activation_writes == writes + 1 && activation_write_id == INT64_C(9007199254740993));
   writes = activation_writes;
   composition_reply = NULL;
   composition_transport = -1;
   assert(kb_client_memory_recall_json("missing local owner", 128, 0) == NULL);
   assert(activation_writes == writes);
   composition_transport = 1;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(empty_ok_post_handler);
   composition_reply = "{\"status\":\"ok\",\"recall\":{\"identity\":[]}}";
   unsetenv("AIMEE_MEMORY_HEALTH_ENABLED");
   json = kb_client_memory_recall_native_json("native projection", 128, 0, 4096);
   assert(json && native_projection_bytes == 4096);
   free(json);
   assert(!composition_task_hint[0]);
   setenv("AIMEE_MEMORY_HEALTH_ENABLED", "1", 1);
   json = kb_client_memory_recall_native_json("native health capture", 128, 0, 4096);
   assert(json && strcmp(composition_task_hint, "native health capture") == 0);
   assert(!strstr(json, "native health capture"));
   free(json);
   char oversized_hint[4098];
   memset(oversized_hint, 'x', sizeof(oversized_hint) - 1);
   oversized_hint[sizeof(oversized_hint) - 1] = 0;
   json = kb_client_memory_recall_native_json(oversized_hint, 128, 0, 4096);
   assert(json && !composition_task_hint[0]);
   free(json);
   unsetenv("AIMEE_MEMORY_HEALTH_ENABLED");
   composition_reply = NULL;
   printf("  PASS: test_recall_carries_and_records_production_activation\n");
}

static void test_readers_distinguish_unreachable_from_empty(void)
{

   /* --- kb unreachable: every count-returning reader reports < 0 --- */
   kb_client_dependency_reset_for_tests();
   mock_agent_http_set_post_handler(unreachable_post_handler);
   char *failed = kb_v1_action_request("memory.find_facts_visible",
                                       cJSON_Parse("{\"query\":\"q\",\"limit\":8}"));
   cJSON *failure = failed ? cJSON_Parse(failed) : NULL;
   assert(failure &&
          strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(failure, "status")),
                 "unavailable") == 0);
   assert(!cJSON_GetObjectItemCaseSensitive(failure, "facts"));
   cJSON_Delete(failure);
   free(failed);

   /* --- healthy but empty: same readers report exactly 0 (not < 0) --- */
   /* Model dependency recovery between the two independent fixtures. Without
    * this reset the intentionally opened process breaker correctly suppresses
    * the healthy handler, which would test cooldown rather than empty-result
    * classification. Breaker recovery itself is covered by kb-client-search. */
   kb_client_dependency_reset_for_tests();
   mock_agent_http_set_post_handler(empty_ok_post_handler);
   char *raw = kb_v1_action_request("memory.find_facts_visible",
                                    cJSON_Parse("{\"query\":\"q\",\"limit\":8}"));
   cJSON *visible = raw ? cJSON_Parse(raw) : NULL;
   assert(visible &&
          strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(visible, "status")), "ok") ==
              0);
   const cJSON *facts = cJSON_GetObjectItemCaseSensitive(visible, "facts");
   assert(cJSON_IsArray(facts) && cJSON_GetArraySize(facts) == 0);
   cJSON_Delete(visible);
   free(raw);

   mock_agent_http_reset();
   printf("  PASS: test_readers_distinguish_unreachable_from_empty\n");
}

static void test_explicit_scope_overrides_ambient_context(void)
{
   mock_agent_http_set_post_handler(explicit_scope_post_handler);
   kb_client_memory_scope_context_set("active-workspace", "active-project", 0);
   cJSON *request = cJSON_Parse("{\"query\":\"q\",\"limit\":2,\"workspace\":\"explicit-workspace\","
                                "\"project\":\"explicit-project\"}");
   kb_client_memory_scope_context_apply(request);
   char *raw = kb_v1_action_request("memory.find_facts_visible", request);
   assert(raw);
   free(raw);
   kb_client_memory_scope_context_clear();
   mock_agent_http_reset();
   printf("  PASS: test_explicit_scope_overrides_ambient_context\n");
}

static void test_typed_context_uses_server_defaults(void)
{
   mock_agent_http_set_post_handler(typed_context_post_handler);
   char *context = kb_client_memory_assemble_typed_context("recover deployment");
   assert(context && strcmp(context, "temporal") == 0);
   free(context);
   typed_context_expect_limits = 1;
   cJSON *limits = cJSON_Parse("{\"schema_version\":1,\"max_context_bytes\":0}");
   context = kb_client_memory_assemble_typed_context_with_limits("recover deployment", limits);
   assert(context && strcmp(context, "temporal") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(limits, "max_context_bytes")->valueint == 0);
   free(context);
   context = kb_client_memory_assemble_typed_context_json("recover deployment", limits);
   assert(context && strcmp(context, "{\"status\":\"ok\",\"revision\":9223372036854775807,\"used_"
                                     "tokens\":4,\"rendered_context\":\"temporal\"}") == 0);
   free(context);
   typed_context_expect_requirements =
       "{\"schema_version\":1,\"task_revision\":\"9007199254740993\","
       "\"query_mode\":\"current_state\",\"obligations\":[{\"subject\":\"deployment\",\"relation\":"
       "\"uses\"}]}";
   context = kb_client_memory_assemble_typed_context_requirements_json(
       "recover deployment", limits, typed_context_expect_requirements);
   assert(context);
   free(context);
   /* Duplicate fields and null must reach the owner's strict decoder intact. */
   typed_context_expect_requirements = "{\"schema_version\":1,\"schema_version\":2}";
   context = kb_client_memory_assemble_typed_context_requirements_json(
       "recover deployment", limits, typed_context_expect_requirements);
   assert(context);
   free(context);
   typed_context_expect_requirements = "null";
   context = kb_client_memory_assemble_typed_context_requirements_json(
       "recover deployment", limits, typed_context_expect_requirements);
   assert(context);
   free(context);
   char oversized[16386];
   memset(oversized, ' ', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = '\0';
   assert(!kb_client_memory_assemble_typed_context_requirements_json("recover deployment", limits,
                                                                     oversized));
   typed_context_expect_requirements = NULL;
   cJSON_Delete(limits);
   typed_context_expect_limits = 0;
   mock_agent_http_reset();
   printf("  PASS: test_typed_context_uses_server_defaults\n");
}

#define PII_CASE(what, call)                                                                       \
   do                                                                                              \
   {                                                                                               \
      mock_agent_http_reset();                                                                     \
      mock_agent_http_set_post_handler(recording_post_handler);                                    \
      g_pii_posts = 0;                                                                             \
      g_pii_last_body[0] = '\0';                                                                   \
      call;                                                                                        \
      assert_secret_never_sent(what);                                                              \
   } while (0)

static void test_screen_failures(void)
{
   const char *replies[] = {"{}", "{\"status\":\"error\",\"verdict\":\"allow\"}",
                            "{\"status\":\"ok\",\"verdict\":\"unknown\"}",
                            "{\"status\":\"ok\",\"verdict\":\"redact\"}"};
   for (size_t i = 0; i < sizeof(replies) / sizeof(replies[0]); i++)
   {
      screen_reply = replies[i];
      char *out = NULL;
      assert(kb_client_pii_screen("fixture", &out) == -1 && out == NULL);
      assert(kb_client_pii_identifier_sensitive("fixture") == 1);
   }
   screen_reply = NULL;
   screen_transport = -1;
   char *out = NULL;
   assert(kb_client_pii_screen("fixture", &out) == -1 && out == NULL);
   screen_transport = 0;
   assert(kb_client_pii_screen("fixture", &out) == -1 && out == NULL);
   screen_transport = 1;
   screen_reply = "{\"status\":\"ok\",\"verdict\":\"redact\",\"redacted\":\"[REDACTED]\"}";
   assert(kb_client_pii_screen("fixture", &out) == 0 && strcmp(out, "[REDACTED]") == 0);
   free(out);
   assert(kb_client_pii_identifier_sensitive("fixture") == 1);
   screen_reply = NULL;
}

static int benchmark_posts;
static int benchmark_post(const char *url, const char *auth, const char *body, char **response,
                          int timeout, const char *headers)
{
   (void)headers;
   assert(strstr(url, "memory.benchmark") && auth && strstr(auth, "test-token"));
   assert(timeout == 120000);
   cJSON *request = cJSON_Parse(body);
   const char *corpus =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "corpus_json"));
   assert(corpus && strstr(corpus, "9007199254740993") && strstr(corpus, "完整"));
   cJSON_Delete(request);
   ++benchmark_posts;
   *response = strdup("{\"status\":\"ok\",\"case_results\":[]}");
   return 200;
}
static void test_benchmark_file_transport(void)
{
   const char *tmp = getenv("TMPDIR");
   if (!tmp || !tmp[0])
      tmp = "/tmp";
   char path[4096];
   int length = snprintf(path, sizeof(path), "%s/aimee-benchmark-transport-XXXXXX", tmp);
   assert(length > 0 && (size_t)length < sizeof(path));
   int fd = mkstemp(path);
   assert(fd >= 0);
   FILE *fp = fdopen(fd, "wb");
   assert(fp);
   const char *raw = "{\"queries\":[{\"query\":\"完整\",\"expected_ids\":[9007199254740993]}]}";
   assert(fwrite(raw, 1, strlen(raw), fp) == strlen(raw));
   assert(fclose(fp) == 0);
   mock_agent_http_set_post_handler(benchmark_post);
   char *reply = kb_client_memory_benchmark_json(cJSON_CreateObject(), path);
   assert(reply && benchmark_posts == 1);
   free(reply);
   fp = fopen(path, "wb");
   assert(fp && fwrite("x\0y", 1, 3, fp) == 3);
   assert(fclose(fp) == 0);
   assert(!kb_client_memory_benchmark_json(cJSON_CreateObject(), path));
   fp = fopen(path, "wb");
   assert(fp);
   for (int i = 0; i < 1048577; i++)
      assert(fputc('x', fp) != EOF);
   assert(fclose(fp) == 0);
   assert(!kb_client_memory_benchmark_json(cJSON_CreateObject(), path));
   assert(unlink(path) == 0);
   assert(!kb_client_memory_benchmark_json(cJSON_CreateObject(), path));
   assert(benchmark_posts == 1);
   mock_agent_http_reset();
}

static int exact_evidence_post(const char *url, const char *auth, const char *body, char **reply,
                               int timeout, const char *headers)
{
   (void)auth;
   (void)timeout;
   (void)headers;
   int outcomes =
       strstr(url, "record_outcome") != NULL || strstr(url, "record_retrieval_outcome") != NULL;
   const char *key = outcomes                           ? "rows"
                     : strstr(url, "ranker.emit_event") ? "doc_ids"
                                                        : "surfaced_ids";
   cJSON *request = cJSON_Parse(body);
   cJSON *ids = cJSON_GetObjectItemCaseSensitive(request, key);
   assert(cJSON_GetArraySize(ids) == 3);
   const char *expected[] = {NULL, "9007199254740993", "9223372036854775807"};
   for (int i = 0; i < 3; i++)
   {
      cJSON *id = cJSON_GetArrayItem(ids, i);
      if (outcomes)
         id = cJSON_GetObjectItemCaseSensitive(id, "id");
      if (i == 0)
         assert(cJSON_IsNumber(id) && id->valuedouble == 42);
      else
         assert(cJSON_IsString(id) && !strcmp(cJSON_GetStringValue(id), expected[i]));
   }
   cJSON_Delete(request);
   *reply = strdup("{\"status\":\"ok\",\"retrieval_event_id\":\"exact-event\",\"written\":3}");
   return 200;
}
static void test_exact_evidence_transport(void)
{
   kb_client_dependency_reset_for_tests();
   mock_agent_http_set_post_handler(exact_evidence_post);
   int64_t ids[] = {42, INT64_C(9007199254740993), INT64_MAX};
   char event_id[64];
   assert(kb_client_evidence_emit_retrieval_event_ex("turn", "Recall", "fp", ids, 3, event_id,
                                                     sizeof(event_id)) == 0);
   assert(!strcmp(event_id, "exact-event"));
   assert(kb_client_ranker_emit_event(ids, 3, "fp", event_id, sizeof(event_id)) == 0);
   assert(!strcmp(event_id, "exact-event"));
   assert(kb_client_record_retrieval_outcome("memory", event_id, ids, 3, "accepted") == 0);
   assert(kb_client_record_retrieval_outcome("ranker", event_id, ids, 3, "accepted") == 0);
   mock_agent_http_reset();
}

int main(void)
{
   test_screen_failures();
   /* A configured kb URL routes kb_client_v1_post_json through agent_http_post
    * (mocked) rather than the unix-socket / spawn path. */
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   test_exact_evidence_transport();
   test_benchmark_file_transport();
   test_generic_action_preserves_budget_auth_and_refusal();
   test_readers_distinguish_unreachable_from_empty();
   test_explicit_scope_overrides_ambient_context();
   test_typed_context_uses_server_defaults();
   test_recall_carries_and_records_production_activation();

   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   printf("test_kb_client_memory: ok\n");
   return 0;
}
