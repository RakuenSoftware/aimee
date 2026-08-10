/* ollama_profile.c: model_provider_t profile for Ollama (local). */
#include "model_provider.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "provider_model_parse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *ollama_env_vars[] = {NULL};

static int ollama_classify_body(model_provider_t *p, int http_status, const char *body,
                                failover_reason_t *out)
{
   (void)p;
   if (!body || !out)
      return 0;
   if ((http_status == 400 || http_status == 500) &&
       (strstr(body, "num_ctx") || strstr(body, "prompt too long")))
   {
      *out = FAILOVER_CONTEXT_OVERFLOW;
      return 1;
   }
   return 0;
}

static int ollama_fetch_models(model_provider_t *p, provider_model_t **models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   const char *base = getenv("OLLAMA_HOST");
   char url[256];
   snprintf(url, sizeof(url), "%s/api/tags", base && base[0] ? base : "http://localhost:11434");

   char *body = NULL;
   int status = agent_http_get(url, NULL, &body, 10000);
   if (status != 200 || !body)
   {
      free(body);
      return -1;
   }

   cJSON *root = cJSON_Parse(body);
   free(body);
   if (!root)
      return -1;

   /* This endpoint publishes ids only -- no context window, no output ceiling,
    * no capabilities -- so every other field stays 0 = "not published" and the
    * operator's declared values stand. */
   int rc = provider_models_from_list_json(root, models_out, n_out);
   cJSON_Delete(root);
   return rc;
}

model_provider_t ollama_provider = {
    .name = "ollama",
    .display_name = "Ollama (local)",
    .description = "Ollama local model server (OpenAI-compatible /v1 API)",
    .base_url = "http://localhost:11434/v1",
    .models_url = "http://localhost:11434/api/tags",
    .signup_url = "https://ollama.com",
    .auth_type = "none",
    .env_vars = ollama_env_vars,
    .api_mode = API_MODE_CHAT_COMPLETIONS,
    .default_model = "qwen3:1.7b",
    .default_aux_model = "qwen3:0.6b",
    .fallback_models = NULL,
    .fixed_temperature = -1,
    .default_max_tokens = 4096,
    .default_headers = NULL,
    .fetch_models = ollama_fetch_models,
    .classify_body = ollama_classify_body,
};
