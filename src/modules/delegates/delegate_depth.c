/* delegate_depth.c: CLI delegation chain depth guard.
 *
 * Tracks delegation nesting depth via the AIMEE_DELEGATE_DEPTH environment
 * variable so that child processes spawned by a delegate agent inherit the
 * current depth and the limit can be enforced across process boundaries.
 */
#include "platform_process.h"
#include "cmd_agent_delegate_impl.h"
#include <aimee/delegates/module_api.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static delegate_chain_provider_fn g_chain_provider;

void delegate_register_chain_provider(delegate_chain_provider_fn provider)
{
   g_chain_provider = provider;
}

/* Both questions below are the delegates module's rule; this asks them.
 *
 * They fail closed in OPPOSITE directions, because the safe answer differs. An
 * unanswerable staleness question must not clear the depth: discarding it would
 * raise the ceiling for everything underneath. An unanswerable depth question
 * must refuse the delegation: allowing it would let a chain run past the limit
 * this guard exists to enforce. */
int delegate_chain_env_should_clear(const char *depth_env, const char *parent_env,
                                    int parent_active_known, int parent_active)
{
   int clear = 0;
   if (!g_chain_provider)
      return 0;
   return g_chain_provider(AIMEE_DELEGATES_CHAIN_OP_SHOULD_CLEAR, depth_env && depth_env[0],
                           parent_env && parent_env[0], parent_active_known, parent_active, 0, 0,
                           &clear, NULL) == 0
              ? clear
              : 0;
}

int delegate_check_chain_depth(int max_depth, char *errbuf, size_t errbuf_sz)
{
   const char *env_val = getenv("AIMEE_DELEGATE_DEPTH");
   int parent_depth = (env_val && env_val[0]) ? atoi(env_val) : 0;
   int allowed = 0;
   int32_t current_depth = 0;

   if (!g_chain_provider ||
       g_chain_provider(AIMEE_DELEGATES_CHAIN_OP_CHECK_DEPTH, 0, 0, 0, 0, parent_depth, max_depth,
                        &allowed, &current_depth) != 0)
   {
      if (errbuf && errbuf_sz > 0)
         snprintf(errbuf, errbuf_sz,
                  "delegation depth cannot be checked (delegates module unavailable); "
                  "refusing to delegate rather than risk exceeding the limit.");
      return -1;
   }

   if (!allowed)
   {
      if (errbuf && errbuf_sz > 0)
         snprintf(errbuf, errbuf_sz,
                  "delegation chain depth limit exceeded (%d/%d). "
                  "Reduce nesting or increase max_delegation_depth in config.",
                  (int)current_depth, max_depth);
      return -1;
   }

   char depth_str[32];
   snprintf(depth_str, sizeof(depth_str), "%d", (int)current_depth);
   platform_setenv("AIMEE_DELEGATE_DEPTH", depth_str);
   return 0;
}
