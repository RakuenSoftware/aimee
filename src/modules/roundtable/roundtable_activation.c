/* roundtable_activation.c: one owner for roundtable activation and surfaces. */
#include "roundtable_activation.h"
#include "aimee.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_roundtable_active;

static int roundtable_env_enabled(void)
{
   const char *v = getenv("AIMEE_MODULE_ROUNDTABLE");
   if (!v || !v[0])
      return 0;
   if (strcasecmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0 ||
       strcasecmp(v, "yes") == 0)
      return 1;
   if (strcasecmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "off") == 0 ||
       strcasecmp(v, "no") == 0)
      return 0;
   aimee_log(LOG_WARN, "roundtable", "invalid AIMEE_MODULE_ROUNDTABLE value; defaulting off");
   return 0;
}

int roundtable_module_enabled(const config_t *cfg)
{
   return config_module_enabled(cfg ? cfg->module_roundtable : -1, roundtable_env_enabled());
}

const char *roundtable_module_disabled_message(void)
{
   return "roundtable module is disabled; set modules.roundtable: true or "
          "AIMEE_MODULE_ROUNDTABLE=true";
}

void roundtable_runtime_configure(const config_t *cfg)
{
   g_roundtable_active = cfg ? roundtable_module_enabled(cfg) : 0;
}

static int name_is_owned(const char *name, const char *const *exact, const char *const *prefix)
{
   if (!name)
      return 0;
   for (int i = 0; exact[i]; i++)
      if (strcmp(name, exact[i]) == 0)
         return 1;
   for (int i = 0; prefix[i]; i++)
      if (strncmp(name, prefix[i], strlen(prefix[i])) == 0)
         return 1;
   return 0;
}

int roundtable_operation_available(const char *operation)
{
   static const char *const exact[] = {"delegate.aggregate", "delegate.roundtable", NULL};
   static const char *const prefix[] = {"pipeline.", NULL};
   return !name_is_owned(operation, exact, prefix) || g_roundtable_active;
}

int roundtable_tool_available(const char *tool)
{
   static const char *const exact[] = {"ensemble_review", "pipeline", NULL};
   static const char *const prefix[] = {"pipeline_", NULL};
   return !name_is_owned(tool, exact, prefix) || g_roundtable_active;
}
