/* test_session_search_tool.c: zero-LLM session_search MCP result builder */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "db1.h"
#include "session_search_tool.h"

static void seed_sessions(void)
{
   assert(db1_primary_session_save(
              "sid-alpha", "codex", "chatgpt",
              "[{\"role\":\"user\",\"content\":\"Configure the reverse proxy on port 8443\"},"
              "{\"role\":\"assistant\",\"content\":\"Use nginx with a local upstream\"},"
              "{\"role\":\"user\",\"content\":\"Record the final decision\"}]") == 0);
   assert(db1_primary_session_save(
              "sid-beta", "codex", "chatgpt",
              "[{\"role\":\"user\",\"content\":\"Investigate delegate queue latency\"},"
              "{\"role\":\"assistant\",\"content\":\"The local model slot is saturated\"}]") == 0);
   assert(db1_server_session_create("sid-hidden", "mcp", "test-principal") == 0);
   assert(db1_primary_session_save(
              "sid-hidden", "codex", "chatgpt",
              "[{\"role\":\"user\",\"content\":\"hidden-source-token should be filtered\"}]") == 0);
   assert(db1_server_session_create("sid-other", "cli", "uid:9999") == 0);
   assert(db1_primary_session_save(
              "sid-other", "codex", "chatgpt",
              "[{\"role\":\"user\",\"content\":\"other-operator-token should be scoped\"}]") == 0);
}

static cJSON *call_tool(cJSON *args)
{
   cJSON *res = session_search_tool_result(args);
   assert(cJSON_IsObject(res));
   return res;
}

static void test_browse_recent_sessions(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddNumberToObject(args, "limit", 5);
   cJSON *res = call_tool(args);
   cJSON *mode = cJSON_GetObjectItemCaseSensitive(res, "mode");
   cJSON *sessions = cJSON_GetObjectItemCaseSensitive(res, "sessions");
   assert(cJSON_IsString(mode) && strcmp(mode->valuestring, "browse") == 0);
   assert(cJSON_IsArray(sessions));
   assert(cJSON_GetArraySize(sessions) >= 2);
   cJSON *first = cJSON_GetArrayItem(sessions, 0);
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(first, "session_id")));
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(first, "preview")));
   cJSON_Delete(res);
   cJSON_Delete(args);
   printf("  PASS: test_browse_recent_sessions\n");
}

static void test_discovery_returns_window_and_bookends(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "query", "reverse proxy");
   cJSON_AddNumberToObject(args, "window", 1);
   cJSON *res = call_tool(args);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(res, "mode")->valuestring, "discovery") == 0);
   cJSON *sessions = cJSON_GetObjectItemCaseSensitive(res, "sessions");
   assert(cJSON_IsArray(sessions));
   assert(cJSON_GetArraySize(sessions) >= 1);
   cJSON *hit = cJSON_GetArrayItem(sessions, 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(hit, "session_id")->valuestring, "sid-alpha") ==
          0);
   assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(hit, "match")));
   assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(hit, "window")));
   assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(hit, "bookend_start")));
   assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(hit, "bookend_end")));
   cJSON_Delete(res);
   cJSON_Delete(args);
   printf("  PASS: test_discovery_returns_window_and_bookends\n");
}

static void test_scroll_returns_anchor_window(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "session_id", "sid-alpha");
   cJSON_AddNumberToObject(args, "around_message_id", 1);
   cJSON_AddNumberToObject(args, "window", 1);
   cJSON *res = call_tool(args);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(res, "mode")->valuestring, "scroll") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "found")));
   cJSON *window = cJSON_GetObjectItemCaseSensitive(res, "window");
   assert(cJSON_IsArray(window));
   assert(cJSON_GetArraySize(window) == 3);
   cJSON *middle = cJSON_GetArrayItem(window, 1);
   assert(cJSON_GetObjectItemCaseSensitive(middle, "message_id")->valueint == 1);
   cJSON_Delete(res);
   cJSON_Delete(args);
   printf("  PASS: test_scroll_returns_anchor_window\n");
}

