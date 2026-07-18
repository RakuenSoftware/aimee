/* delegate_driver.c: provider driver registry and dispatch helpers */
#include "aimee.h"
#include "delegate_driver.h"
#include "agent_tools.h"
#include <string.h>

/* --- Registry --- */

#define MAX_DRIVERS 16

static const delegate_driver_t *g_drivers[MAX_DRIVERS];
static int g_driver_count;
static int g_drivers_inited;

/* Forward declarations for built-in drivers */
extern const delegate_driver_t delegate_driver_openai;
extern const delegate_driver_t delegate_driver_anthropic;
extern const delegate_driver_t delegate_driver_chatgpt;
extern const delegate_driver_t delegate_driver_openrouter;
extern const delegate_driver_t delegate_driver_mistral;
extern const delegate_driver_t delegate_driver_minimax;

void delegate_driver_register(const delegate_driver_t *driver)
{
   if (!driver || !driver->name)
      return;
   /* Prevent duplicate registration */
   for (int i = 0; i < g_driver_count; i++)
   {
      if (strcmp(g_drivers[i]->name, driver->name) == 0)
         return;
   }
   if (g_driver_count >= MAX_DRIVERS)
      return;
   g_drivers[g_driver_count++] = driver;
}

void delegate_drivers_init(void)
{
   if (g_drivers_inited)
      return;
   g_drivers_inited = 1;

   delegate_driver_register(&delegate_driver_openai);
   delegate_driver_register(&delegate_driver_anthropic);
   delegate_driver_register(&delegate_driver_chatgpt);
   delegate_driver_register(&delegate_driver_openrouter);
   delegate_driver_register(&delegate_driver_mistral);
   delegate_driver_register(&delegate_driver_minimax);
}

const delegate_driver_t *delegate_driver_get(const char *provider)
{
   if (!provider || !provider[0])
      provider = "openai";

   /* Exact match */
   for (int i = 0; i < g_driver_count; i++)
   {
      if (strcmp(g_drivers[i]->name, provider) == 0)
         return g_drivers[i];
   }

   /* Aliases: local llama.cpp/Ollama endpoints are OpenAI-compatible;
    * codex uses the Responses driver. */
   if (strcmp(provider, "codex") == 0)
   {
      for (int i = 0; i < g_driver_count; i++)
      {
         if (strcmp(g_drivers[i]->name, "chatgpt") == 0)
            return g_drivers[i];
      }
   }

   if (strcmp(provider, "ollama") == 0 || strcmp(provider, "llama-eval") == 0 ||
       strcmp(provider, "llama_native") == 0)
   {
      for (int i = 0; i < g_driver_count; i++)
      {
         if (strcmp(g_drivers[i]->name, "openai") == 0)
            return g_drivers[i];
      }
   }

   /* Fallback: openai driver */
   for (int i = 0; i < g_driver_count; i++)
   {
      if (strcmp(g_drivers[i]->name, "openai") == 0)
         return g_drivers[i];
   }
   return NULL;
}

/* --- Convenience helpers --- */

int delegate_build_url(const delegate_driver_t *driver, const agent_t *agent, char *url,
                       size_t url_len)
{
   if (driver && driver->build_url)
      return driver->build_url(agent, url, url_len);
   /* Default: OpenAI chat completions */
   snprintf(url, url_len, "%s/chat/completions", agent->endpoint);
   return 0;
}

void delegate_get_caps(const delegate_driver_t *driver, const agent_t *agent, driver_caps_t *caps)
{
   if (driver && driver->get_caps)
   {
      driver->get_caps(agent, caps);
      return;
   }
   /* Sensible defaults */
   caps->capability_flags = DRIVER_CAP_TOOL_CALLS | DRIVER_CAP_STREAMING;
   caps->context_limit = DRIVER_CTX_LARGE;
   caps->max_output_tokens = 0;
}
