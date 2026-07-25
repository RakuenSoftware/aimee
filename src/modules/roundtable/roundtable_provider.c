/* roundtable_provider.c -- adapt optional roundtable execution to core. */
#include <aimee/delegates/panel_provider.h>

#include "delegate_ensemble.h"
#include "log.h"
#include "roundtable_activation.h"

static int provider_aggregate(agent_config_t *agents, const config_t *cfg, const char *prompt,
                              aimee_panel_aggregate_result_t *out)
{
   return delegate_ensemble_run(agents, cfg, prompt, out);
}

static int provider_run(agent_config_t *agents, const config_t *cfg, const char *task,
                        const aimee_panel_options_t *options, aimee_panel_result_t *out)
{
   return delegate_roundtable_run(agents, cfg, task, options, out);
}

static void provider_release(aimee_panel_result_t *result)
{
   delegate_roundtable_result_free(result);
}

static const aimee_panel_provider_t provider = {
    .aggregate = provider_aggregate,
    .run = provider_run,
    .release = provider_release,
};

int roundtable_provider_configure(const config_t *cfg)
{
   int enabled = cfg ? roundtable_module_enabled(cfg) : 0;
   roundtable_runtime_configure(enabled ? cfg : NULL);
   if (!enabled)
   {
      if (aimee_panel_provider_available())
         (void)aimee_panel_provider_unregister(&provider);
      return 0;
   }
   int rc = aimee_panel_provider_register(&provider);
   if (rc != AIMEE_PANEL_PROVIDER_OK)
   {
      aimee_log(LOG_ERROR, "roundtable", "could not register panel provider (%d)", rc);
      return -1;
   }
   return 1;
}
