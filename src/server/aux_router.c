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

char *aux_call(const char *task_name, const char *prompt, int max_tokens)
{
   if (!config_aux_enabled())
      return NULL;
   if (cooldown_active())
      return NULL;

   /* Resolve provider and model from per-task config, falling back to defaults.
    * Both are copied into local storage rather than pointed at: they are held
    * across agent_load_config and the agent lookup below, well past the point an
    * accessor's thread-local buffer would be reclaimed. */
   char provider[64] = "";
   char model[128] = "";
   snprintf(provider, sizeof(provider), "%s", config_aux_default_provider());
   snprintf(model, sizeof(model), "%s", config_aux_default_model());
   int tok = (max_tokens > 0) ? max_tokens : config_aux_default_max_tokens();

   int n = config_aux_task_count();
   for (int i = 0; i < n && i < CONFIG_AUX_MAX_TASKS; i++)
   {
      config_aux_task_t t;
      if (config_aux_task_at(i, &t) != 0 || strcmp(t.task, task_name) != 0)
         continue;
      if (t.provider[0])
         snprintf(provider, sizeof(provider), "%s", t.provider);
      if (t.model[0])
         snprintf(model, sizeof(model), "%s", t.model);
      if (t.max_tokens > 0)
         tok = t.max_tokens;
      break;
   }

   if (!provider[0])
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
   if (model[0])
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

void aux_config_show(void)
{
   printf("auxiliary.enabled:          %s\n", config_aux_enabled() ? "true" : "false");
   /* Each printf consumes its accessor's buffer before the next call, so these
    * can read one at a time -- unlike the loop below, which needs three values
    * from one element live at once and takes a copy. */
   printf("auxiliary.default_provider: %s\n",
          config_aux_default_provider()[0] ? config_aux_default_provider() : "(none)");
   printf("auxiliary.default_model:    %s\n",
          config_aux_default_model()[0] ? config_aux_default_model() : "(none)");
   printf("auxiliary.default_max_tokens: %d\n", config_aux_default_max_tokens());
   int n = config_aux_task_count();
   if (n == 0)
   {
      printf("(no per-task overrides)\n");
      return;
   }
   printf("\n%-30s  %-20s  %-40s  %s\n", "task", "provider", "model", "max_tokens");
   printf("%-30s  %-20s  %-40s  %s\n", "----", "--------", "-----", "----------");
   for (int i = 0; i < n && i < CONFIG_AUX_MAX_TASKS; i++)
   {
      config_aux_task_t t;
      if (config_aux_task_at(i, &t) != 0)
         continue;
      const char *prov = t.provider[0] ? t.provider : "(default)";
      const char *mod = t.model[0] ? t.model : "(default)";
      if (t.max_tokens > 0)
         printf("%-30s  %-20s  %-40s  %d\n", t.task, prov, mod, t.max_tokens);
      else
         printf("%-30s  %-20s  %-40s  (default)\n", t.task, prov, mod);
   }
}
