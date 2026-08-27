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
#include "db1_client/user_memory.h"
#include "db1_client/caches.h"
#include "support/mock_agent_http.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int activation_writes;
static int64_t activation_write_id;
static int64_t activation_write_turn;

const char *session_id(void)
{
   return "activation-client-session";
}

int db1_context_snapshot_activation(const char *session_id_arg,
                                    char (*out)[DB1_CONTEXT_ACTIVATION_ROW_LEN], int max)
{
   assert(strcmp(session_id_arg, "activation-client-session") == 0);
   assert(max >= 3);
   snprintf(out[0], DB1_CONTEXT_ACTIVATION_ROW_LEN, "0 7");
   snprintf(out[1], DB1_CONTEXT_ACTIVATION_ROW_LEN, "41 6");
   snprintf(out[2], DB1_CONTEXT_ACTIVATION_ROW_LEN, "52 3");
   return 3;
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

static int activation_recall_post_handler(const char *url, const char *auth_header,
                                          const char *body, char **response_buf, int timeout_ms,
                                          const char *extra_headers)
{
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   assert(url && strstr(url, "/v1/actions/memory.recall") != NULL);
   assert(body && strstr(body, "\"current_turn\":7") != NULL);
   assert(strstr(body, "\"memory_id\":41") != NULL);
   assert(strstr(body, "\"last_turn\":6") != NULL);
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
   mock_agent_http_reset();
   printf("  PASS: test_recall_carries_and_records_production_activation\n");
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
   (void)kb_client_memory_insert("L2", "fact", "scoped-key", "scoped-content", 0.8, NULL, NULL);
   (void)kb_client_memory_find_id_by_key_kind("scoped-key", "fact");
   (void)kb_client_memory_supersede(42, "replacement", 0.9, NULL, NULL);
   (void)kb_client_memory_update_as(42, "replacement", MEMORY_AUTHORITY_MODEL, NULL);
   (void)kb_client_memory_delete_as(42, MEMORY_AUTHORITY_MODEL);
   (void)kb_client_memory_touch(42);
   (void)kb_client_memory_reject(42, "wrong");
   (void)kb_client_memory_restore(42);
   json = kb_client_memory_review_list_json(NULL, 8);
   free(json);
   (void)kb_client_memory_get(42, &mems[0]);

   kb_client_memory_scope_context_clear();
   assert(scoped_request_count == 31);
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

   /* 2. The secret never appears on the wire: either redacted, or withheld with
    *    nothing sent at all. */
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
   printf("  PASS: test_pii_never_reaches_kb\n");
}

/* ---------------------------------------------------------------------------
 * The screen guards EVERY content-carrying write, not just memory.store.
 *
 * memory.store, memory.update and memory.supersede were screened first; the
 * other eleven builders that persist session-authored prose into aimee-kb were
 * not, which is the same defect in a different file. The invariant asserted
 * here is the one that matters and the one that holds for every wrapper
 * whatever its return type: THE SECRET NEVER APPEARS ON THE WIRE. A wrapper
 * may satisfy it by withholding the request entirely or by sending a redacted
 * form -- both are safe, and which one applies depends on whether the
 * classifier could locate the span.
 *
 * A new content-carrying wrapper that forgets the screen fails here.
 */
static void assert_secret_never_sent(const char *what)
{
   if (g_pii_posts > 0 && strstr(g_pii_last_body, "hunter2trustno1") != NULL)
   {
      fprintf(stderr, "FAIL: %s transmitted the secret to aimee-kb: %s\n", what, g_pii_last_body);
      assert(0);
   }
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

static void test_every_content_wrapper_screens(void)
{
   const char *secret = "my password: hunter2trustno1";
   char *json = NULL;
   aimee_task_t task;
   anti_pattern_t ap;
   db2_decision_log_row_t dec;
   int reinforced = 0;

   /* Wrappers returning a response document. */
   PII_CASE("notes.create", json = kb_client_note_create_json("t", secret, "tag", "a"));
   free(json);
   PII_CASE("notes.create(title)", json = kb_client_note_create_json(secret, "c", "tag", "a"));
   free(json);
   PII_CASE("memory.prospective_create",
            json = kb_client_memory_prospective_create_json(secret, "do it", "", "", "", ""));
   free(json);
   PII_CASE("memory.prospective_create(action)",
            json = kb_client_memory_prospective_create_json("when", secret, "", "", "", ""));
   free(json);
   PII_CASE("memory.directive_create", json = kb_client_memory_directive_create_json(
                                           secret, "topic", "", "", "cause", 1, "s", ""));
   free(json);
   PII_CASE("memory.directive_resolve",
            json = kb_client_memory_directive_resolve_json(7, 0, secret));
   free(json);
   PII_CASE("curiosity.create",
            json = kb_client_curiosity_create_json("gap", "", "topic", secret, 1.0, 1.0, "s"));
   free(json);

   /* Wrappers returning a status code. */
   PII_CASE("rules.insert", (void)kb_client_rules_insert("positive", "title", secret, 1));
   PII_CASE("feedback.record",
            (void)kb_client_feedback_record("positive", "title", secret, 1, &reinforced));
   PII_CASE("anti_pattern.insert",
            (void)kb_client_anti_pattern_insert(secret, "desc", "src", "ref", 1.0, &ap));
   PII_CASE("decision_log.insert",
            (void)kb_client_decision_log_insert(1, "opts", "chosen", secret, "assume", &dec));
   PII_CASE("collab_rules.propose", (void)kb_client_collab_rules_propose(secret, "why", "me"));
   PII_CASE("task.create", (void)kb_client_task_create(secret, "s", 0, &task));
   PII_CASE("memory.upsert_workflow",
            (void)kb_client_memory_upsert_workflow("ws", "sig", secret, 1.0, "s"));
   PII_CASE("memory.reject", (void)kb_client_memory_reject(42, secret));

   /* And the screen must not have turned these into blanket refusals: clean
    * content still reaches the kb. */
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(recording_post_handler);
   g_pii_posts = 0;
   g_pii_last_body[0] = '\0';
   json = kb_client_note_create_json("title", "the sky is blue", "tag", "a");
   free(json);
   assert(g_pii_posts == 1);
   assert(strstr(g_pii_last_body, "the sky is blue") != NULL);

   mock_agent_http_reset();
   printf("  PASS: test_every_content_wrapper_screens\n");
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
   test_recall_carries_and_records_production_activation();
   test_as_of_reaches_the_kb_and_its_verdict_comes_back();
   test_pii_never_reaches_kb();
   test_every_content_wrapper_screens();

   unsetenv("AIMEE_KB_API_URL");
   runtime_secret_remove("AIMEE_KB_API_BEARER_TOKEN");
   printf("test_kb_client_memory: ok\n");
   return 0;
}
