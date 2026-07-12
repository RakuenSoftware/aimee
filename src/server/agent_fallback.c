/* agent_fallback.c: Delegate fallback classification and retry helpers. */
#include "aimee.h"
#include "agent.h"
#include "delegate_role.h"
#include "log.h"
#include "provider_catalog.h"
#include <stdlib.h>
#include <string.h>

/* Classify transient provider errors where a different fallback may succeed. */
int agent_error_is_retryable(const char *error)
{
   if (!error || !error[0])
      return 0;
   return strstr(error, "429") != NULL || strstr(error, "rate limit") != NULL ||
          strstr(error, "rate_limit") != NULL || strstr(error, "503") != NULL ||
          strstr(error, "502") != NULL || strstr(error, "HTTP 500") != NULL ||
          strstr(error, "status 500") != NULL || strstr(error, "HTTP 504") != NULL ||
          strstr(error, "status 504") != NULL || strstr(error, "HTTP 408") != NULL ||
          strstr(error, "status 408") != NULL || strstr(error, "RATE_LIMIT") != NULL ||
          strstr(error, "overloaded") != NULL || strstr(error, "HTTP request failed") != NULL ||
          strstr(error, "status -1") != NULL || strstr(error, "connection failed") != NULL ||
          strstr(error, "Connection failed") != NULL || strstr(error, "timeout") != NULL ||
          strstr(error, "timed out") != NULL || strstr(error, "unreachable") != NULL ||
          strstr(error, "Loading model") != NULL || strstr(error, "Model is loading") != NULL ||
          strstr(error, "model is loading") != NULL || strstr(error, "model loading") != NULL ||
          /* An empty completion on an otherwise-successful HTTP 200 is a transient
           * provider glitch (e.g. a fast degenerate response), not a hard error —
           * a retry / fallback usually gets a real completion. Treating it as
           * retryable stops one blank response from degrading a provider or
           * failing a pinned roundtable seat. */
          strstr(error, "no content in response") != NULL;
}

/* Should a fallback/retry caller try a DIFFERENT agent for this result? Yes for a
 * saturation refusal (AGENT_RC_AT_LIMIT — the agent is momentarily at its
 * max_parallel ceiling, so a peer may be free) OR a retryable provider error. A
 * plain success (0) or a non-retryable hard failure is terminal for the fallback
 * path. Centralises the at-limit-triggers-fallback rule so callers don't rely on
 * the error-string classifier (which does NOT — and must not — treat the
 * "at concurrency limit" message as retryable, so at-limit never records health). */
int agent_rc_should_try_another(int rc, const char *error)
{
   if (rc == 0)
      return 0;
   return rc == AGENT_RC_AT_LIMIT || agent_error_is_retryable(error);
}

static int agent_supports_delegate_role(const agent_t *ag, const char *role)
{
   return ag && role && (agent_has_role(ag, role) || agent_is_exec_role(ag, role));
}

static int agent_is_named_in_fallback_chain(const agent_config_t *cfg, const char *name)
{
   if (!cfg || !name || !name[0])
      return 0;
   for (int i = 0; i < cfg->fallback_count; i++)
   {
      if (strcmp(cfg->fallback_chain[i], name) == 0)
         return 1;
   }
   return 0;
}

int agent_try_same_tier_fallback(agent_config_t *cfg, agent_t **current, const char *role,
                                 const char *system_prompt, const char *user_prompt, int max_tokens,
                                 int enforce_writes, agent_result_t *out, int rc)
{
   agent_t *ag = current ? *current : NULL;
   /* Proceed for a retryable failure OR a saturation refusal (AGENT_RC_AT_LIMIT):
    * a same-tier peer may be free even if the primary was momentarily at its cap. */
   if (!cfg || !ag || !out || !agent_rc_should_try_another(rc, out->error))
      return rc;

   /* Cost-tier fallback: a tier is a pool. Even when fallback_chain is stale
    * or names only one primary, retry any same-cost peer before failing. */
   int tier = ag->cost_tier;
   for (int i = 0; i < cfg->agent_count && rc != 0; i++)
   {
      agent_t *peer = &cfg->agents[i];
      if (peer == ag || !peer->enabled || peer->cost_tier != tier ||
          !agent_supports_delegate_role(peer, role) || !agent_is_available_for_routing(peer) ||
          agent_is_named_in_fallback_chain(cfg, peer->name))
         continue;
      if (provider_catalog_get_health(peer->name) == CATALOG_HEALTH_DOWN)
      {
         aimee_log(LOG_DEBUG, "agent", "skipping DOWN same-tier agent '%s'", peer->name);
         continue;
      }

      peer->write_capable = enforce_writes && delegate_role_is_write(role) ? 1 : 0;

      free(out->response);
      out->response = NULL;
      out->error[0] = '\0';

      aimee_log(LOG_INFO, "agent", "fallback: trying same-tier agent '%s'", peer->name);
      /* Through the single guarded executor: enforces peer->max_parallel and
       * records peer health (success or failure). An AGENT_RC_AT_LIMIT keeps rc
       * non-zero so the loop simply moves to the next same-tier peer. */
      rc = agent_dispatch_one(peer, &cfg->network, role, system_prompt, user_prompt, max_tokens,
                              0.3, 1 /* use_tools */, out);
      if (rc == 0)
      {
         ag = peer;
         if (current)
            *current = peer;
      }
   }

   return rc;
}
/* agent_execute_guarded was removed: the concurrency guard + health recording it
 * provided now live in the single agent_dispatch_one (agent_runtime.c), which
 * EVERY dispatch path routes through. */
