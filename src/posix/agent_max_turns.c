/* posix/agent_max_turns.c: max-turn resolution for primary vs delegate sessions. */
#include "aimee.h"
#include "agent_exec.h"
#include "agent_config.h"
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
   int cap = role ? config_max_iterations_delegate() : config_max_iterations();
   return cap > 0 ? cap : INT_MAX;
}

/* The role an invocation is BUDGETED as, which is not always the role it is
 * ROUTED as. The webchat/primary chat turn runs through agent_run_with_tools
 * with a nominal role ("code") so the routing layer can pick a seat, but it is
 * not a delegate: the user can always send another message, so it must never be
 * capped by the delegate budget nor told its tool budget is exhausted. The
 * caller already marks the turn with agent_routing_set_primary_turn() (thread-
 * local, so delegations the turn spawns on other threads stay policed), so
 * budget policy keys off that flag rather than off the routing role. */
const char *agent_budget_role(const char *role)
{
   return agent_routing_primary_turn() ? NULL : role;
}
