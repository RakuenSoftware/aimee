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
#include "runtime_secret.h"
#include "db1/user_memory.h"
#include "support/mock_agent_http.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db1_user_memory_any(void)
{
   return 0;
}

void db1_user_memory_merge_into_array(cJSON *arr, db1_user_recall_section_t section,
                                      const char *why)
{
   (void)arr;
   (void)section;
   (void)why;
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

static int single_miss_post_handler(const char *url, const char *auth_header, const char *body,
                                    char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = strdup("{\"status\":\"error\",\"message\":\"record not found\"}");
   return 200;
}

static int scoped_request_count;

static int scoped_ok_post_handler(const char *url, const char *auth_header, const char *body,
                                  char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   assert(body != NULL);
   assert(strstr(body, "\"scope_context\":true") != NULL);
   assert(strstr(body, "\"workspace\":\"active-workspace\"") != NULL);
   assert(strstr(body, "\"project\":\"active-project\"") != NULL);
   assert(strstr(body, "\"include_all\":false") != NULL);
   scoped_request_count++;
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"memories\":[],\"facts\":[],\"results\":[],"
                             "\"conflicts\":[],\"edges\":[],\"relations\":[],\"links\":[],"
                             "\"context\":\"\",\"block\":\"\",\"answer\":\"\","
                             "\"citations\":[]}");
   return 200;
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
   if (response_buf)
      *response_buf =
          strdup("{\"status\":\"ok\",\"used_tokens\":4,\"rendered_context\":\"temporal\"}");
   return 200;
}

static void test_readers_distinguish_unreachable_from_empty(void)
{
   memory_t mems[8];
   search_result_t windows[8];
   conflict_t conflicts[8];
   char *clusters[] = {"hello"};

   /* --- kb unreachable: every count-returning reader reports < 0 --- */
   kb_client_dependency_reset_for_tests();
   mock_agent_http_set_post_handler(unreachable_post_handler);
   assert(kb_client_memory_list(NULL, NULL, 8, mems, 8) < 0);
   assert(kb_client_memory_find_facts("q", 8, mems, 8) < 0);
   assert(kb_client_memory_find_facts_scoped("q", NULL, NULL, 8, mems, 8) < 0);
   assert(kb_client_memory_find_facts_visible("q", NULL, NULL, 8, mems, 8) < 0);
   assert(kb_client_memory_search(clusters, 1, 8, windows, 8) < 0);
   assert(kb_client_memory_list_conflicts(conflicts, 8) < 0);

   /* --- healthy but empty: same readers report exactly 0 (not < 0) --- */
   /* Model dependency recovery between the two independent fixtures. Without
    * this reset the intentionally opened process breaker correctly suppresses
    * the healthy handler, which would test cooldown rather than empty-result
    * classification. Breaker recovery itself is covered by kb-client-search. */
   kb_client_dependency_reset_for_tests();
   mock_agent_http_set_post_handler(empty_ok_post_handler);
   assert(kb_client_memory_list(NULL, NULL, 8, mems, 8) == 0);
   assert(kb_client_memory_find_facts("q", 8, mems, 8) == 0);
   assert(kb_client_memory_find_facts_scoped("q", NULL, NULL, 8, mems, 8) == 0);
   assert(kb_client_memory_find_facts_visible("q", NULL, NULL, 8, mems, 8) == 0);
   assert(kb_client_memory_search(clusters, 1, 8, windows, 8) == 0);
   assert(kb_client_memory_list_conflicts(conflicts, 8) == 0);

   mock_agent_http_reset();
   printf("  PASS: test_readers_distinguish_unreachable_from_empty\n");
}

