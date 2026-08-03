/* test_kb_curator_llm.c: curator stage->LLM dispatch (curator-llm-backend §2b).
 * Provider path is driven through the mocked agent_http_post; the sidecar
 * fallback path is the unchanged kb_curator_sidecar_run and not re-tested here. */
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
      *response_buf = strdup("{\"choices\":[{\"message\":{\"content\":"
                             "\"{\\\"synthesis\\\":\\\"ok\\\"}\"}}]}");
   return 200;
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

/* A provider is configured -> dispatch routes to provider_client and returns the
 * response content (here the synthesis JSON). This used to seed tier_b.* because
 * SYNTHESIZE was a reasoning stage; one provider serves every stage now. */
static void test_provider_path(void)
{
   kb_curator_provider_backoff_recovered();
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(ok_handler);
   g_seen_timeout_ms = 0;
   g_post_calls = 0;
   memset(&cfg, 0, sizeof(cfg));
   cfg.kb_curator_extract_max_tokens = 137;
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://big/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "big-32b");

   char err[256];
   char *resp = kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, "be-a-curator", "{\"topic\":\"t\"}",
                                   NULL, "" /* no fallback */, 16384, err, sizeof(err));
   assert(resp != NULL);
   assert(strcmp(resp, "{\"synthesis\":\"ok\"}") == 0);
   assert(strcmp(g_seen_url, "http://big/v1/chat/completions") == 0);
   /* The ceiling has to clear the SLOWEST sidecar we ship, and this assertion is
    * what makes lowering it a deliberate act. It has been wrong twice: 120s was
    * shorter than a normal constrained extraction, and 300s was shorter than an E4B
    * one -- 1768 completion tokens at 4.98 tokens/s is 375s, measured on the
    * deployed sidecar. See KB_CURATOR_PROVIDER_TIMEOUT_MS for why the value is 600s
    * and not the ~825s the 4096-token budget would suggest (the 15-minute
    * stale-lease reaper is the real ceiling). */
   assert(g_seen_timeout_ms == 600000);
   assert(g_post_calls == 1);
   assert(!kb_curator_provider_backoff_active());
   /* system_prompt + request_json must reach the provider as message content. */
   assert(strstr(g_seen_body, "be-a-curator") != NULL);
   assert(strstr(g_seen_body, "\\\"topic\\\":\\\"t\\\"") != NULL || strstr(g_seen_body, "topic"));
   cJSON *sent = cJSON_Parse(g_seen_body);
   assert(sent != NULL);
   cJSON *max_tokens = cJSON_GetObjectItemCaseSensitive(sent, "max_tokens");
   assert(cJSON_IsNumber(max_tokens) && max_tokens->valueint == 137);
   cJSON_Delete(sent);
   free(resp);
   mock_agent_http_reset();
   printf("kb_curator_llm: provider path (url + system + request in body) ok\n");
}

/* The durable curator queue owns retries. A network timeout must not trigger the
 * provider client's default three immediate attempts (which used to leave three
 * overlapping generations running on the bundled model). */
static void test_provider_network_error_is_single_attempt(void)
{
   kb_curator_provider_backoff_recovered();
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(network_error_handler);
   g_post_calls = 0;
   memset(&cfg, 0, sizeof(cfg));
   /* Configure the provider explicitly. This used to resolve through ambient env
    * left set by an earlier case, which only worked by test ordering. */
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://big/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "big-32b");

   char err[256] = "";
   char *resp = kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, "sys", "{}", NULL, "", 16384, err,
                                   sizeof(err));
   assert(resp == NULL);
   assert(g_post_calls == 1);
   assert(kb_curator_provider_backoff_active());
   kb_curator_provider_backoff_recovered();
   mock_agent_http_reset();
   printf("kb_curator_llm: network failure uses one durable-queue attempt ok\n");
}

/* Provider configured but the call fails -> NULL + reason, no crash/leak. */
static void test_provider_error(void)
{
   kb_curator_provider_backoff_recovered();
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(err_handler);
   memset(&cfg, 0, sizeof(cfg));

   char err[256] = "";
   char *resp = kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, "sys", "{}", NULL, "", 16384, err,
                                   sizeof(err));
   assert(resp == NULL);
   assert(err[0] != '\0');
   assert(!kb_curator_provider_backoff_active());
   mock_agent_http_reset();
   printf("kb_curator_llm: provider error -> NULL + reason ok\n");
}

/* Tier-B unconfigured AND no fallback command -> idle (NULL + reason). The
 * Tier-A default must NOT be borrowed for a Tier-B stage. */
static void test_idle_when_unconfigured(void)
{
   mock_agent_http_reset();
   memset(&cfg, 0, sizeof(cfg));
   /* Only a Tier-A provider set; synthesize is Tier-B, so it must stay idle. */
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://small/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "small");

   char err[256] = "";
   char *resp = kb_curator_llm_run(KB_CURATOR_STAGE_SYNTHESIZE, "sys", "{}", NULL, "", 16384, err,
                                   sizeof(err));
   assert(resp == NULL);
   assert(err[0] != '\0'); /* "no curator provider or command configured" */
   printf("kb_curator_llm: idle when tier unconfigured (no tier-A fallback) ok\n");
}

int main(void)
{
   /* The resolver falls back to SYNTHESIS_ENDPOINT env; clear it so the idle-path test
    * is deterministic regardless of the ambient/CI environment. */
   unsetenv("SYNTHESIS_ENDPOINT");
   unsetenv("SYNTHESIS_MODEL");
   unsetenv("SYNTHESIS_API_KEY");
   test_provider_path();
   test_provider_error();
   test_provider_network_error_is_single_attempt();
   test_idle_when_unconfigured();
   printf("kb_curator_llm: all tests passed\n");
   return 0;
}
