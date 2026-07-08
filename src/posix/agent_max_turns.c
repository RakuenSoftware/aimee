/* posix/agent_max_turns.c: max-turn resolution for primary vs delegate sessions. */
#include "aimee.h"
#include "agent_exec.h"
#include "agent_types.h"
#include "config.h"

#include <limits.h>

int agent_resolve_max_turns(const agent_t *agent, const char *role)
{
   /* An explicit positive per-agent cap (or a positive per-role cap the delegate
    * policy stamped on) bounds a delegate run to that many turns. */
   if (agent->max_turns > 0 && role)
      return agent->max_turns;
   /* Default is INFINITE. max_turns <= 0 (the default -1) means "no per-agent/role
    * cap"; a run is unbounded UNLESS an operator set a global iteration cap
    * (GUI-settable max_iterations[_delegate], 0/unset = no cap). This is the
    * "-1 = infinite, for everything, by default" contract. */
   config_t iter_cfg;
   config_load(&iter_cfg);
   int cap = role ? iter_cfg.max_iterations_delegate : iter_cfg.max_iterations;
   return cap > 0 ? cap : INT_MAX;
}
