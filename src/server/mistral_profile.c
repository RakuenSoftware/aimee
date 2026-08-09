/* mistral_profile.c: model_provider_t profile for Mistral AI. */
#include "model_provider.h"
#include "runtime_secret.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "provider_model_parse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *mistral_env_vars[] = {"MISTRAL_API_KEY", NULL};
static const char *mistral_fallback_models[] = {"mistral-large-latest", "mistral-small-latest",
                                                NULL};

static int mistral_fetch_models(model_provider_t *p, provider_model_t **models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   char key[384];
   if (!runtime_secret_get("MISTRAL_API_KEY", key, sizeof(key)))
      return -1;

   char auth_header[512];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", key);

   char *body = NULL;
   int status = agent_http_get("https://api.mistral.ai/v1/models", auth_header, &body, 15000);
   runtime_secret_wipe(key, sizeof(key));
   runtime_secret_wipe(auth_header, sizeof(auth_header));
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

model_provider_t mistral_provider = {
    .name = "mistral",
    .display_name = "Mistral AI",
    .description = "Mistral API (Codestral, Mistral Large, Mistral Small)",
    .base_url = "https://api.mistral.ai/v1",
    .models_url = "https://api.mistral.ai/v1/models",
    .signup_url = "https://console.mistral.ai/api-keys",
    .auth_type = "api_key",
    .env_vars = mistral_env_vars,
    .api_mode = API_MODE_CHAT_COMPLETIONS,
    .default_model = "mistral-large-latest",
    .default_aux_model = "mistral-small-latest",
    .fallback_models = mistral_fallback_models,
    .fixed_temperature = -1,
    .default_max_tokens = 8192,
    .default_headers = NULL,
    .fetch_models = mistral_fetch_models,
};
