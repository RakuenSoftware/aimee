/* test_roundtable_seat_resolve.c: the seat-model resolver — a "$random" seat
 * picks any eligible review agent (excluding `used`) and reports exhaustion; a
 * pinned model resolves to that EXACT agent with no substitution, reporting
 * PINNED_UNAVAILABLE when it is absent, disabled, or lacks the role. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "agent_config.h"
#include "roundtable_seat_resolve.h"

/* Build a plain HTTP agent that is enabled, serves `role`, and is routable (a
 * literal api_key gives resolvable credentials, so agent_is_available_for_routing
 * short-circuits before touching the vault). */
static void mk_agent(agent_t *a, const char *name, const char *role, int enabled)
{
   memset(a, 0, sizeof *a);
   snprintf(a->name, sizeof a->name, "%s", name);
   a->enabled = enabled;
   snprintf(a->roles[0], sizeof a->roles[0], "%s", role);
   a->role_count = 1;
   snprintf(a->api_key, sizeof a->api_key, "sk-test-%s", name); /* literal -> routable */
}

int main(void)
{
   /* Deterministic random draws. */
   delegate_role_pick_seed(1234u);

   /* --- rt_seat_is_random --- */
   assert(rt_seat_is_random("$random"));
   assert(rt_seat_is_random(""));   /* unset -> random, never a fail */
   assert(rt_seat_is_random(NULL)); /* NULL -> random */
   assert(!rt_seat_is_random("codex"));
   assert(!rt_seat_is_random("$RANDOM")); /* case-sensitive sentinel */

   /* Roster: A/B/C are routable review agents; D is review-capable but disabled;
    * E is enabled but only serves "code". */
   agent_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   mk_agent(&cfg.agents[0], "agentA", "review", 1);
   mk_agent(&cfg.agents[1], "agentB", "review", 1);
   mk_agent(&cfg.agents[2], "agentC", "review", 1);
   mk_agent(&cfg.agents[3], "agentD", "review", 0); /* disabled */
   /* agentE serves only "code": an EXPLICIT exec_roles list omitting "review" is
    * what actually makes an agent non-review-capable (an empty exec list falls back
    * to the default roles, which include "review"). This mirrors a specialized
    * agent like gpu-mid that must never be seated on a review panel. */
   mk_agent(&cfg.agents[4], "agentE", "code", 1);
   snprintf(cfg.agents[4].exec_roles[0], sizeof cfg.agents[4].exec_roles[0], "code");
   cfg.agents[4].exec_role_count = 1;
   cfg.agent_count = 5;

   int idx = -1;

   /* --- pinned: exact resolution / no substitution --- */
   assert(rt_resolve_seat_model(&cfg, "agentB", "review", NULL, 0, &idx) == RT_SEAT_OK);
   assert(idx == 1 && strcmp(cfg.agents[idx].name, "agentB") == 0);

   assert(rt_resolve_seat_model(&cfg, "nope", "review", NULL, 0, &idx) ==
          RT_SEAT_PINNED_UNAVAILABLE);
   assert(rt_resolve_seat_model(&cfg, "agentD", "review", NULL, 0, &idx) ==
          RT_SEAT_PINNED_UNAVAILABLE); /* disabled */
   assert(rt_resolve_seat_model(&cfg, "agentE", "review", NULL, 0, &idx) ==
          RT_SEAT_PINNED_UNAVAILABLE); /* lacks the review role */

   /* --- $random: picks an eligible review agent, honoring exclusion --- */
   assert(rt_resolve_seat_model(&cfg, "$random", "review", NULL, 0, &idx) == RT_SEAT_OK);
   assert(idx >= 0 && idx <= 2); /* one of A/B/C, never disabled D or off-role E */

   /* Exclude A and B: the only remaining eligible pick is C. */
   const char *used2[] = {"agentA", "agentB"};
   assert(rt_resolve_seat_model(&cfg, "$random", "review", used2, 2, &idx) == RT_SEAT_OK);
   assert(idx == 2 && strcmp(cfg.agents[idx].name, "agentC") == 0);

   /* Exclude all three eligible agents: random is exhausted. */
   const char *used3[] = {"agentA", "agentB", "agentC"};
   assert(rt_resolve_seat_model(&cfg, "$random", "review", used3, 3, &idx) ==
          RT_SEAT_RANDOM_EXHAUSTED);

   /* Empty/unset model behaves as "$random", not a pinned failure. */
   assert(rt_resolve_seat_model(&cfg, "", "review", NULL, 0, &idx) == RT_SEAT_OK);

   /* Bad args. */
   assert(rt_resolve_seat_model(NULL, "agentA", "review", NULL, 0, &idx) == RT_SEAT_INVALID);
   assert(rt_resolve_seat_model(&cfg, "agentA", "review", NULL, 0, NULL) == RT_SEAT_INVALID);

   printf("test_roundtable_seat_resolve: OK\n");
   return 0;
}
