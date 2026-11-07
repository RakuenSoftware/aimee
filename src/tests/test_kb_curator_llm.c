/* Canonical curator model boundary and terminal text-projection regressions. */
#include "kb_curator_llm.h"
#include "support/curator_config_stub.h"

#include "cJSON.h"
#include "config.h"
#include "support/mock_agent_http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_seen_url[512];
static char g_seen_body[1024];
static int g_seen_timeout_ms;
static int g_post_calls;

static int ok_handler(const char *url, const char *auth_header, const char *body,
                      char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)auth_header;
   (void)extra_headers;
   g_seen_timeout_ms = timeout_ms;
   g_post_calls++;
   snprintf(g_seen_url, sizeof(g_seen_url), "%s", url ? url : "");
   snprintf(g_seen_body, sizeof(g_seen_body), "%s", body ? body : "");
   if (response_buf)
      *response_buf = strdup("{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"content\":"
                             "\"{\\\"synthesis\\\":\\\"ok\\\"}\"}}]}");
   return 200;
}

static int partial_handler(const char *url, const char *auth, const char *body, char **reply,
                           int timeout, const char *headers)
{
   int rc = ok_handler(url, auth, body, reply, timeout, headers);
   free(*reply);
   *reply = strdup("{\"model\":\"partial-model\",\"choices\":[{\"finish_reason\":\"length\","
                   "\"message\":{\"content\":\"{\\\"synthesis\\\":\\\"partial\\\"}\"}}],\"usage\":{"
                   "\"prompt_tokens\":31,\"completion_tokens\":7}}");
   return rc;
}

static int network_error_handler(const char *url, const char *auth_header, const char *body,
                                 char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   (void)extra_headers;
   g_post_calls++;
   return -1;
}

static int err_handler(const char *url, const char *auth_header, const char *body,
                       char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)timeout_ms;
   (void)extra_headers;
   if (response_buf)
      *response_buf = strdup("{\"error\":\"boom\"}");
   return 400; /* non-retryable client error -> provider_client fails fast */
}

