/* wfe_delegate_resolve.c: resolve a workflow step's delegate name to a concrete
 * agent, including the "$random" sentinel -> a uniformly-random enabled roster
 * agent. Self-contained (only the agent_config struct + libc) so it is cheaply
 * unit-testable. Part of the "Random delegate" workflow option. */
#include "aimee.h"

#include "wfe_live_delegate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Random source. Seedable for deterministic tests via wfe_resolve_delegate_seed;
 * otherwise drawn from /dev/urandom (falls back to time()). */
static unsigned g_rand_seed;
static int g_rand_seeded;

void wfe_resolve_delegate_seed(unsigned seed)
{
   g_rand_seed = seed;
   g_rand_seeded = 1;
}

static unsigned wfe_rand_u(void)
{
   if (g_rand_seeded)
      return (unsigned)rand_r(&g_rand_seed);
   unsigned v = 0;
   FILE *f = fopen("/dev/urandom", "rb");
   if (f)
   {
      if (fread(&v, 1, sizeof v, f) != sizeof v)
         v = 0;
      fclose(f);
   }
   if (!v)
      v = (unsigned)time(NULL);
   return v;
}

int wfe_resolve_delegate(const char *name, agent_config_t *acfg, char *out, size_t outn)
{
   if (out && outn)
      out[0] = '\0';
   if (!name || !name[0])
      return 0; /* no per-step delegate -> route by role */
   if (strcmp(name, "$random") != 0)
   {
      if (out)
         snprintf(out, outn, "%s", name); /* a specific agent */
      return 0;
   }
   /* $random: uniform pick over the enabled roster; fail fast if none, so the
    * sentinel never leaks downstream. */
   if (!acfg)
      return -1;
   int idxs[MAX_AGENTS];
   int n = 0;
   for (int i = 0; i < acfg->agent_count && i < MAX_AGENTS; i++)
      if (acfg->agents[i].enabled)
         idxs[n++] = i;
   if (n == 0)
      return -1;
   int pick = idxs[wfe_rand_u() % (unsigned)n];
   if (out)
      snprintf(out, outn, "%s", acfg->agents[pick].name);
   return 0;
}
