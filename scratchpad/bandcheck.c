/* Band resolution against the real catalog, at sizes either side of thresholds. */
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "agent_config.h"
#include "agent_tier_lint.h"
#include "model_registry.h"

static void probe(const char *name, const char *provider, const char *model, int ctx_ceiling)
{
   agent_t ag; memset(&ag, 0, sizeof(ag));
   snprintf(ag.name, sizeof(ag.name), "%s", name);
   snprintf(ag.provider, sizeof(ag.provider), "%s", provider);
   snprintf(ag.model, sizeof(ag.model), "%s", model);
   ag.middleware.context_window = ctx_ceiling;
   ag.enabled = 1;

   model_capability_t cap; memset(&cap,0,sizeof(cap));
   model_capability_get(provider, model, &cap);
   printf("%-14s bands=%d reachable=%d", model, cap.price_band_count,
          agent_has_reachable_price_band(&ag));
   int probes[] = {1000, 250000, 300000, 600000, 0};
   for (int i = 0; probes[i]; i++)
   {
      double in=0, out=0;
      if (agent_resolved_price_at_context(&ag, probes[i], &in, &out, NULL))
         printf("  @%dk=$%.2f/$%.2f", probes[i]/1000, in, out);
   }
   printf("\n");
}
int main(void)
{
   probe("codex", "openai", "gpt-5.6-sol", 272000);
   probe("mm", "minimax", "MiniMax-M3", 0);
   probe("claude", "anthropic", "claude-opus-4-8", 200000);
   return 0;
}

/* Mirror of agent_config.c, so this harness links without the config layer. */
const char *agent_catalog_provider(const agent_t *agent)
{
   if (!agent) return "";
   return agent->catalog_provider[0] ? agent->catalog_provider : agent->provider;
}
int agent_has_role(const agent_t *a, const char *r){(void)a;(void)r;return 1;}
int agent_is_exec_role(const agent_t *a, const char *r){(void)a;(void)r;return 1;}
void aimee_log(int lvl, const char *cat, const char *fmt, ...){(void)lvl;(void)cat;(void)fmt;}
