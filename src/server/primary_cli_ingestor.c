/* primary_cli_ingestor.c -- see primary_cli_ingestor.h. */
#include "primary_cli_ingestor.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "wfe_bind_ingress.h"
#include "wfe_enforce.h"

int primary_cli_ingestor_enabled(void)
{
   const char *v = getenv("AIMEE_PRIMARY_CLI_INGESTOR");
   if (!v || !v[0])
      return 0;
   return strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0;
}

void primary_cli_ingestor_log_posture(void)
{
   if (!primary_cli_ingestor_enabled())
      return; /* default-off: stay quiet; the hot path is untouched */

   /* Same source of truth as the enforce gate: wfe_bind_interactive parses this
    * identical env var with the same parser (unset is treated as off). */
   wfe_enforce_stage_t stage = wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE"));
   if (stage == WFE_ENFORCE_OFF)
      aimee_log(LOG_WARN, "primary-cli-ingestor",
                "AIMEE_PRIMARY_CLI_INGESTOR is on but AIMEE_WORKFLOW_ENFORCE_STAGE is off or unset "
                "-> the tmux CLI primary will NOT bind. Set the dial to advisory/soft/hard.");
   else
      aimee_log(LOG_INFO, "primary-cli-ingestor",
                "enforce-before-send ACTIVE for the tmux CLI primary (dial=%s)",
                wfe_enforce_stage_name(stage));
}
