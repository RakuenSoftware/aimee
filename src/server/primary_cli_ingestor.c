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

int primary_cli_ingestor_enforce_preturn(const char *session_id, const char *message,
                                         const char *repo)
{
   /* Trust boundary: with no resolvable aimee session id the seam CANNOT enforce.
    * Return 0 (unbound) rather than pretend -- a caller that lost the session id
    * must degrade to a generic turn, never a silent "enforced" that binds nothing
    * and leaves the guard unanchored. */
   if (!session_id || !session_id[0])
      return 0;

   /* wfe_bind_interactive is itself dial-gated (default-off) and only binds an
    * enforced-routed turn (converse/research stay unbound). Calling it HERE --
    * before the turn is dispatched to the CLI -- is what makes S2 preventive for
    * the external-CLI primary, whose out-of-band model call the gateway router
    * never sees with a session id attached. */
   return wfe_bind_interactive(session_id, message, repo);
}

void primary_cli_ingestor_log_posture(void)
{
   if (!primary_cli_ingestor_enabled())
      return; /* default-off: stay quiet; the hot path is untouched */

   wfe_enforce_stage_t stage = wfe_enforce_stage_parse(getenv("AIMEE_WORKFLOW_ENFORCE_STAGE"));
   if (stage == WFE_ENFORCE_OFF)
      aimee_log(LOG_WARN, "primary-cli-ingestor",
                "AIMEE_PRIMARY_CLI_INGESTOR is on but AIMEE_WORKFLOW_ENFORCE_STAGE=off -> "
                "enforcement is INERT: the tmux CLI primary will NOT bind. Set the dial to "
                "advisory/soft/hard to activate.");
   else
      aimee_log(LOG_INFO, "primary-cli-ingestor",
                "enforce-before-send ACTIVE for the tmux CLI primary (dial=%s)",
                wfe_enforce_stage_name(stage));
}
