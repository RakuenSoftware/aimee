/* roundtable_activation.c: one owner for roundtable activation and surfaces. */
#include "roundtable_activation.h"
#include "aimee.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int g_roundtable_active;

int roundtable_module_enabled(void)
{
   return config_module_roundtable_enabled();
}

const char *roundtable_module_disabled_message(void)
{
   return "roundtable module is disabled; set modules.roundtable: true or "
          "AIMEE_MODULE_ROUNDTABLE=true";
}

void roundtable_runtime_configure(void)
{
   g_roundtable_active = roundtable_module_enabled();
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
   /* "roundtable_review", NOT "ensemble_review". The served tool is
    * roundtable_review; ensemble_review survives only in file names
    * (delegate_ensemble_review.c) and has not been a tool name for some time.
    * Naming the dead one made this predicate a no-op for the only tool it
    * exists to guard: with the module disabled the tool stayed on tools/list,
    * the agent called it, and got back
    * {"status":"pending","pause_reason":"panel_unreachable"} -- two wasted round
    * trips per cell, every cell, measured on the benchmark. The filtering
    * machinery around it was correct the whole time. */
   static const char *const exact[] = {"roundtable_review", "pipeline", NULL};
   static const char *const prefix[] = {"pipeline_", NULL};
   return !name_is_owned(tool, exact, prefix) || g_roundtable_active;
}
