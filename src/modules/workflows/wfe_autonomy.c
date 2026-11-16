/* wfe_autonomy.c: the autonomy driver + human-only gate-override. */
#include "wfe_autonomy.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h> /* isfinite — validate the AIMEE_AUTONOMY_MAX_USD override */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "config.h" /* config_autonomy_lookup: live autonomy.* caps (env > snapshot) */
#include "db1_client/wfe_store.h"
#include "wfe_approval.h"
#include "wfe_blocks.h"
#include "wfe_def.h"
#include "wfe_engine.h"
#include "wfe_iface.h"

int wfe_autonomy_killed(void)
{
   const char *v = getenv("AIMEE_AUTONOMY_KILL_SWITCH");
   if (!v)
      return 0;
   char normalized[8];
   size_t n = strlen(v);
   if (n >= sizeof(normalized))
      return 0;
   for (size_t i = 0; i <= n; i++)
      normalized[i] = (char)tolower((unsigned char)v[i]);
   return !strcmp(normalized, "1") || !strcmp(normalized, "true") || !strcmp(normalized, "yes") ||
          !strcmp(normalized, "on");
}

double wfe_autonomy_default_max_cost_usd(void)
{
   /* WP-5: the shared per-run USD budget ceiling (see wfe_autonomy.h). Reject
    * inf/NaN — either would void the cap rather than tune it. */
   double cap_usd = 5.0;
   const char *v = getenv("AIMEE_AUTONOMY_MAX_USD");
   if (v && v[0])
   {
      char *e = NULL;
      double d = strtod(v, &e);
      if (e && *e == '\0' && isfinite(d) && d >= 0)
         cap_usd = d;
   }
   return cap_usd;
}
