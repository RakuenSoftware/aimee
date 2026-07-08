/* test_kb_curator_llm.c: curator stage->LLM dispatch (curator-llm-backend §2b).
 * Provider path is driven through the mocked agent_http_post; the sidecar
 * fallback path is the unchanged kb_curator_sidecar_run and not re-tested here. */
#include "kb/kb_curator_llm.h"

#include "config.h"
#include "support/mock_agent_http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_seen_url[512];
static char g_seen_body[1024];

static int ok_handler(const char *url, const char *auth_header, const char *body,
                      char **response_buf, int timeout_ms, const char *extra_headers)
{
   (void)auth_header;
   (void)timeout_ms;
   (void)extra_headers;
   snprintf(g_seen_url, sizeof(g_seen_url), "%s", url ? url : "");
   snprintf(g_seen_body, sizeof(g_seen_body), "%s", body ? body : "");
   if (response_buf)
      *response_buf = strdup("{\"choices\":[{\"message\":{\"content\":"
                             "\"{\\\"synthesis\\\":\\\"ok\\\"}\"}}]}");
   return 200;
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

/* A Tier-B provider is configured -> dispatch routes to provider_client and
 * returns the response content (here the synthesis JSON). */
static void test_provider_path(void)
{
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(ok_handler);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_tier_b_base_url, sizeof(cfg.kb_curator_tier_b_base_url),
            "http://big/v1");
   snprintf(cfg.kb_curator_tier_b_model, sizeof(cfg.kb_curator_tier_b_model), "big-32b");

   char err[256];
   char *resp =
       kb_curator_llm_run(&cfg, KB_CURATOR_STAGE_SYNTHESIZE, "be-a-curator", "{\"topic\":\"t\"}",
                          NULL, "" /* no fallback */, 16384, err, sizeof(err));
   assert(resp != NULL);
   assert(strcmp(resp, "{\"synthesis\":\"ok\"}") == 0);
   assert(strcmp(g_seen_url, "http://big/v1/chat/completions") == 0);
   /* system_prompt + request_json must reach the provider as message content. */
   assert(strstr(g_seen_body, "be-a-curator") != NULL);
   assert(strstr(g_seen_body, "\\\"topic\\\":\\\"t\\\"") != NULL || strstr(g_seen_body, "topic"));
   free(resp);
   mock_agent_http_reset();
   printf("kb_curator_llm: provider path (url + system + request in body) ok\n");
}

/* Provider configured but the call fails -> NULL + reason, no crash/leak. */
static void test_provider_error(void)
{
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(err_handler);
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.kb_curator_tier_b_base_url, sizeof(cfg.kb_curator_tier_b_base_url),
            "http://big/v1");
   snprintf(cfg.kb_curator_tier_b_model, sizeof(cfg.kb_curator_tier_b_model), "big-32b");

   char err[256] = "";
   char *resp = kb_curator_llm_run(&cfg, KB_CURATOR_STAGE_SYNTHESIZE, "sys", "{}", NULL, "", 16384,
                                   err, sizeof(err));
   assert(resp == NULL);
   assert(err[0] != '\0');
   mock_agent_http_reset();
   printf("kb_curator_llm: provider error -> NULL + reason ok\n");
}

/* Tier-B unconfigured AND no fallback command -> idle (NULL + reason). The
 * Tier-A default must NOT be borrowed for a Tier-B stage. */
static void test_idle_when_unconfigured(void)
{
   mock_agent_http_reset();
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* Only a Tier-A provider set; synthesize is Tier-B, so it must stay idle. */
   snprintf(cfg.kb_curator_provider_base_url, sizeof(cfg.kb_curator_provider_base_url),
            "http://small/v1");
   snprintf(cfg.kb_curator_provider_model, sizeof(cfg.kb_curator_provider_model), "small");

   char err[256] = "";
   char *resp = kb_curator_llm_run(&cfg, KB_CURATOR_STAGE_SYNTHESIZE, "sys", "{}", NULL, "", 16384,
                                   err, sizeof(err));
   assert(resp == NULL);
   assert(err[0] != '\0'); /* "no curator provider or command configured" */
   printf("kb_curator_llm: idle when tier unconfigured (no tier-A fallback) ok\n");
}

int main(void)
{
   /* The resolver falls back to LLM_ENDPOINT env; clear it so the idle-path test
    * is deterministic regardless of the ambient/CI environment. */
   unsetenv("LLM_ENDPOINT");
   unsetenv("LLM_MODEL");
   unsetenv("LLM_API_KEY");
   test_provider_path();
   test_provider_error();
   test_idle_when_unconfigured();
   printf("kb_curator_llm: all tests passed\n");
   return 0;
}
