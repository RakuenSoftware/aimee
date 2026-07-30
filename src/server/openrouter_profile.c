/* openrouter_profile.c: model_provider_t profile for OpenRouter. */
#include "model_provider.h"
#include "runtime_secret.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *openrouter_env_vars[] = {"OPENROUTER_API_KEY", NULL};
static const char *openrouter_default_headers[] = {
    "HTTP-Referer", "https://github.com/JBailes/aimee", "X-Title", "aimee", NULL};
static const char *openrouter_fallback_models[] = {
    "anthropic/claude-opus-4.7", "google/gemini-2.5-flash", "mistralai/mistral-large-2512", NULL};

static int openrouter_classify_body(model_provider_t *p, int http_status, const char *body,
                                    failover_reason_t *out)
{
   (void)p;
   if (!body || !out)
      return 0;
   if ((http_status == 400 || http_status == 403 || http_status == 404) &&
       (strstr(body, "No endpoints found") || strstr(body, "data policy") ||
        strstr(body, "privacy policy")))
   {
      *out = FAILOVER_PROVIDER_POLICY;
      return 1;
   }
   return 0;
}

static int openrouter_fetch_models(model_provider_t *p, char ***models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   char key[384];
   if (!runtime_secret_get("OPENROUTER_API_KEY", key, sizeof(key)))
      return -1;

   char auth_header[512];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", key);

   char *body = NULL;
   int status = agent_http_get("https://openrouter.ai/api/v1/models", auth_header, &body, 15000);
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

   cJSON *data = cJSON_GetObjectItem(root, "data");
   if (!data || !cJSON_IsArray(data))
   {
      cJSON_Delete(root);
      return -1;
   }

   int n = cJSON_GetArraySize(data);
   char **arr = calloc((size_t)n, sizeof(char *));
   if (!arr)
   {
      cJSON_Delete(root);
      return -1;
   }

   int count = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, data)
   {
      cJSON *id = cJSON_GetObjectItem(item, "id");
      if (id && cJSON_IsString(id) && id->valuestring)
      {
         arr[count] = strdup(id->valuestring);
         if (arr[count])
            count++;
      }
   }

   cJSON_Delete(root);
   *models_out = arr;
   *n_out = count;
   return 0;
}

model_provider_t openrouter_provider = {
    .name = "openrouter",
    .display_name = "OpenRouter",
    .description = "OpenAI-compatible routing layer for 200+ models",
    .base_url = "https://openrouter.ai/api/v1",
    .models_url = "https://openrouter.ai/api/v1/models",
    .signup_url = "https://openrouter.ai/keys",
    .auth_type = "api_key",
    .env_vars = openrouter_env_vars,
    .api_mode = API_MODE_CHAT_COMPLETIONS,
    .default_model = "anthropic/claude-opus-4.7",
    .default_aux_model = "google/gemini-2.5-flash",
    .fallback_models = openrouter_fallback_models,
    .fixed_temperature = -1,
    .default_max_tokens = 8192,
    .default_headers = openrouter_default_headers,
    .fetch_models = openrouter_fetch_models,
    .classify_body = openrouter_classify_body,
};
