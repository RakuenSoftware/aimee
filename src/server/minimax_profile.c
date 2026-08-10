/* minimax_profile.c: model_provider_t profile for MiniMax. */
#include "model_provider.h"
#include "runtime_secret.h"
#include "aimee.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "provider_model_parse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *minimax_env_vars[] = {"MINIMAX_API_KEY", NULL};
/* M2.7 is kept as the fallback model behind the M3 default. */
static const char *minimax_fallback_models[] = {"MiniMax-M2.7", NULL};

static int minimax_fetch_models(model_provider_t *p, provider_model_t **models_out, int *n_out)
{
   (void)p;
   *models_out = NULL;
   *n_out = 0;

   char key[384];
   if (!runtime_secret_get("MINIMAX_API_KEY", key, sizeof(key)))
      return -1;

   char auth_header[512];
   snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", key);

   char *body = NULL;
   int status = agent_http_get("https://api.minimax.io/v1/models", auth_header, &body, 15000);
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

static const char *minimax_routable_models[] = {"MiniMax-M3", NULL};

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
    .routable_models = minimax_routable_models,
    .fixed_temperature = -1,
    .default_max_tokens = 8192,
    .default_headers = NULL,
    .fetch_models = minimax_fetch_models,
};
