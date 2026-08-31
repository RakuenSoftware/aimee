#include "server_mcp_roundtable.h"
#include "../server/roundtable_review_bus.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_rpc_status = 200;
static char g_rpc_response[4096];
static char g_dispatched_body[4096];
static uint32_t g_dispatched_caps;
static int g_rpc_calls;

int loopback_rpc(const char *body, int body_len, char *resp, int resp_cap, uint32_t conn_caps)
{
   g_rpc_calls++;
   snprintf(g_dispatched_body, sizeof(g_dispatched_body), "%.*s", body_len, body ? body : "");
   g_dispatched_caps = conn_caps;
   snprintf(resp, (size_t)resp_cap, "%s", g_rpc_response);
   return g_rpc_status;
}

uint32_t server_capability_for_method(const char *method)
{
   return strcmp(method, "roundtable.review") == 0 ? 0x10u : 0u;
}

static cJSON *review_args(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "diff", "a complete implementation artifact");
   cJSON_AddStringToObject(args, "original_request", "implement the requested behavior");
   cJSON_AddStringToObject(args, "artifact_stage", "frozen_diff");
   return args;
}

/* The whole point of the change: one call in, a verdict out. The caller blocks
 * on aimee-server, which blocks on the bus, which blocks on the model -- so
 * there is no run id to hand back and nothing for the caller to poll. */
static void test_review_returns_the_verdict(void)
{
   g_rpc_status = 200;
   g_rpc_calls = 0;
   snprintf(g_rpc_response, sizeof(g_rpc_response),
            "{\"approved\":true,\"findings\":[],\"chair\":\"synthesis\"}");
   cJSON *args = review_args();
   char err[256];
   cJSON *verdict = mcp_roundtable_review(args, 0x1234u, err, sizeof(err));
   assert(verdict != NULL && err[0] == '\0');
   assert(g_rpc_calls == 1);
   assert(g_dispatched_caps == 0x1234u);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(verdict, "approved")));

   cJSON *body = cJSON_Parse(g_dispatched_body);
   assert(body != NULL);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "method")->valuestring,
                 "roundtable.review") == 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "prompt")->valuestring,
                 "a complete implementation artifact") == 0);
   /* Omission stays an omission here; the review resolves the configured or
    * literal saved default immediately before dispatch. */
   assert(cJSON_GetObjectItemCaseSensitive(body, "roundtable") == NULL);

   /* No poll contract, in either direction. Advertising one is what taught the
    * caller to spend a model turn per second on a job that runs for minutes. */
   assert(cJSON_GetObjectItemCaseSensitive(verdict, "next_tool") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(verdict, "poll_after_ms") == NULL);
   assert(cJSON_GetObjectItemCaseSensitive(verdict, "run_id") == NULL);
   cJSON_Delete(body);
   cJSON_Delete(verdict);
   cJSON_Delete(args);
}

static void test_review_preserves_named_roundtable(void)
{
   g_rpc_status = 200;
   snprintf(g_rpc_response, sizeof(g_rpc_response), "{\"approved\":false}");
   cJSON *args = review_args();
   cJSON_AddStringToObject(args, "roundtable", "implementation");
   char err[256];
   cJSON *verdict = mcp_roundtable_review(args, 0x10u, err, sizeof(err));
   assert(verdict != NULL);
   cJSON *body = cJSON_Parse(g_dispatched_body);
   assert(body != NULL);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "roundtable")->valuestring,
                 "implementation") == 0);
   cJSON_Delete(body);
   cJSON_Delete(verdict);
   cJSON_Delete(args);
}

/* A dispatch can answer 2xx carrying an error object. That is a failed review,
 * not a verdict, and returning it as one would report an unreviewed change as
 * reviewed. */
static void test_review_rejects_an_error_payload(void)
{
   g_rpc_status = 200;
   snprintf(g_rpc_response, sizeof(g_rpc_response),
            "{\"error\":{\"message\":\"roundtable review failed: DEADLINE\"}}");
   cJSON *args = review_args();
   char err[256];
   assert(mcp_roundtable_review(args, 0x10u, err, sizeof(err)) == NULL);
   assert(strstr(err, "DEADLINE") != NULL);
   cJSON_Delete(args);
}

static void test_review_reports_a_dispatch_failure(void)
{
   g_rpc_status = 503;
   snprintf(g_rpc_response, sizeof(g_rpc_response),
            "{\"error\":\"roundtable review module is not attached to the event bus\"}");
   cJSON *args = review_args();
   char err[256];
   assert(mcp_roundtable_review(args, 0x10u, err, sizeof(err)) == NULL);
   assert(strstr(err, "not attached") != NULL);
   cJSON_Delete(args);
}

/* A refusal must read as a refusal rather than as a failure from inside the
 * review, and must not reach the bus at all. */
static void test_review_preflights_the_capability(void)
{
   g_rpc_calls = 0;
   cJSON *args = review_args();
   char err[256];
   assert(mcp_roundtable_review(args, 0x1u, err, sizeof(err)) == NULL);
   assert(strstr(err, "insufficient capabilities") != NULL);
   assert(g_rpc_calls == 0);
   cJSON_Delete(args);
}

/* The chairman is a separate turn with its own full phase deadline, so the call
 * must cover analysis plus chairman plus a serialization grace. A single-phase
 * deadline starved it whenever the seats ran long. */
static void test_deadline_covers_the_chairman_phase(void)
{
   assert(roundtable_review_deadline_ms(600000, 0) == 630000);
   assert(roundtable_review_deadline_ms(600000, 1) == 1230000);
   assert(roundtable_review_deadline_ms(INT_MAX, 1) == INT_MAX);
}

int main(void)
{
   printf("server_mcp_roundtable: ");
   test_review_returns_the_verdict();
   test_review_preserves_named_roundtable();
   test_review_rejects_an_error_payload();
   test_review_reports_a_dispatch_failure();
   test_review_preflights_the_capability();
   test_deadline_covers_the_chairman_phase();
   printf("ok\n");
   return 0;
}
