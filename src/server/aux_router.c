/* aux_router.c: auxiliary model routing + fallback chain. */
#include "aux_router.h"
#include "aimee.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_types.h"
#include "log.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define AUX_COOLDOWN_TRANSIENT_SECS 60
#define AUX_COOLDOWN_NOCONFIG_SECS  600

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_cooldown_until = 0;

static int cooldown_active(void)
{
   pthread_mutex_lock(&g_lock);
   int active = g_cooldown_until > 0 && time(NULL) < g_cooldown_until;
   pthread_mutex_unlock(&g_lock);
   return active;
}

static void cooldown_set(int secs)
{
   time_t until = time(NULL) + secs;
   pthread_mutex_lock(&g_lock);
   if (until > g_cooldown_until)
      g_cooldown_until = until;
   pthread_mutex_unlock(&g_lock);
}

char *aux_call(const config_t *cfg, const char *task_name, const char *prompt, int max_tokens)
{
   if (!cfg || !cfg->aux_enabled)
      return NULL;
   if (cooldown_active())
      return NULL;

   /* Resolve provider and model from per-task config, fall back to defaults */
   const char *provider = cfg->aux_default_provider[0] ? cfg->aux_default_provider : NULL;
   const char *model = cfg->aux_default_model[0] ? cfg->aux_default_model : NULL;
   int tok = (max_tokens > 0) ? max_tokens : cfg->aux_default_max_tokens;

   for (int i = 0; i < cfg->aux_task_count; i++)
   {
      if (strcmp(cfg->aux_tasks[i].task, task_name) == 0)
      {
         if (cfg->aux_tasks[i].provider[0])
            provider = cfg->aux_tasks[i].provider;
         if (cfg->aux_tasks[i].model[0])
            model = cfg->aux_tasks[i].model;
         if (cfg->aux_tasks[i].max_tokens > 0)
            tok = cfg->aux_tasks[i].max_tokens;
         break;
      }
   }

   if (!provider || !provider[0])
   {
      LOG_WARN("aux", "no provider configured for task '%s'; cooldown %ds", task_name,
               AUX_COOLDOWN_NOCONFIG_SECS);
      cooldown_set(AUX_COOLDOWN_NOCONFIG_SECS);
      return NULL;
   }

   agent_config_t ac;
   memset(&ac, 0, sizeof(ac));
   if (agent_load_config(&ac) != 0)
   {
      LOG_WARN("aux", "aux_call: could not load agents.json");
      cooldown_set(AUX_COOLDOWN_TRANSIENT_SECS);
      return NULL;
   }

   agent_t *ag = agent_find(&ac, provider);
   if (!ag || !ag->enabled)
   {
      LOG_WARN("aux", "aux_call: no enabled agent '%s' in agents.json", provider);
      cooldown_set(AUX_COOLDOWN_NOCONFIG_SECS);
      return NULL;
   }

   /* Clone so we can override model without touching shared state */
   agent_t local = *ag;
   if (model && model[0])
      snprintf(local.model, sizeof(local.model), "%s", model);

   agent_result_t res;
   memset(&res, 0, sizeof(res));
   int rc = agent_dispatch_one(&local, NULL, NULL, NULL, prompt, tok, 0.0, 0 /* use_tools */, &res);
   if (rc != 0 || !res.response || !res.response[0])
   {
      LOG_WARN("aux", "aux_call task '%s': %s", task_name,
               res.error[0] ? res.error : "empty response");
      free(res.response);
      cooldown_set(AUX_COOLDOWN_TRANSIENT_SECS);
      return NULL;
   }
   return res.response;
}

void aux_config_show(const config_t *cfg)
{
   if (!cfg)
      return;
   printf("auxiliary.enabled:          %s\n", cfg->aux_enabled ? "true" : "false");
   printf("auxiliary.default_provider: %s\n",
          cfg->aux_default_provider[0] ? cfg->aux_default_provider : "(none)");
   printf("auxiliary.default_model:    %s\n",
          cfg->aux_default_model[0] ? cfg->aux_default_model : "(none)");
   printf("auxiliary.default_max_tokens: %d\n", cfg->aux_default_max_tokens);
   if (cfg->aux_task_count == 0)
   {
      printf("(no per-task overrides)\n");
      return;
   }
   printf("\n%-30s  %-20s  %-40s  %s\n", "task", "provider", "model", "max_tokens");
   printf("%-30s  %-20s  %-40s  %s\n", "----", "--------", "-----", "----------");
   for (int i = 0; i < cfg->aux_task_count; i++)
   {
      const char *prov = cfg->aux_tasks[i].provider[0] ? cfg->aux_tasks[i].provider : "(default)";
      const char *mod = cfg->aux_tasks[i].model[0] ? cfg->aux_tasks[i].model : "(default)";
      int mt = cfg->aux_tasks[i].max_tokens;
      if (mt > 0)
         printf("%-30s  %-20s  %-40s  %d\n", cfg->aux_tasks[i].task, prov, mod, mt);
      else
         printf("%-30s  %-20s  %-40s  (default)\n", cfg->aux_tasks[i].task, prov, mod);
   }
}
