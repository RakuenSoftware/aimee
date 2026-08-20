/* test_roundtable_seat_resolve.c: the seat-model resolver — a "$random" seat
 * picks any eligible review agent (excluding `used`) and reports exhaustion; a
 * pinned model resolves to that EXACT agent with no substitution, reporting
 * PINNED_UNAVAILABLE when it is absent, disabled, or lacks the role. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "agent_config.h"
#include "roundtable_seat_resolve.h"

/* Build a plain HTTP agent that is enabled, serves `role`, and is routable (a
 * literal api_key gives resolvable credentials, so agent_is_available_for_routing
 * short-circuits before touching the vault). */
/* Only ever used to DECLARE the selection authority before it is dropped, so
 * the resolver sees an authority with no provider behind it. */
static int seat_route_selector(int randomized, uint32_t candidate_count, uint32_t *selected_index)
{
   (void)randomized;
   (void)candidate_count;
   if (selected_index)
      *selected_index = 0;
   return 0;
}

static void mk_agent(agent_t *a, const char *name, const char *role, int enabled)
{
   memset(a, 0, sizeof *a);
   snprintf(a->name, sizeof a->name, "%s", name);
   a->enabled = enabled;
   snprintf(a->roles[0], sizeof a->roles[0], "%s", role);
   a->role_count = 1;
   snprintf(a->api_key, sizeof a->api_key, "sk-test-%s", name); /* literal -> routable */
}

/* Stub capacity probe: reports slots in use per agent name, or -1 for unknown. */
static int g_cap_full, g_cap_free;
static int cap_probe(const char *agent_name)
{
   if (agent_name && strcmp(agent_name, "full") == 0)
      return g_cap_full;
   if (agent_name && strcmp(agent_name, "free") == 0)
      return g_cap_free;
   return -1;
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
   /* agentE declares only the "code" role and NO explicit exec_roles. Because
    * `review` is not a default exec role, that alone makes it non-review-capable:
    * an implementation-only delegate (e.g. a local synth model) is never seated on
    * a review panel just for leaving exec_roles empty. */
   mk_agent(&cfg.agents[4], "agentE", "code", 1);
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

   /* Downgrade contract (wfe_live_panel): when the DISTINCT pool is exhausted (more
    * lenses than eligible agents), a retry WITHOUT the exclusion still resolves —
    * i.e. an already-seated agent is REUSED rather than leaving the lens unfilled.
    * This is what lets a panel compose by reusing agents instead of degrading the
    * gate; it degrades only when NO eligible agent exists at all. */
   assert(rt_resolve_seat_model(&cfg, "$random", "review", used3, 3, &idx) ==
          RT_SEAT_RANDOM_EXHAUSTED); /* distinct: none */
   assert(rt_resolve_seat_model(&cfg, "$random", "review", NULL, 0, &idx) ==
          RT_SEAT_OK);           /* reuse */
   assert(idx >= 0 && idx <= 2); /* reuse still respects role eligibility (A/B/C, not D/E) */

   /* Empty/unset model behaves as "$random", not a pinned failure. */
   assert(rt_resolve_seat_model(&cfg, "", "review", NULL, 0, &idx) == RT_SEAT_OK);

   /* Bad args. */
   assert(rt_resolve_seat_model(NULL, "agentA", "review", NULL, 0, &idx) == RT_SEAT_INVALID);
   assert(rt_resolve_seat_model(&cfg, "agentA", "review", NULL, 0, NULL) == RT_SEAT_INVALID);

   /* --- a routing-module outage is NOT an exhausted roster ---
    * Both arrive as a -1 from the role picker, and reporting the outage as
    * exhaustion tells an operator to add agents to a roster that is already
    * full while the routing module is what is down. The panel also retries a
    * $random seat with a smaller exclusion set on exhaustion, which cannot
    * possibly help here. */
   agent_set_route_selection_provider(seat_route_selector);
   agent_set_route_selection_provider(NULL); /* authority declared, provider lost */
   assert(rt_resolve_seat_model(&cfg, "$random", "review", NULL, 0, &idx) ==
          RT_SEAT_ROUTING_UNAVAILABLE);
   agent_reset_route_selection_authority();
   /* With the authority gone the same call resolves again, so the verdict above
    * was about the outage and not about this roster. */
   assert(rt_resolve_seat_model(&cfg, "$random", "review", NULL, 0, &idx) == RT_SEAT_OK);
   /* And a genuinely exhausted pool still reports exhaustion, not an outage. */
   assert(rt_resolve_seat_model(&cfg, "$random", "review", used3, 3, &idx) ==
          RT_SEAT_RANDOM_EXHAUSTED);

   /* --- a "$random" seat prefers an agent with a free concurrency slot ---
    * Routing checks health/policy/structure but not capacity, so a saturated
    * agent stayed selectable and the seat failed at admission with
    * AGENT_RC_AT_LIMIT. Health cannot compensate: at-limit is deliberately never
    * recorded as a provider fault, so the agent is never marked DOWN. */
   agent_config_t ccfg;
   memset(&ccfg, 0, sizeof ccfg);
   mk_agent(&ccfg.agents[0], "full", "review", 1);
   mk_agent(&ccfg.agents[1], "free", "review", 1);
   ccfg.agents[0].max_parallel = 3;
   ccfg.agents[1].max_parallel = 3;
   ccfg.agent_count = 2;

   agent_set_route_capacity_probe(cap_probe);
   /* "full" is at its cap, "free" is idle: every draw must pick "free". */
   g_cap_full = 3;
   g_cap_free = 0;
   for (int i = 0; i < 40; i++)
   {
      assert(rt_resolve_seat_model(&ccfg, "$random", "review", NULL, 0, &idx) == RT_SEAT_OK);
      assert(idx == 1); /* never the saturated agent while a free one exists */
   }

   /* Both saturated: PREFER must not become EXCLUDE. The seat still resolves —
    * the caller gets an agent and blocking admission waits for a slot — rather
    * than collapsing a populated roster into "no agent for role". */
   g_cap_full = 3;
   g_cap_free = 3;
   assert(rt_resolve_seat_model(&ccfg, "$random", "review", NULL, 0, &idx) == RT_SEAT_OK);
   assert(idx == 0 || idx == 1);

   /* Unknown capacity (-1, i.e. controller unconfigured) must read as "has
    * capacity", so an unconfigured build keeps the prior behaviour. */
   g_cap_full = -1;
   g_cap_free = -1;
   assert(rt_resolve_seat_model(&ccfg, "$random", "review", NULL, 0, &idx) == RT_SEAT_OK);
   assert(idx == 0 || idx == 1);

   /* A pinned seat is unaffected by capacity: it resolves to that exact agent
    * (no substitution) even when saturated — admission decides, not routing. */
   g_cap_full = 3;
   g_cap_free = 0;
   assert(rt_resolve_seat_model(&ccfg, "full", "review", NULL, 0, &idx) == RT_SEAT_OK);
   assert(idx == 0);

   agent_set_route_capacity_probe(NULL); /* leave the global clean for other tests */

   printf("test_roundtable_seat_resolve: OK\n");
   return 0;
}