static void test_ordered_readers_propagate_active_project_context(void)
{
   memory_t mems[8];
   memory_diagnostic_t diagnostics[2];
   memory_relation_t relations[8];
   memory_entity_profile_t profile;
   memory_answer_result_t answer;

   scoped_request_count = 0;
   mock_agent_http_set_post_handler(scoped_ok_post_handler);
   kb_client_memory_scope_context_set("active-workspace", "active-project", 0);

   (void)kb_client_memory_find_facts("q", 8, mems, 8);
   (void)kb_client_memory_find_facts_ex("q", 8, mems, 8, "on");
   (void)kb_client_memory_list(NULL, NULL, 8, mems, 8);
   char *clusters[] = {"q"};
   search_result_t windows[2];
   (void)kb_client_memory_search(clusters, 1, 2, windows, 2);
   (void)kb_client_memory_find_facts_visible("q", NULL, NULL, 8, mems, 8);
   char *json = kb_client_memory_assemble_context("q");
   free(json);
   json = kb_client_memory_assemble_typed_context("q");
   free(json);
   json = kb_client_memory_recall_json("q", 128, 0);
   free(json);
   json = kb_client_memory_alerts_json(NULL);
   free(json);
   cJSON *briefing = kb_client_memory_briefing(128);
   cJSON_Delete(briefing);
   (void)kb_client_memory_get_entity_profile("entity", &profile);
   (void)kb_client_memory_get_entity_edges("entity", 8, relations, 8);
   (void)kb_client_memory_search_graph("entity", 8, relations, 8);
   (void)kb_client_memory_search_graph_as_of("entity", "2026-07-29", 8, relations, 8);
   (void)kb_client_memory_ask("q", NULL, NULL, 8, &answer);
   json = kb_client_memory_context_block("q", "general", 8);
   free(json);
   (void)kb_client_memory_diagnose("q", 2, diagnostics, 2);
   json = kb_client_memory_facts("q");
   free(json);
   (void)kb_client_memory_top_l2_facts(mems, 8);
   (void)kb_client_memory_list_session_scope_priority(mems, 8);
   (void)kb_client_memory_list_session_scope_priority_like("%q%", mems, 8);

   kb_client_memory_scope_context_clear();
   assert(scoped_request_count == 21);
   mock_agent_http_reset();
   printf("  PASS: test_ordered_readers_propagate_active_project_context\n");
}

static void test_single_record_miss_is_not_dependency_failure(void)
{
   memory_t memory;
   memory_entity_profile_t profile;
   memory_episode_t episode;

   kb_client_dependency_reset_for_tests();
   mock_agent_http_set_post_handler(single_miss_post_handler);
   assert(kb_client_memory_get(42, &memory) == 1);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_EMPTY);
   assert(kb_client_memory_get_entity_profile("missing", &profile) == 1);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_EMPTY);
   assert(kb_client_memory_get_episode("missing", &episode) == 1);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_EMPTY);

   kb_client_dependency_reset_for_tests();
   mock_agent_http_set_post_handler(unreachable_post_handler);
   assert(kb_client_memory_get(42, &memory) < 0);
   assert(kb_client_last_result_status() == KB_CLIENT_RESULT_UNAVAILABLE);

   mock_agent_http_reset();
   printf("  PASS: test_single_record_miss_is_not_dependency_failure\n");
}

static void test_explicit_scope_overrides_ambient_context(void)
{
   memory_t mems[2];
   mock_agent_http_set_post_handler(explicit_scope_post_handler);
   kb_client_memory_scope_context_set("active-workspace", "active-project", 0);
   assert(kb_client_memory_find_facts_visible("q", "explicit-workspace", "explicit-project", 2,
                                              mems, 2) == 0);
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
   mock_agent_http_reset();
   printf("  PASS: test_typed_context_uses_server_defaults\n");
}

/* ---------------------------------------------------------------------------
 * PII must never cross to aimee-kb.
 *
 * The assertion that matters is NOT "the call returned an error" -- it is that
 * NO REQUEST WAS ISSUED. A gate that lets the POST happen and then reports a
 * rejection has already moved the data it exists to contain. g_pii_posts counts
 * transmissions; for withheld content it must stay at zero.
 */
