/* Shared stub for wfe tests that link wfe_blocks.o (which reads autonomy.* via
 * config_autonomy_lookup) but not config.o. Mimics production's OPERATOR-ENV path (there is
 * no config snapshot in these tests): return the env value if set, else 0 so the caller falls
 * back to its own default. This preserves the tests' setenv-driven behavior. */
#include <stdlib.h>
#include <string.h>
int config_autonomy_lookup(const char *env_name, long *out)
{
   if (!env_name || !out)
      return 0;
   const char *e = getenv(env_name);
   if (!e || !e[0])
      return 0;
   *out = strstr(env_name, "FANOUT") ? (e[0] == '1' ? 1 : 0) : atol(e);
   return 1;
}