static void test_hidden_sources_excluded_unless_included(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "query", "hidden-source-token");
   cJSON *res = call_tool(args);
   assert(cJSON_GetObjectItemCaseSensitive(res, "count")->valueint == 0);
   cJSON_Delete(res);

   cJSON *include = cJSON_AddArrayToObject(args, "include_sources");
   cJSON_AddItemToArray(include, cJSON_CreateString("mcp"));
   res = call_tool(args);
   assert(cJSON_GetObjectItemCaseSensitive(res, "count")->valueint == 1);
   cJSON_Delete(res);
   cJSON_Delete(args);
   printf("  PASS: test_hidden_sources_excluded_unless_included\n");
}

static void test_principal_scope_excludes_other_operator(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "query", "other-operator-token");
   cJSON_AddStringToObject(args, "_principal", "uid:1000");
   cJSON *res = call_tool(args);
   assert(cJSON_GetObjectItemCaseSensitive(res, "count")->valueint == 0);
   cJSON_Delete(res);
   cJSON_Delete(args);
   printf("  PASS: test_principal_scope_excludes_other_operator\n");
}

/* Regression: a session can be returned by the DB-level search because the query
 * matched session_id / agent_name / provider (or structured content that
 * message_text cannot read) while NO message body actually contains it. In that
 * case the tool must not report message 0 as "the match". */
static void test_no_message_match_reports_no_match(void)
{
   cJSON *args = cJSON_CreateObject();
   /* "chatgpt" is the provider of every seeded session but appears in no message. */
   cJSON_AddStringToObject(args, "query", "chatgpt");
   cJSON *res = call_tool(args);
   cJSON *sessions = cJSON_GetObjectItemCaseSensitive(res, "sessions");
   assert(cJSON_IsArray(sessions));
   assert(cJSON_GetArraySize(sessions) > 0);
   cJSON *s0 = cJSON_GetArrayItem(sessions, 0);
   cJSON *match = cJSON_GetObjectItemCaseSensitive(s0, "match");
   int mid = cJSON_GetObjectItemCaseSensitive(match, "message_id")->valueint;
   const char *snip = cJSON_GetObjectItemCaseSensitive(match, "snippet")->valuestring;
   printf("    [diag] provider-only query -> message_id=%d snippet=\"%.40s\"\n", mid, snip);
   assert(mid == -1);
   assert(snip[0] == 0);
   cJSON_Delete(res);
   cJSON_Delete(args);
   printf("  PASS: test_no_message_match_reports_no_match\n");
}

static void test_persona_delivery_tracks_durable_session_lifetime(void)
{
   const char *sid = "sid-persona-delivery";
   assert(db1_server_session_create(sid, "gateway", "test-principal") == 0);
   assert(db1_server_session_persona_delivery_claim(sid) == 1);
   assert(db1_server_session_persona_delivery_claim(sid) == 0); /* reserved */
   assert(db1_server_session_persona_delivery_finish(sid, 0) == 0);
   assert(db1_server_session_persona_delivery_claim(sid) == 1); /* retry */
   assert(db1_server_session_persona_delivery_finish(sid, 1) == 0);
   assert(db1_server_session_persona_delivery_claim(sid) == 0); /* committed */

   /* Normal session expiration/deletion removes the delivery record with it;
    * recreating the same id is a genuinely new session and may deliver once. */
   assert(db1_server_session_delete(sid) == 0);
   assert(db1_server_session_create(sid, "gateway", "test-principal") == 0);
   assert(db1_server_session_persona_delivery_claim(sid) == 1);
   assert(db1_server_session_persona_delivery_finish(sid, 1) == 0);
   printf("  PASS: test_persona_delivery_tracks_durable_session_lifetime\n");
}

int main(void)
{
   assert(db1_init(":memory:") == 0);
   seed_sessions();
   test_browse_recent_sessions();
   test_discovery_returns_window_and_bookends();
   test_scroll_returns_anchor_window();
   test_hidden_sources_excluded_unless_included();
   test_principal_scope_excludes_other_operator();
   test_no_message_match_reports_no_match();
   test_persona_delivery_tracks_durable_session_lifetime();
   printf("session_search_tool: all tests passed\n");
   return 0;
}
