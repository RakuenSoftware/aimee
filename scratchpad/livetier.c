/* Run the tier lint against the LIVE agents.json + real models.dev catalog. */
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "agent_config.h"
#include "agent_tier_lint.h"

int main(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0) { printf("no agents.json\n"); return 1; }
   printf("agents: %d\n", cfg.agent_count);
   agent_tier_conflict_t c[AGENT_TIER_LINT_MAX];
   int n = agent_tier_price_conflicts(&cfg, c, AGENT_TIER_LINT_MAX);
   printf("conflicts: %d\n", n);
   for (int i = 0; i < n && i < AGENT_TIER_LINT_MAX; i++)
      printf("  '%s' tier %d ($%.2f/$%.2f) is DEARER than '%s' tier %d ($%.2f/$%.2f)\n",
             c[i].cheaper_tier_agent, c[i].cheaper_tier, c[i].cheaper_tier_in, c[i].cheaper_tier_out,
             c[i].costlier_tier_agent, c[i].costlier_tier, c[i].costlier_tier_in, c[i].costlier_tier_out);
   return 0;
}