static void configure(void)
{
   kb_curator_provider_backoff_recovered();
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(ok_handler);
   memset(&cfg, 0, sizeof cfg);
   cfg.kb_curator_extract_max_tokens = 137;
   snprintf(cfg.kb_curator_provider_base_url, sizeof cfg.kb_curator_provider_base_url,
            "http://fixture/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof cfg.kb_curator_provider_model, "fixture-model");
   g_post_calls = 0;
}
static void test_owned_request_and_response(void)
{
   configure();
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[256] = "";
   char *job = strdup("{\"topic\":\"t\"}");
   assert(!kb_curator_llm_request_init(&request, "curator policy", job, NULL));
   free(job);
   assert(!aimee_ir_request_admit(&request));
   assert(request.system[0].context.origin == AIMEE_CTX_ORIGIN_PLATFORM);
   assert(request.messages[0].blocks[0].context.origin == AIMEE_CTX_ORIGIN_RETRIEVAL);
   assert(
       !kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, &request, &response, error, sizeof error));
   assert(response.stop_reason == AIMEE_STOP_END_TURN);
   char *text = kb_curator_llm_text(&response, error, sizeof error);
   assert(text && !strcmp(text, "{\"synthesis\":\"ok\"}"));
   assert(g_post_calls == 1 && g_seen_timeout_ms == 600000);
   assert(!strcmp(g_seen_url, "http://fixture/v1/chat/completions"));
   cJSON *sent = cJSON_Parse(g_seen_body);
   assert(sent && cJSON_GetObjectItemCaseSensitive(sent, "max_tokens")->valueint == 137);
   cJSON_Delete(sent);
   free(text);
   aimee_response_free(&response);
   /* An unadmitted input must not reach provider transport. */
   request.n_tools = AIMEE_IR_MAX_BLOCKS + 1;
   assert(kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, &request, &response, error,
                             sizeof error) < 0);
   assert(g_post_calls == 1);
   request.n_tools = 0;
   aimee_request_free(&request);
}
static void test_projection_requires_terminal_authored_text(void)
{
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_THINKING, .text = "private reasoning"},
                             {.type = AIMEE_BLK_TEXT, .text = "{\"synthesis\":\"answer\"}"}};
   aimee_response_t response = {.role = "assistant",
                                .content = blocks,
                                .n_content = 2,
                                .stop_reason = AIMEE_STOP_END_TURN,
                                .model = "retained-model",
                                .usage_in = 31,
                                .usage_out = 7,
                                .raw_stop_reason = "stop"};
   char error[256];
   char *text = kb_curator_llm_text(&response, error, sizeof error);
   assert(text && !strcmp(text, blocks[1].text) && !strstr(text, "private"));
   free(text);
   assert(response.content == blocks && response.usage_in == 31 &&
          !strcmp(response.model, "retained-model"));
   response.stop_reason = AIMEE_STOP_STOP_SEQUENCE;
   text = kb_curator_llm_text(&response, error, sizeof error);
   assert(text);
   free(text);
   const aimee_stop_reason_t failures[] = {AIMEE_STOP_MAX_TOKENS, AIMEE_STOP_TOOL_USE,
                                           AIMEE_STOP_CONTENT_FILTER, AIMEE_STOP_ERROR,
                                           AIMEE_STOP_UNKNOWN};
   for (size_t i = 0; i < sizeof failures / sizeof *failures; i++)
   {
      response.stop_reason = failures[i];
      assert(!kb_curator_llm_text(&response, error, sizeof error));
      assert(response.content == blocks && response.stop_reason == failures[i]);
   }
   response.stop_reason = AIMEE_STOP_END_TURN;
   response.n_content = 1;
   assert(!kb_curator_llm_text(&response, error, sizeof error)); /* never reasoning-as-answer */
   response.n_content = 2;
   response.error_message = "provider error";
   assert(!kb_curator_llm_text(&response, error, sizeof error));
   response.error_message = NULL;
   blocks[0].type = AIMEE_BLK_TOOL_USE;
   blocks[0].tool_id = "pending";
   blocks[0].tool_name = "write_file";
   blocks[0].tool_input = cJSON_CreateObject();
   assert(!kb_curator_llm_text(&response, error, sizeof error));
   cJSON_Delete(blocks[0].tool_input);
}
static void test_actual_partial_response_retained_not_projected(void)
{
   configure();
   mock_agent_http_set_post_handler(partial_handler);
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[256] = "";
   assert(!kb_curator_llm_request_init(&request, "policy", "{}", NULL));
   assert(kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, &request, &response, error,
                             sizeof error) < 0);
   assert(response.stop_reason == AIMEE_STOP_MAX_TOKENS && response.usage_in == 31);
   assert(response.model && !strcmp(response.model, "partial-model"));
   assert(response.n_content == 1 && strstr(response.content[0].text, "partial"));
   assert(!kb_curator_llm_text(&response, error, sizeof error));
   aimee_response_free(&response);
   aimee_request_free(&request);
}

static void test_transport_failure_and_no_command_fallback(void)
{
   configure();
   aimee_request_t request = {0};
   aimee_response_t response = {0};
   char error[256] = "";
   assert(!kb_curator_llm_request_init(&request, "policy", "{}", NULL));
   mock_agent_http_set_post_handler(network_error_handler);
   assert(kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, &request, &response, error,
                             sizeof error) < 0);
   assert(g_post_calls == 1 && kb_curator_provider_backoff_active());
   kb_curator_provider_backoff_recovered();
   mock_agent_http_set_post_handler(err_handler);
   assert(kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, &request, &response, error,
                             sizeof error) < 0);
   aimee_response_free(&response);
   memset(&cfg, 0, sizeof cfg);
   mock_agent_http_reset();
   g_post_calls = 0;
   assert(kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, &request, &response, error,
                             sizeof error) < 0);
   assert(strstr(error, "command model fallbacks are unsupported") && g_post_calls == 0);
   assert(!response.content);
   aimee_request_free(&request);
}
int main(void)
{
   unsetenv("SYNTHESIS_ENDPOINT");
   unsetenv("SYNTHESIS_MODEL");
   unsetenv("SYNTHESIS_API_KEY");
   test_owned_request_and_response();
   test_projection_requires_terminal_authored_text();
   test_actual_partial_response_retained_not_projected();
   test_transport_failure_and_no_command_fallback();
   puts("kb_curator_llm: canonical request/response, projection and no fallback PASS");
   return 0;
}
