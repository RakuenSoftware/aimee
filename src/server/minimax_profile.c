/* minimax_profile.c: model_provider_t profile for MiniMax. */
#include "model_provider.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *minimax_env_vars[] = {"MINIMAX_API_KEY", NULL};
/* M2.7 is kept as the fallback model behind the M3 default. */
static const char *minimax_fallback_models[] = {"MiniMax-M2.7", NULL};

static int minimax_fetch_models(model_provider_t *p, char ***models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   const char *key = getenv("MINIMAX_API_KEY");
   if (!key || !key[0])
      return -1;

   char auth_header[512];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", key);

   char *body = NULL;
   int status = agent_http_get("https://api.minimax.io/v1/models", auth_header, &body, 15000);
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

model_provider_t minimax_provider = {
    .name = "minimax",
    .display_name = "MiniMax",
    .description = "MiniMax API (MiniMax-M3)",
    .base_url = "https://api.minimax.io/v1",
    .models_url = "https://api.minimax.io/v1/models",
    .signup_url = "https://platform.minimax.io",
    .auth_type = "api_key",
    .env_vars = minimax_env_vars,
    .api_mode = API_MODE_CHAT_COMPLETIONS,
    .default_model = "MiniMax-M3",
    .default_aux_model = "MiniMax-M3",
    .fallback_models = minimax_fallback_models,
    .fixed_temperature = -1,
    .default_max_tokens = 8192,
    .default_headers = NULL,
    .fetch_models = minimax_fetch_models,
};
