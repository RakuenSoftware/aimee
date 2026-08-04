#include "server_mcp_roundtable.h"
#include "../server/roundtable_review_bus.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_submit_status = 200;
static char g_submit_response[2048];
static char g_submitted_method[64];
static char g_submitted_body[4096];
static uint32_t g_submitted_caps;
static int g_status_code = 200;
static char g_status_response[4096];

int server_http_submit_op_run(const char *method, const char *body, uint32_t capabilities,
                              char *response, int response_n)
{
   snprintf(g_submitted_method, sizeof(g_submitted_method), "%s", method ? method : "");
   snprintf(g_submitted_body, sizeof(g_submitted_body), "%s", body ? body : "");
   g_submitted_caps = capabilities;
   snprintf(response, (size_t)response_n, "%s", g_submit_response);
   return g_submit_status;
}

int route_runs_get(const char *run_id, char *response, int response_n)
{
   (void)run_id;
   snprintf(response, (size_t)response_n, "%s", g_status_response);
   return g_status_code;
}

static cJSON *review_args(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "diff", "a complete implementation artifact");
   cJSON_AddStringToObject(args, "original_request", "implement the requested behavior");
   cJSON_AddStringToObject(args, "artifact_stage", "frozen_diff");
   return args;
}

static void test_submission_returns_before_review_finishes(void)
{
   snprintf(g_submit_response, sizeof(g_submit_response),
            "{\"id\":\"oprun_generation_1\",\"object\":\"op.run\","
            "\"method\":\"roundtable.review\",\"status\":\"queued\"}");
   cJSON *args = review_args();
   char err[256];
   cJSON *run = mcp_roundtable_submit(args, 0x1234u, err, sizeof(err));
   assert(run != NULL && err[0] == '\0');
   assert(strcmp(g_submitted_method, "roundtable.review") == 0);
   assert(g_submitted_caps == 0x1234u);
   cJSON *body = cJSON_Parse(g_submitted_body);
   assert(body != NULL);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "prompt")->valuestring,
                 "a complete implementation artifact") == 0);
   /* Omission stays an omission here. The C->Go proxy resolves the configured
    * or literal saved default immediately before dispatch. */
   assert(cJSON_GetObjectItemCaseSensitive(body, "roundtable") == NULL);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(run, "run_id")->valuestring,
                 "oprun_generation_1") == 0);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(run, "next_tool")->valuestring,
                 "roundtable_status") == 0);
   assert(cJSON_GetObjectItemCaseSensitive(run, "poll_after_ms")->valueint == 1000);
   cJSON_Delete(body);
   cJSON_Delete(run);
   cJSON_Delete(args);
}

static void test_submission_preserves_named_roundtable(void)
{
   snprintf(g_submit_response, sizeof(g_submit_response),
            "{\"id\":\"oprun_generation_2\",\"method\":\"roundtable.review\","
            "\"status\":\"queued\"}");
   cJSON *args = review_args();
   cJSON_AddStringToObject(args, "roundtable", "implementation");
   char err[256];
   cJSON *run = mcp_roundtable_submit(args, 1, err, sizeof(err));
   assert(run != NULL);
   cJSON *body = cJSON_Parse(g_submitted_body);
   assert(body != NULL);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(body, "roundtable")->valuestring,
                 "implementation") == 0);
   cJSON_Delete(body);
   cJSON_Delete(run);
   cJSON_Delete(args);
}

static void test_status_returns_terminal_synthesis(void)
{
   snprintf(g_status_response, sizeof(g_status_response),
            "{\"id\":\"oprun_generation_1\",\"object\":\"op.run\","
            "\"method\":\"roundtable.review\",\"status\":\"completed\","
            "\"result\":{\"approved\":true}}");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "run_id", "oprun_generation_1");
   char err[256];
   cJSON *run = mcp_roundtable_status(args, err, sizeof(err));
   assert(run != NULL && err[0] == '\0');
   assert(cJSON_GetObjectItemCaseSensitive(run, "next_tool") == NULL);
   cJSON *result = cJSON_GetObjectItemCaseSensitive(run, "result");
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "approved")));
   cJSON_Delete(run);
   cJSON_Delete(args);
}

static void test_status_cannot_read_an_unrelated_run(void)
{
   snprintf(g_status_response, sizeof(g_status_response),
            "{\"id\":\"oprun_generation_9\",\"method\":\"kb.build\","
            "\"status\":\"completed\"}");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "run_id", "oprun_generation_9");
   char err[256];
   assert(mcp_roundtable_status(args, err, sizeof(err)) == NULL);
   assert(strstr(err, "does not belong") != NULL);
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
   test_submission_returns_before_review_finishes();
   test_submission_preserves_named_roundtable();
   test_status_returns_terminal_synthesis();
   test_status_cannot_read_an_unrelated_run();
   test_deadline_covers_the_chairman_phase();
   printf("ok\n");
   return 0;
}
