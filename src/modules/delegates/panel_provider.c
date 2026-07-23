/* panel_provider.c -- required-core facade for an optional panel engine. */
#include <aimee/delegates/panel_provider.h>

#include <string.h>

static const aimee_panel_provider_t *active_provider;
static unsigned int outstanding_results;

static int provider_valid(const aimee_panel_provider_t *provider)
{
   return provider && provider->aggregate && provider->run && provider->release;
}

int aimee_panel_provider_register(const aimee_panel_provider_t *provider)
{
   if (!provider_valid(provider))
      return AIMEE_PANEL_PROVIDER_INVALID;
   if (active_provider && active_provider != provider)
      return AIMEE_PANEL_PROVIDER_BUSY;
   active_provider = provider;
   return AIMEE_PANEL_PROVIDER_OK;
}

int aimee_panel_provider_unregister(const aimee_panel_provider_t *provider)
{
   if (!provider || active_provider != provider)
      return AIMEE_PANEL_PROVIDER_INVALID;
   if (outstanding_results > 0)
      return AIMEE_PANEL_PROVIDER_BUSY;
   active_provider = NULL;
   return AIMEE_PANEL_PROVIDER_OK;
}

int aimee_panel_provider_available(void)
{
   return active_provider != NULL;
}

int aimee_panel_aggregate(agent_config_t *agents, const config_t *cfg, const char *prompt,
                          aimee_panel_aggregate_result_t *out)
{
   if (!out)
      return AIMEE_PANEL_PROVIDER_INVALID;
   memset(out, 0, sizeof(*out));
   if (!agents || !cfg || !prompt)
      return AIMEE_PANEL_PROVIDER_INVALID;
   if (!active_provider)
      return AIMEE_PANEL_PROVIDER_UNAVAILABLE;
   if (active_provider->aggregate(agents, cfg, prompt, out) != 0)
   {
      memset(out, 0, sizeof(*out));
      return AIMEE_PANEL_PROVIDER_ERROR;
   }
   return AIMEE_PANEL_PROVIDER_OK;
}

int aimee_panel_run(agent_config_t *agents, const config_t *cfg, const char *task,
                    const aimee_panel_options_t *options, aimee_panel_result_t *out)
{
   if (!out)
      return AIMEE_PANEL_PROVIDER_INVALID;
   memset(out, 0, sizeof(*out));
   if (!agents || !cfg || !task)
      return AIMEE_PANEL_PROVIDER_INVALID;
   if (!active_provider)
      return AIMEE_PANEL_PROVIDER_UNAVAILABLE;
   if (active_provider->run(agents, cfg, task, options, out) != 0 || !out->artifact)
   {
      if (out->artifact)
         active_provider->release(out);
      memset(out, 0, sizeof(*out));
      return AIMEE_PANEL_PROVIDER_ERROR;
   }
   outstanding_results++;
   return AIMEE_PANEL_PROVIDER_OK;
}

void aimee_panel_result_release(aimee_panel_result_t *result)
{
   if (!result || !result->artifact)
      return;
   if (active_provider)
   {
      active_provider->release(result);
      memset(result, 0, sizeof(*result));
      if (outstanding_results > 0)
         outstanding_results--;
   }
}
