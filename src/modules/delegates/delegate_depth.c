/* delegate_depth.c: CLI delegation chain depth guard.
 *
 * Tracks delegation nesting depth via the AIMEE_DELEGATE_DEPTH environment
 * variable so that child processes spawned by a delegate agent inherit the
 * current depth and the limit can be enforced across process boundaries.
 */
#include "platform_process.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int delegate_chain_env_should_clear(const char *depth_env, const char *parent_env,
                                    int parent_active_known, int parent_active)
{
   int has_depth = depth_env && depth_env[0];
   int has_parent = parent_env && parent_env[0];
   if (has_depth && !has_parent)
      return 1;
   if (has_parent && parent_active_known && !parent_active)
      return 1;
   return 0;
}

int delegate_check_chain_depth(int max_depth, char *errbuf, size_t errbuf_sz)
{
   const char *env_val = getenv("AIMEE_DELEGATE_DEPTH");
   int parent_depth = (env_val && env_val[0]) ? atoi(env_val) : 0;
   int current_depth = parent_depth + 1;

   if (current_depth > max_depth)
   {
      if (errbuf && errbuf_sz > 0)
         snprintf(errbuf, errbuf_sz,
                  "delegation chain depth limit exceeded (%d/%d). "
                  "Reduce nesting or increase max_delegation_depth in config.",
                  current_depth, max_depth);
      return -1;
   }

   char depth_str[32];
   snprintf(depth_str, sizeof(depth_str), "%d", current_depth);
   platform_setenv("AIMEE_DELEGATE_DEPTH", depth_str);
   return 0;
}
