/* model_provider.c: provider profile registry. */
#include "model_provider.h"
#include <pthread.h>
#include <string.h>

#define MAX_PROVIDERS 32

static model_provider_t *g_providers[MAX_PROVIDERS];
static int g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Forward declarations for built-in profiles */
extern model_provider_t openai_provider;
extern model_provider_t anthropic_provider;
extern model_provider_t ollama_provider;
extern model_provider_t llama_native_provider;
extern model_provider_t openrouter_provider;
extern model_provider_t mistral_provider;
extern model_provider_t minimax_provider;

static void register_builtins(void)
{
   model_provider_register(&openai_provider);
   model_provider_register(&anthropic_provider);
   model_provider_register(&ollama_provider);
   model_provider_register(&llama_native_provider);
   model_provider_register(&openrouter_provider);
   model_provider_register(&mistral_provider);
   model_provider_register(&minimax_provider);
}

void model_provider_register(model_provider_t *p)
{
   if (!p || !p->name)
      return;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < g_count; i++)
   {
      if (strcmp(g_providers[i]->name, p->name) == 0)
      {
         pthread_mutex_unlock(&g_lock);
         return;
      }
   }
   if (g_count < MAX_PROVIDERS)
      g_providers[g_count++] = p;
   pthread_mutex_unlock(&g_lock);
}

model_provider_t *model_provider_get(const char *name)
{
   if (!name || !name[0])
      return NULL;
   pthread_once(&g_once, register_builtins);
   pthread_mutex_lock(&g_lock);
   model_provider_t *found = NULL;
   for (int i = 0; i < g_count; i++)
   {
      if (strcmp(g_providers[i]->name, name) == 0)
      {
         found = g_providers[i];
         break;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

int model_provider_list(model_provider_t **out, int max)
{
   pthread_once(&g_once, register_builtins);
   pthread_mutex_lock(&g_lock);
   int n = g_count < max ? g_count : max;
   for (int i = 0; i < n; i++)
      out[i] = g_providers[i];
   pthread_mutex_unlock(&g_lock);
   return n;
}
