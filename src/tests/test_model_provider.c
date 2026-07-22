#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model_provider.h"

static void test_builtin_profiles_registered(void)
{
   model_provider_t *providers[16];
   int n = model_provider_list(providers, 16);
   assert(n >= 6); /* was 7; the gemini profile is gone */
   assert(model_provider_get("openai") != NULL);
   assert(model_provider_get("anthropic") != NULL);
   assert(model_provider_get("openrouter") != NULL);
   assert(model_provider_get("ollama") != NULL);
   assert(model_provider_get("llama_native") != NULL);
   assert(model_provider_get("mistral") != NULL);
}

static void test_mistral_profile_shape(void)
{
   model_provider_t *p = model_provider_get("mistral");
   assert(p != NULL);
   assert(strcmp(p->display_name, "Mistral AI") == 0);
   assert(strcmp(p->base_url, "https://api.mistral.ai/v1") == 0);
   assert(strcmp(p->models_url, "https://api.mistral.ai/v1/models") == 0);
   assert(strcmp(p->auth_type, "api_key") == 0);
   assert(p->env_vars != NULL);
   assert(strcmp(p->env_vars[0], "MISTRAL_API_KEY") == 0);
   assert(strcmp(p->default_model, "mistral-large-latest") == 0);
   assert(strcmp(p->default_aux_model, "mistral-small-latest") == 0);
   assert(p->fetch_models != NULL);
}

static void test_anthropic_profile_shape(void)
{
   model_provider_t *p = model_provider_get("anthropic");
   assert(p != NULL);
   assert(strcmp(p->display_name, "Anthropic") == 0);
   assert(strcmp(p->base_url, "https://api.anthropic.com/v1") == 0);
   assert(strcmp(p->auth_type, "x-api-key") == 0);
   assert(p->env_vars != NULL);
   assert(strcmp(p->env_vars[0], "ANTHROPIC_API_KEY") == 0);
   assert(strcmp(p->default_model, "claude-sonnet-4-6") == 0);
   assert(strcmp(p->default_aux_model, "claude-haiku-4-5-20251001") == 0);
   assert(p->api_mode == API_MODE_ANTHROPIC_MESSAGES);
}

static void test_openrouter_profile_headers(void)
{
   model_provider_t *p = model_provider_get("openrouter");
   assert(p != NULL);
   assert(p->default_headers != NULL);
   assert(strcmp(p->default_headers[0], "HTTP-Referer") == 0);
   assert(strcmp(p->default_headers[1], "https://github.com/JBailes/aimee") == 0);
   assert(strcmp(p->default_headers[2], "X-Title") == 0);
   assert(strcmp(p->default_headers[3], "aimee") == 0);
   assert(p->default_headers[4] == NULL);
   assert(strcmp(p->default_model, "anthropic/claude-opus-4.7") == 0);
   assert(p->fallback_models != NULL);
   assert(strcmp(p->fallback_models[0], "anthropic/claude-opus-4.7") == 0);
   assert(strcmp(p->fallback_models[1], "google/gemini-2.5-flash") == 0);
   assert(strcmp(p->fallback_models[2], "mistralai/mistral-large-2512") == 0);
   assert(p->fallback_models[3] == NULL);
}

static void test_anthropic_profile_headers(void)
{
   model_provider_t *p = model_provider_get("anthropic");
   assert(p != NULL);
   assert(p->default_headers != NULL);
   assert(strcmp(p->default_headers[0], "anthropic-version") == 0);
   assert(strcmp(p->default_headers[1], "2023-06-01") == 0);
   assert(p->default_headers[2] == NULL);
}

static void test_mistral_fetch_requires_key(void)
{
   model_provider_t *p = model_provider_get("mistral");
   char **models = NULL;
   int n = -1;
   unsetenv("MISTRAL_API_KEY");
   assert(p->fetch_models(p, &models, &n) == -1);
   assert(models == NULL);
   assert(n == 0);
}

static void test_fetch_models_requires_provider_keys(void)
{
   struct
   {
      const char *provider;
      const char *env_vars[3];
   } cases[] = {
       {"openrouter", {"OPENROUTER_API_KEY", NULL}},
   };

   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      model_provider_t *p = model_provider_get(cases[i].provider);
      char **models = NULL;
      int n = -1;
      assert(p != NULL);
      assert(p->fetch_models != NULL);
      for (int j = 0; cases[i].env_vars[j] != NULL; j++)
         unsetenv(cases[i].env_vars[j]);
      assert(p->fetch_models(p, &models, &n) == -1);
      assert(models == NULL);
      assert(n == 0);
   }
}

int main(void)
{
   test_builtin_profiles_registered();
   test_mistral_profile_shape();
   test_anthropic_profile_shape();
   test_openrouter_profile_headers();
   test_anthropic_profile_headers();
   test_mistral_fetch_requires_key();
   test_fetch_models_requires_provider_keys();
   printf("model_provider: all tests passed\n");
   return 0;
}
