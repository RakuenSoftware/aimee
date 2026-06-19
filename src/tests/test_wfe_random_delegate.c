/* test_wfe_random_delegate.c -- wfe_resolve_delegate, incl. the "$random"
 * sentinel -> a uniformly-random ENABLED roster agent, and fail-fast on an empty
 * roster (the sentinel must never leak downstream). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "agent_config.h"
#include "wfe_live_delegate.h"

int main(void)
{
   printf("wfe-random-delegate: ");
   char out[64];

   /* Empty / specific names pass through. */
   assert(wfe_resolve_delegate("", NULL, out, sizeof out) == 0 && out[0] == '\0');
   assert(wfe_resolve_delegate("mistral", NULL, out, sizeof out) == 0 &&
          strcmp(out, "mistral") == 0);

   /* A roster with two enabled + one disabled agent. */
   agent_config_t acfg;
   memset(&acfg, 0, sizeof acfg);
   acfg.agent_count = 3;
   snprintf(acfg.agents[0].name, sizeof acfg.agents[0].name, "alpha");
   acfg.agents[0].enabled = 1;
   snprintf(acfg.agents[1].name, sizeof acfg.agents[1].name, "beta");
   acfg.agents[1].enabled = 0; /* disabled -> never picked */
   snprintf(acfg.agents[2].name, sizeof acfg.agents[2].name, "gamma");
   acfg.agents[2].enabled = 1;

   /* $random picks an ENABLED agent; deterministic under a seed. Never the
    * disabled one, never the literal sentinel. */
   int saw_alpha = 0, saw_gamma = 0;
   for (unsigned s = 1; s <= 20; s++)
   {
      wfe_resolve_delegate_seed(s);
      assert(wfe_resolve_delegate("$random", &acfg, out, sizeof out) == 0);
      assert(strcmp(out, "$random") != 0);
      assert(strcmp(out, "beta") != 0); /* disabled excluded */
      assert(strcmp(out, "alpha") == 0 || strcmp(out, "gamma") == 0);
      if (!strcmp(out, "alpha"))
         saw_alpha = 1;
      if (!strcmp(out, "gamma"))
         saw_gamma = 1;
   }
   assert(saw_alpha && saw_gamma); /* both enabled agents reachable */

   /* Empty roster -> fail fast (never leak "$random"). */
   agent_config_t empty;
   memset(&empty, 0, sizeof empty);
   assert(wfe_resolve_delegate("$random", &empty, out, sizeof out) == -1);
   assert(out[0] == '\0');

   printf("ok\n");
   return 0;
}
