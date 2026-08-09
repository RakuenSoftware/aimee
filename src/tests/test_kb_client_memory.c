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
   assert(scoped_request_count == 20);
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

/* `memory get --as-of` was marshalled by the client, accepted by aimee-kb, and
 * lost in between: kb_client_memory_get sent a request carrying nothing but the
 * id. Every test around it passed anyway, because each end was checked against a
 * payload written by hand to contain the field -- the CLI marshaller was asserted
 * to emit as_of, the printer was fed a synthetic reply that already had valid_at,
 * and the DB2 primitive was called directly. Three green pieces that never
 * touched each other.
 *
 * So this asserts the SERIALIZED REQUEST BODY, which is the thing that was
 * actually empty. A test that only checked the return value would pass against
 * the broken version. */
static char last_request_body[1024];
static const char *as_of_reply = NULL;

static int as_of_post_handler(const char *url, const char *auth_header, const char *body,
                              char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   snprintf(last_request_body, sizeof(last_request_body), "%s", body ? body : "");
   if (response_buf)
      *response_buf = strdup(as_of_reply);
   return 200;
}

static void test_as_of_reaches_the_kb_and_its_verdict_comes_back(void)
{
   memory_t m;
   kb_valid_at_t verdict;

   mock_agent_http_set_post_handler(as_of_post_handler);
   as_of_reply = "{\"status\":\"ok\",\"as_of\":\"2026-06-12 00:00:00\",\"valid_at\":false,"
                 "\"memory\":{\"id\":42,\"key\":\"k\",\"content\":\"c\"}}";

   /* The field must be ON THE WIRE. This is the assertion that fails against the
    * bug: the old request body was {"id":42} and nothing else. */
   last_request_body[0] = '\0';
   assert(kb_client_memory_get_as_of(42, "2026-06-12 00:00:00", &m, &verdict) == 0);
   assert(strstr(last_request_body, "\"as_of\"") != NULL);
   assert(strstr(last_request_body, "2026-06-12 00:00:00") != NULL);
   assert(verdict == KB_VALID_AT_NO);

   /* "unknown" must survive as a third answer. Folding it into NO would report
    * "not in force" for a row the service could not judge. */
   as_of_reply = "{\"status\":\"ok\",\"as_of\":\"2026-06-12 00:00:00\",\"valid_at\":\"unknown\","
                 "\"memory\":{\"id\":42,\"key\":\"k\",\"content\":\"c\"}}";
   assert(kb_client_memory_get_as_of(42, "2026-06-12 00:00:00", &m, &verdict) == 0);
   assert(verdict == KB_VALID_AT_UNKNOWN);

   as_of_reply = "{\"status\":\"ok\",\"as_of\":\"2026-06-12 00:00:00\",\"valid_at\":true,"
                 "\"memory\":{\"id\":42,\"key\":\"k\",\"content\":\"c\"}}";
   assert(kb_client_memory_get_as_of(42, "2026-06-12 00:00:00", &m, &verdict) == 0);
   assert(verdict == KB_VALID_AT_YES);

   /* Not asking must send no as_of at all: aimee-kb emits the verdict exactly
    * when it receives a non-empty as_of, so an empty string here would be
    * indistinguishable from asking. */
   as_of_reply = "{\"status\":\"ok\",\"memory\":{\"id\":42,\"key\":\"k\",\"content\":\"c\"}}";
   last_request_body[0] = '\0';
   assert(kb_client_memory_get_as_of(42, NULL, &m, &verdict) == 0);
   assert(strstr(last_request_body, "as_of") == NULL);
   assert(verdict == KB_VALID_AT_UNASKED);

   last_request_body[0] = '\0';
   assert(kb_client_memory_get_as_of(42, "", &m, &verdict) == 0);
   assert(strstr(last_request_body, "as_of") == NULL);
   assert(verdict == KB_VALID_AT_UNASKED);

   /* The plain entry point keeps its six existing callers' behaviour: no as_of
    * on the wire, and it must still compile against a NULL verdict. */
   last_request_body[0] = '\0';
   assert(kb_client_memory_get(42, &m) == 0);
   assert(strstr(last_request_body, "as_of") == NULL);

   mock_agent_http_reset();
   printf("  PASS: test_as_of_reaches_the_kb_and_its_verdict_comes_back\n");
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
   test_as_of_reaches_the_kb_and_its_verdict_comes_back();

   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   printf("test_kb_client_memory: ok\n");
   return 0;
}
