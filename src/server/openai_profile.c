/* openai_profile.c: model_provider_t profile for OpenAI. */
#include "model_provider.h"
#include "runtime_secret.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *openai_env_vars[] = {"OPENAI_API_KEY", NULL};

static int openai_fetch_models(model_provider_t *p, char ***models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   char key[384];
   if (!runtime_secret_get("OPENAI_API_KEY", key, sizeof(key)))
      return -1;

   char auth_header[512];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", key);

   char *body = NULL;
   int status = agent_http_get("https://api.openai.com/v1/models", auth_header, &body, 15000);
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

static const char *openai_routable_models[] = {"gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna",
                                               NULL};

model_provider_t openai_provider = {
    .name = "openai",
    .display_name = "OpenAI",
    .description = "OpenAI API (GPT-4o, o1, o3, ...)",
    .base_url = "https://api.openai.com/v1",
    .models_url = "https://api.openai.com/v1/models",
    .signup_url = "https://platform.openai.com/api-keys",
    .auth_type = "api_key",
    .env_vars = openai_env_vars,
    .api_mode = API_MODE_CHAT_COMPLETIONS,
    .default_model = "gpt-4o",
    .default_aux_model = "gpt-4o-mini",
    .fallback_models = NULL,
    .routable_models = openai_routable_models,
    .fixed_temperature = -1,
    .default_max_tokens = 8192,
    .default_headers = NULL,
    .fetch_models = openai_fetch_models,
};
