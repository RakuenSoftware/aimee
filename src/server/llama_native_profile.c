/* llama_native_profile.c: model_provider_t profile for llama.cpp server. */
#include "model_provider.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "provider_model_parse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *llama_env_vars[] = {NULL};

static int llama_fetch_models(model_provider_t *p, provider_model_t **models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   const char *base = getenv("LLAMA_HOST");
   char url[256];
   snprintf(url, sizeof(url), "%s/v1/models", base && base[0] ? base : "http://localhost:8080");

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

model_provider_t llama_native_provider = {
    .name = "llama_native",
    .display_name = "llama.cpp (local)",
    .description = "llama.cpp server — native or OpenAI-compatible API",
    .base_url = "http://localhost:8080/v1",
    .models_url = "http://localhost:8080/v1/models",
    .signup_url = "https://github.com/ggerganov/llama.cpp",
    .auth_type = "none",
    .env_vars = llama_env_vars,
    .api_mode = API_MODE_LLAMA_NATIVE,
    .default_model = "local",
    .default_aux_model = "local",
    .fallback_models = NULL,
    .fixed_temperature = -1,
    .default_max_tokens = 4096,
    .default_headers = NULL,
    .fetch_models = llama_fetch_models,
};