static int g_pii_posts;
static char g_pii_last_body[4096];

static int recording_post_handler(const char *url, const char *auth_header, const char *body,
                                  char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   g_pii_posts++;
   snprintf(g_pii_last_body, sizeof(g_pii_last_body), "%s", body ? body : "");
   if (response_buf)
      *response_buf = strdup("{\"status\":\"ok\",\"memory\":{\"id\":7}}");
   return 200;
}

static void test_pii_never_reaches_kb(void)
{
   const char *secret = "my password: hunter2trustno1";

   /* 1. Clean content is transmitted unchanged. */
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(recording_post_handler);
   g_pii_posts = 0;
   g_pii_last_body[0] = '\0';
   assert(kb_client_memory_insert(TIER_L1, KIND_FACT, "k-clean", "the sky is blue", 1.0, "s",
                                  NULL) == 0);
   assert(g_pii_posts == 1);
   assert(strstr(g_pii_last_body, "the sky is blue") != NULL);

   /* 2. Redactable content crosses only in redacted form: the secret itself is
    *    absent from the wire, and the marker is present. */
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(recording_post_handler);
   g_pii_posts = 0;
   g_pii_last_body[0] = '\0';
   int rc = kb_client_memory_insert(TIER_L1, KIND_FACT, "k-secret", secret, 1.0, "s", NULL);
   if (rc == 0)
   {
      assert(g_pii_posts == 1);
      assert(strstr(g_pii_last_body, "hunter2trustno1") == NULL);
      assert(strstr(g_pii_last_body, "REDACTED") != NULL);
   }
   else
   {
      /* Unredactable -> withheld outright, and nothing was sent. */
      assert(rc == KB_CLIENT_MEMORY_WITHHELD_PII);
      assert(g_pii_posts == 0);
   }

   /* 3. A sensitive KEY withholds the whole write: the key is the lookup handle
    *    and cannot be redacted in place. Nothing is transmitted. */
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(recording_post_handler);
   g_pii_posts = 0;
   rc = kb_client_memory_insert(TIER_L1, KIND_FACT, secret, "benign body", 1.0, "s", NULL);
   assert(rc == KB_CLIENT_MEMORY_WITHHELD_PII);
   assert(g_pii_posts == 0);

   /* 4. The same screen guards update and supersede, not just insert. */
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(recording_post_handler);
   g_pii_posts = 0;
   g_pii_last_body[0] = '\0';
   rc = kb_client_memory_update(42, secret);
   if (rc == KB_CLIENT_MEMORY_WITHHELD_PII)
      assert(g_pii_posts == 0);
   else
      assert(strstr(g_pii_last_body, "hunter2trustno1") == NULL);

   mock_agent_http_reset();
   mock_agent_http_set_post_handler(recording_post_handler);
   g_pii_posts = 0;
   g_pii_last_body[0] = '\0';
   rc = kb_client_memory_supersede(42, secret, 1.0, "s", NULL);
   if (rc == KB_CLIENT_MEMORY_WITHHELD_PII)
      assert(g_pii_posts == 0);
   else
      assert(strstr(g_pii_last_body, "hunter2trustno1") == NULL);

   mock_agent_http_reset();
}

int main(void)
{
   /* A configured kb URL routes kb_client_v1_post_json through agent_http_post
    * (mocked) rather than the unix-socket / spawn path. */
   assert(setenv("AIMEE_KB_API_URL", "http://127.0.0.1:4010/", 1) == 0);
   assert(runtime_secret_store("AIMEE_KB_API_BEARER_TOKEN", "test-token") == 0);

   test_readers_distinguish_unreachable_from_empty();
   test_single_record_miss_is_not_dependency_failure();
   test_ordered_readers_propagate_active_project_context();
   test_explicit_scope_overrides_ambient_context();
   test_typed_context_uses_server_defaults();
   test_pii_never_reaches_kb();

   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   printf("test_kb_client_memory: ok\n");
   return 0;
}
