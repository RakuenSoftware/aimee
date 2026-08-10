/* routing.c: delegate routing and role/persona matching.
 *
 * Split out of agent_config.c, which had grown past the 2500-line hard cap in
 * line-check. The seam is the one the file already documented: config
 * load/save and auth resolution stay behind, everything under the "Routing"
 * banner moved here verbatim. Pure code motion — no behaviour change.
 *
 * This is the cheapest-seat-with-capability router: agent_route() picks the
 * lowest cost_tier that serves the role, and the agent_route_with_caps*()
 * family gates that choice on capability requirements before cost is
 * considered.
 */
#include "aimee.h"
#include "util.h"
#include "agent_config.h"
#include "agent_config_internal.h"
#include "model_registry.h"
#include "provider_cli_adapter.h"
#include "cJSON.h"
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include "log.h"
#include <unistd.h>

/* --- Routing --- */

int agent_has_role(const agent_t *agent, const char *role)
{
   for (int i = 0; i < agent->role_count; i++)
   {
      /* "all" is a wildcard: the agent serves every role (routing only — tool
       * use is still governed by exec_roles / tools_enabled). */
      if (strcmp(agent->roles[i], "all") == 0 || strcmp(agent->roles[i], role) == 0)
         return 1;
   }
   return 0;
}

/* 1 if the agent may be dispatched AS `persona`. An agent with no personas list
 * (backward-compatible default) or one containing the "all" wildcard serves any
 * persona; otherwise the persona must be listed. A NULL/empty persona is treated
 * as unconstrained (routing then depends on role alone). */
int agent_supports_persona(const agent_t *agent, const char *persona)
{
   if (!agent)
      return 0;
   if (!persona || !persona[0] || agent->persona_count == 0)
      return 1;
   for (int i = 0; i < agent->persona_count; i++)
   {
      if (strcmp(agent->personas[i], "all") == 0 || strcmp(agent->personas[i], persona) == 0)
         return 1;
   }
   return 0;
}

/* Selection eligibility is exactly role membership: an agent serves `role` iff
 * it declares the `all` wildcard or the role itself (agent_has_role). There is
 * deliberately NO exec-role fallback here — a role an agent did not declare is a
 * role it is not selected for. This is the single rule every routing, fallback,
 * and panel-seating path uses; agent_is_exec_role governs only tool exposure at
 * execution time, not who is picked. */
static int agent_supports_role(const agent_t *agent, const char *role)
{
   return agent_has_role(agent, role);
}

static int agent_command_on_path(const char *cmd)
{
   if (!cmd || !cmd[0])
      return 0;

   char *tokens[32] = {0};
   int count = shlex_split(cmd, tokens, 32);
   if (count <= 0 || count >= 32)
   {
      util_free_tokens(tokens, count);
      return 0;
   }
   for (int i = 0; i < count; i++)
   {
      if (util_token_is_shell_operator(tokens[i]))
      {
         util_free_tokens(tokens, count);
         return 0;
      }
   }
   const char *exe = (count > 0 && tokens[0]) ? tokens[0] : cmd;
   int available = 0;

   if (strchr(exe, '/'))
   {
      available = access(exe, X_OK) == 0;
   }
   else
   {
      const char *path = getenv("PATH");
      if (!path || !path[0])
         path = "/usr/local/bin:/usr/bin:/bin";

      char *copy = safe_strdup(path);
      char *saveptr = NULL;
      for (char *dir = strtok_r(copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr))
      {
         char candidate[MAX_PATH_LEN];
         snprintf(candidate, sizeof(candidate), "%s/%s", dir[0] ? dir : ".", exe);
         if (access(candidate, X_OK) == 0)
         {
            available = 1;
            break;
         }
      }
      free(copy);
   }

   util_free_tokens(tokens, count);
   return available;
}

/* Optional route-time health filter; see agent_set_route_health_filter. */
static int (*g_route_health_filter)(const char *agent_name) = NULL;

void agent_set_route_health_filter(int (*fn)(const char *agent_name))
{
   g_route_health_filter = fn;
}

/* Optional route-time DEGRADED predicate; see agent_set_route_degraded_filter.
 * Unlike the health filter this does NOT exclude - a degraded seat stays
 * routable - it only lets selection PREFER a healthy peer when one exists. */
static int (*g_route_degraded_filter)(const char *agent_name) = NULL;

void agent_set_route_degraded_filter(int (*fn)(const char *agent_name))
{
   g_route_degraded_filter = fn;
}

/* 1 if the agent is currently degraded (recovering from a failure streak, or
 * intermittently failing) per the registered predicate. Never true in
 * filter-less builds (CLI / tests), which keep the prior behaviour. */
static int agent_is_degraded(const agent_t *ag)
{
   return g_route_degraded_filter && ag->name[0] && g_route_degraded_filter(ag->name);
}

/* Optional route-time delegate-policy filter; see agent_set_route_policy_filter. */
static int (*g_route_policy_filter)(const agent_t *agent) = NULL;

void agent_set_route_policy_filter(int (*fn)(const agent_t *agent))
{
   g_route_policy_filter = fn;
}

/* Optional route-time capacity probe; see agent_set_route_capacity_probe. A hook,
 * like the health and policy filters above, so this unit keeps no link
 * dependency on the admission controller and filter-less builds (CLI / tests)
 * keep the prior behaviour. */
static int (*g_route_capacity_probe)(const char *agent_name) = NULL;

void agent_set_route_capacity_probe(int (*fn)(const char *agent_name))
{
   g_route_capacity_probe = fn;
}

/* How many delegates is this agent running right now, or -1 when that is not
 * knowable here (no probe registered, or the controller is unconfigured).
 *
 * Exposed so the agent list served over /v1 can publish live occupancy. The Go
 * WFE routes seats in a separate process and cannot call the in-process probe
 * above, so without this it can only see max_parallel and will happily seat an
 * agent that is already saturated — the seat then fails at admission with
 * AGENT_RC_AT_LIMIT. Keeps the same hook indirection, so this unit still has no
 * link dependency on the admission controller. */
int agent_route_agent_active(const char *agent_name)
{
   if (!g_route_capacity_probe || !agent_name || !agent_name[0])
      return -1;
   return g_route_capacity_probe(agent_name);
}

/* Does this agent have a free concurrency slot right now? Returns 1 when it
 * does, and ALSO when capacity is unknown — no probe registered, controller
 * unconfigured (-1), or an agent with no per-agent cap. Unknown must read as
 * "yes": this predicate only narrows a candidate set, so answering "no" on
 * ignorance would drop seats for a reason we cannot substantiate. */
static int agent_has_free_slot(const agent_t *ag)
{
   if (!g_route_capacity_probe || !ag || !ag->name[0] || ag->max_parallel <= 0)
      return 1;
   int active = g_route_capacity_probe(ag->name);
   if (active < 0)
      return 1; /* unconfigured / unknown */
   return active < ag->max_parallel;
}

/* See agent_config.h: marks the current thread's turn as PRIMARY (not
 * delegation) so the policy filter doesn't exclude the provider-named agent
 * from its own chat turn. */
static _Thread_local int g_routing_primary_turn;

void agent_routing_set_primary_turn(int on)
{
   g_routing_primary_turn = on ? 1 : 0;
}

int agent_routing_primary_turn(void)
{
   return g_routing_primary_turn;
}

agent_route_block_t agent_routing_block_reason(const agent_t *agent, char *detail, size_t detail_sz)
{
   if (detail && detail_sz)
      detail[0] = '\0';
   if (!agent)
      return AGENT_ROUTE_NULL;
   /* A provider the health catalog has marked unavailable (e.g. DOWN after a
    * failure streak) must not receive new routed work, or delegates wedge on
    * a dead endpoint. Treat it like a disabled agent so callers fall back to a
    * healthy peer; routing returns NULL (clean "no agent" error) only when
    * every candidate is filtered out. */
   if (g_route_health_filter && agent->name[0] && g_route_health_filter(agent->name))
      return AGENT_ROUTE_HEALTH_DOWN;
   /* A claude-CLI agent can only ever execute as a delegate SERVER-SIDE (a
    * client-only claude has no server session to drive — dispatch would just
    * fail). Structural, so it is enforced even with no policy filter
    * registered; the per-agent rules (the `primary_only` opt-out, primary
    * self-delegation) live in the registered policy filter. */
   if (agent_is_claude_cli(agent) && !agent->is_server_hosted)
      return AGENT_ROUTE_CLIENT_ONLY_CLAUDE;
   if (g_route_policy_filter && g_route_policy_filter(agent))
   {
      /* The filter is opaque here, but the agent record names the common case:
       * a Primary-Agent-Only opt-out. Anything else is a generic policy block. */
      if (detail && detail_sz && agent->primary_only)
         snprintf(detail, detail_sz, "it is flagged \"Primary Agent Only\"");
      return AGENT_ROUTE_POLICY_EXCLUDED;
   }
   if (strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) == 0)
   {
      const char *cmd =
          agent->cli_cmd[0] ? agent->cli_cmd : (agent->cli_kind[0] ? agent->cli_kind : "claude");
      if (!agent_command_on_path("tmux"))
      {
         if (detail && detail_sz)
            snprintf(detail, detail_sz, "tmux");
         return AGENT_ROUTE_MISSING_COMMAND;
      }
      if (!agent_command_on_path(cmd))
      {
         if (detail && detail_sz)
            snprintf(detail, detail_sz, "%s", cmd);
         return AGENT_ROUTE_MISSING_COMMAND;
      }
      return AGENT_ROUTE_OK;
   }

   if (strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) != 0 &&
       strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) != 0)
      return agent_has_resolvable_credentials(agent) ? AGENT_ROUTE_OK : AGENT_ROUTE_NO_CREDENTIALS;

   const provider_cli_adapter_t *adapter = provider_cli_adapter_get(agent->cli_kind);
   if (adapter && adapter->native_provider && adapter->native_provider[0])
      return AGENT_ROUTE_OK;

   const char *cmd = agent->cli_cmd[0] ? agent->cli_cmd : agent->cli_kind;
   if (!agent_command_on_path(cmd))
   {
      if (detail && detail_sz)
         snprintf(detail, detail_sz, "%s", cmd);
      return AGENT_ROUTE_MISSING_COMMAND;
   }
   return AGENT_ROUTE_OK;
}

int agent_is_available_for_routing(const agent_t *agent)
{
   return agent_routing_block_reason(agent, NULL, 0) == AGENT_ROUTE_OK;
}

int agent_any_delegate_available(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0)
      return 0;
   for (int i = 0; i < cfg.agent_count; i++)
      if (cfg.agents[i].enabled && agent_is_available_for_routing(&cfg.agents[i]))
         return 1;
   return 0;
}

agent_t *agent_find(agent_config_t *cfg, const char *name)
{
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (strcmp(cfg->agents[i].name, name) == 0)
         return &cfg->agents[i];
   }
   return NULL;
}

agent_t *agent_default_primary(agent_config_t *cfg)
{
   /* An explicitly configured default wins — but only when it is actually
    * usable. Routing to a disabled default (or, historically, a disabled
    * agents[0]) makes every ingress request that doesn't name a model fast-fail
    * as "failed to reach the primary provider" even though enabled agents
    * exist, so the fallback deliberately skips disabled seats. */
   if (cfg->default_agent[0])
   {
      agent_t *ag = agent_find(cfg, cfg->default_agent);
      if (ag && ag->enabled)
         return ag;
   }
   for (int i = 0; i < cfg->agent_count; i++)
      if (cfg->agents[i].enabled)
         return &cfg->agents[i];
   return NULL;
}

/* Pick fairly from an array of equally eligible candidates.  `default_agent` is
 * the primary-session default, not a hidden delegate pin: letting it win every
 * unpinned delegation starves every peer at the same tier.  A process-wide
 * atomic cursor gives those peers round-robin opportunities while explicit
 * --via pins continue to be handled before this function is reached. */
/* A FREE, locally-hosted seat: llama.cpp / Ollama style, or an unauthenticated
 * endpoint on this machine. Mirrors the local-provider notion already used by
 * agent_default_inject_respond_tool. */
static int agent_is_local(const agent_t *ag)
{
   if (!ag)
      return 0;
   if (strcmp(ag->provider, "ollama") == 0 || strcmp(ag->provider, "llama_native") == 0 ||
       strcmp(ag->provider, "llama-eval") == 0)
      return 1;
   return strcmp(ag->auth_type, "none") == 0 && agent_endpoint_is_localish(ag->endpoint);
}

static agent_route_selection_fn g_route_selection_provider;

void agent_set_route_selection_provider(agent_route_selection_fn provider)
{
   g_route_selection_provider = provider;
}

static agent_t *agent_pick_balanced(agent_t **candidates, int count)
{
   static unsigned cursor;
   if (count <= 0)
      return NULL;
   if (count == 1)
      return candidates[0];
   if (g_route_selection_provider)
   {
      uint32_t selected = 0;
      if (g_route_selection_provider(0, (uint32_t)count, &selected) != 0 ||
          selected >= (uint32_t)count)
         return NULL;
      return candidates[selected];
   }
   unsigned pick = __atomic_fetch_add(&cursor, 1u, __ATOMIC_RELAXED);
   return candidates[pick % (unsigned int)count];
}

int agent_is_claude_cli(const agent_t *agent)
{
   if (!agent)
      return 0;
   /* Only the Claude CLI (`claude` / `claude-code`) run via tmux or the
    * provider-CLI binary — i.e. authenticated by the interactive `claude` login,
    * NOT an API key. Other CLI agents (Codex CLI, gemini-cli, …) are not gated. */
   if (strcmp(agent->cli_kind, "claude") != 0 && strcmp(agent->cli_kind, "claude-code") != 0)
      return 0;
   return strcmp(agent->backend, AGENT_BACKEND_TMUX_CLI) == 0 ||
          strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 ||
          strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) == 0;
}

/* --- generalized role dispatch: a viable delegate for a role ---
 * Return the index of an enabled, routable agent that serves `role` and is not
 * named in `exclude`, chosen uniformly at random among the eligible set (so
 * repeated requests for the same role vary — a roundtable of N `review`
 * delegates, excluding those already used, gets diverse reviewers). Returns -1
 * when none remain. Callers loop: pick -> run -> on failure add the agent to
 * `exclude` -> pick again, until one works. Eligibility + retry-until-viable is
 * the whole mechanism; a specific agent is used only when a caller pins one. */
static unsigned g_role_rand_seed;
static int g_role_rand_seeded;
void delegate_role_pick_seed(unsigned seed)
{
   g_role_rand_seed = seed;
   g_role_rand_seeded = 1;
}
static unsigned delegate_role_rand(void)
{
   if (g_role_rand_seeded)
      return (unsigned)rand_r(&g_role_rand_seed);
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
int delegate_pick_for_role(agent_config_t *cfg, const char *role, const char *const exclude[],
                           int nexclude)
{
   if (!cfg || !role || !role[0])
      return -1;
   int elig[MAX_AGENTS];
   int n = 0;
   for (int i = 0; i < cfg->agent_count && i < MAX_AGENTS; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      int skip = 0;
      for (int e = 0; e < nexclude && !skip; e++)
         if (exclude[e] && strcmp(exclude[e], ag->name) == 0)
            skip = 1;
      if (skip)
         continue;
      elig[n++] = i;
   }
   if (n == 0)
      return -1;

   /* Prefer a seat that can actually start now. Eligibility above asks whether
    * the agent is routable (health, policy, structure, credentials) but not
    * whether it has a free concurrency slot, so a saturated agent stayed
    * selectable and the seat failed at admission with AGENT_RC_AT_LIMIT. Health
    * cannot close that gap: being at-limit is deliberately NOT recorded as a
    * provider fault (agent_fallback.c), so such an agent is never marked DOWN.
    * Live effect: roundtable seats drew a saturated agent, failed instantly, and
    * a panel needing 2 of 3 seats was declared unreachable while other agents
    * sat idle.
    *
    * PREFER, never exclude. If every eligible agent is saturated we fall back to
    * the full eligible set, so this cannot turn a populated roster into "no
    * agent for role" (-1) — the caller still gets a seat and blocking admission
    * waits for a slot, exactly as before. It only stops us choosing a full agent
    * over a free one. */
   int freeel[MAX_AGENTS];
   int nfree = 0;
   for (int i = 0; i < n; i++)
      if (agent_has_free_slot(&cfg->agents[elig[i]]))
         freeel[nfree++] = elig[i];

   const int *pool = nfree > 0 ? freeel : elig;
   int pool_n = nfree > 0 ? nfree : n;
   if (g_route_selection_provider)
   {
      uint32_t selected = 0;
      if (g_route_selection_provider(1, (uint32_t)pool_n, &selected) != 0 ||
          selected >= (uint32_t)pool_n)
         return -1;
      return pool[selected];
   }
   return pool[delegate_role_rand() % (unsigned)pool_n];
}

int agent_pick_named_for_role(agent_config_t *cfg, const char *name, const char *role)
{
   if (!cfg || !name || !name[0] || !role || !role[0])
      return -1;
   for (int i = 0; i < cfg->agent_count && i < MAX_AGENTS; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (strcmp(ag->name, name) != 0)
         continue;
      /* Same eligibility triple delegate_pick_for_role applies — a pinned seat
       * resolves with NO substitution, so an agent that exists but is disabled,
       * lacks the role, or is unroutable reports -1 (caller fails the run). */
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         return -1;
      return i;
   }
   return -1;
}

static int agent_route_candidate_eligible(const agent_t *ag, const char *role,
                                          unsigned required_caps, int min_context,
                                          agent_scope_t scope);

/* A PRIMARY (user-facing) turn must reach the configured default agent whatever
 * its cost_tier. The default is the operator's "most capable seat" choice, and a
 * user must never be handed a weaker model just because a cheaper peer exists —
 * a session the user interacts with has to be coherent before it is cheap.
 *
 * This used to be expressed by consulting `default_agent` on a primary turn and
 * then returning it only from inside the min_tier-filtered pass below, which
 * meant a default above min_tier was never returned. With a premium default
 * (cost_tier 1) and any cheaper peer serving the same role (cost_tier 0), the
 * primary turn silently routed to the cheap peer — the exact opposite of the
 * intent. Resolve the primary default up front instead, subject only to the
 * eligibility every route requires. */
static agent_t *agent_primary_turn_default(agent_config_t *cfg, const char *role)
{
   if (!agent_routing_primary_turn() || !cfg->default_agent[0])
      return NULL;
   agent_t *ag = agent_find(cfg, cfg->default_agent);
   if (!ag || !ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
      return NULL;

   /* Session affinity outranks the default. A tmux agent holds a STATEFUL
    * session; switching a user mid-conversation to an HTTP peer abandons it.
    * The tier-filtered pass below has always preferred tmux, so returning the
    * default before that check would silently break session continuity for a
    * user whose default happens to be an HTTP agent. Only bypass cost_tier
    * here — that is the ordering this fix is about — not the tmux preference. */
   if (strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) != 0)
   {
      /* Decline ONLY for a tmux peer the normal pass would actually select.
       * That pass is tier-filtered and prefers tmux WITHIN min_tier, so a tmux
       * agent at a higher tier never wins there. Declining for any tmux peer at
       * all would abandon the default and then hand the turn to some unrelated
       * cheap agent — losing the default without preserving the session. */
      int min_tier = -1;
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *peer = &cfg->agents[i];
         if (!agent_route_candidate_eligible(peer, role, 0, 0, AGENT_SCOPE_UNSET))
            continue;
         if (min_tier < 0 || peer->cost_tier < min_tier)
            min_tier = peer->cost_tier;
      }
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *peer = &cfg->agents[i];
         if (peer == ag || !agent_route_candidate_eligible(peer, role, 0, 0, AGENT_SCOPE_UNSET))
            continue;
         if (peer->cost_tier == min_tier && strcmp(peer->backend, AGENT_BACKEND_TMUX_CLI) == 0)
            return NULL; /* the normal pass will pick this stateful session */
      }
   }
   return ag;
}

agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   /* Tier is a COST ordering; it must not gate the user-facing seat. */
   agent_t *primary_default = agent_primary_turn_default(cfg, role);
   if (primary_default)
      return primary_default;

   /* First pass: find the minimum tier; note if any tmux agent is there
    * (tmux sessions are stateful and always preferred over HTTP peers). */
   int min_tier = -1;
   int has_tmux = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      if (min_tier < 0 || ag->cost_tier < min_tier)
      {
         min_tier = ag->cost_tier;
         has_tmux = 0;
      }
      if (ag->cost_tier == min_tier && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) == 0)
         has_tmux = 1;
   }
   if (min_tier < 0)
      return NULL;

   /* Second pass: collect candidates at min_tier (tmux-only if any exist). */
   agent_t *candidates[MAX_AGENTS];
   int count = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      if (ag->cost_tier != min_tier)
         continue;
      if (has_tmux && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) != 0)
         continue;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_balanced(candidates, count);
}

/* Route fairly among enabled agents at exactly the given cost_tier. */
agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier)
{
   agent_t *candidates[MAX_AGENTS];
   int count = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || ag->cost_tier != tier || !agent_is_available_for_routing(ag))
         continue;
      if (role && !agent_supports_role(ag, role))
         continue;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_balanced(candidates, count);
}

/* Does this agent's declared ceiling admit work of `scope`?
 *
 * BINDING, like a capability flag and unlike min_context: escalation may relax a
 * context shortfall (a bigger seat is a genuine best effort) but must NEVER relax
 * a scope ceiling. Relaxing it would escalate a whole_task packet INTO the very
 * seat the operator declared unable to handle it - the exact inversion of the
 * requirement. If nothing can serve the packet that is a config error worth
 * surfacing, not something to paper over. */
/* Scope is fixed at DECOMPOSITION time and is part of a packet's identity for its
 * lifetime. A retry, a same-tier fallback or an escalation re-routes the SAME
 * packet and must not alter its scope; widening bounded work into whole_task work
 * is a re-decomposition, which only the orchestrator may do by emitting a new
 * packet. */
static int agent_scope_admits(const agent_t *ag, agent_scope_t scope)
{
   if (!ag || ag->max_scope == AGENT_SCOPE_UNSET)
      return 1; /* no declared ceiling */
   /* An undeclared packet scope resolves to WHOLE_TASK: under uncertainty prefer
    * the capable seat, since over-selecting costs less than a misplacement. */
   agent_scope_t want = scope == AGENT_SCOPE_UNSET ? AGENT_SCOPE_WHOLE_TASK : scope;
   return want <= ag->max_scope;
}

static int agent_satisfies_required_caps(const agent_t *ag, unsigned required_caps, int min_context,
                                         agent_scope_t scope)
{
   if (!ag)
      return 0;
   if (!agent_scope_admits(ag, scope))
      return 0;

   unsigned missing_caps = required_caps;
   if (ag->tools_enabled)
      missing_caps &= ~MODEL_CAP_TOOLS;

   if (required_caps == 0 && min_context <= 0)
      return 1; /* scope already checked above */

   /* An explicit per-agent context_window override (agents.json
    * `middleware.context_window`, set via `aimee agent --ctx` or auto-detected
    * by `ag_probe_slots`) is authoritative for the min_context gate, so a model
    * the capability catalog doesn't know about is a config change rather than a
    * code change to the registry table. */
   int override_ctx = ag->middleware.context_window;

   model_capability_t caps;
   if (model_capability_get(agent_catalog_provider(ag), ag->model, &caps) == 0)
   {
      if (missing_caps != 0)
         return 0;
      /* No catalog entry: the override is the only context signal we have. */
      return min_context <= 0 || (override_ctx > 0 && override_ctx >= min_context);
   }

   if (ag->tools_enabled)
      caps.flags |= MODEL_CAP_TOOLS;
   else
      caps.flags &= ~MODEL_CAP_TOOLS;
   if (required_caps && (caps.flags & required_caps) != required_caps)
      return 0;
   /* NOTE the deliberate ASYMMETRY with the max_scope ceiling above: min_context
    * RELAXES during escalation, max_scope never does. The reason is monotonicity.
    * "More context" is strictly more capable, so escalating to a bigger window is
    * always a genuine best effort. "More scope" is NOT monotone: a whole_task
    * packet is not a bigger version of a bounded one, it is a categorically
    * different shape of work. Relaxing a ceiling would therefore hand the hardest
    * packet to the seat declared least able to serve it. Do not "fix" one of these
    * to match the other.
    *
    * An UNKNOWN context window (0) must not satisfy a positive min_context. The
    * previous `effective_ctx > 0` guard made zero pass the gate, so a model whose
    * window aimee could not establish was admitted for arbitrarily large prompts
    * — the failure looked like success.
    *
    * This predicate FAILS CLOSED: an agent with unprovable capacity is dropped
    * from the candidate set, and when it was the last candidate the caller gets
    * NULL. There is deliberately no escalation-to-primary here yet — that is a
    * routing-layer policy, not a per-agent predicate, and it must land before
    * model_meta_capability_routing is enabled by default, or an operator who
    * turns the flag on can lose a route they previously had. The flag defaults
    * OFF, so this is inert until then. Escape hatches meanwhile: a per-agent
    * middleware.context_window override, or a catalog entry. */
   int effective_ctx = override_ctx > 0 ? override_ctx : caps.context_window;
   /* A tmux-CLI agent (codex / claude-oauth) often carries no resolvable model —
    * the vendor CLI picks it — so fall back to the window its adapter declares
    * before treating the window as unknown. Mirrors agent_meets_filter() in
    * delegate_routing.c; without it the zero-context rule below would strand
    * exactly the CLI-backed agents that never had a catalog model. */
   if (effective_ctx <= 0 && ag->cli_kind[0])
   {
      const provider_cli_adapter_t *adapter = provider_cli_adapter_get(ag->cli_kind);
      if (adapter && adapter->caps.max_context_tokens > 0)
         effective_ctx = adapter->caps.max_context_tokens;
   }
   if (min_context > 0 && effective_ctx <= 0)
      return 0;
   if (min_context > 0 && effective_ctx > 0 && effective_ctx < min_context)
      return 0;
   if (caps.deprecated)
      return 0;
   return 1;
}

/* Route to the cheapest capable agent, filtering by required capability flags and minimum context
 * window when sys_cfg->capability_routing is enabled.  Falls back to plain agent_route
 * when capability routing is disabled. */
static agent_t *agent_route_with_caps_inner(agent_config_t *cfg, const char *role,
                                            const agent_route_policy_t *sys_cfg,
                                            unsigned required_caps, int min_context,
                                            agent_scope_t scope)
{
   if (!sys_cfg || !sys_cfg->capability_routing)
   {
      /* A scope ceiling is CONFIGURATION eligibility, not model-metadata
       * capability routing: it must bind whether or not capability routing is
       * enabled. Returning agent_route() here unconditionally would let a
       * whole_task packet reach a bounded-only seat the moment an operator set
       * model_meta.capability_routing=false. */
      agent_t *plain = agent_route(cfg, role);
      if (!plain || agent_scope_admits(plain, scope))
         return plain;
      /* The cheapest seat is ceiling-barred: retry with the barred agents
       * temporarily withheld so plain cost-tier routing still finds a seat. */
      int saved[MAX_AGENTS];
      for (int i = 0; i < cfg->agent_count && i < MAX_AGENTS; i++)
      {
         saved[i] = cfg->agents[i].enabled;
         if (!agent_scope_admits(&cfg->agents[i], scope))
            cfg->agents[i].enabled = 0;
      }
      agent_t *scoped = agent_route(cfg, role);
      for (int i = 0; i < cfg->agent_count && i < MAX_AGENTS; i++)
         cfg->agents[i].enabled = saved[i];
      return scoped;
   }

   /* Same primary-turn rule as agent_route(): the user-facing seat is chosen by
    * capability, not by price. It must still SATISFY the requirements — a
    * default that cannot hold the prompt or lacks a required modality is not a
    * usable seat — but its cost_tier is irrelevant to that decision. */
   agent_t *primary_default = agent_primary_turn_default(cfg, role);
   if (primary_default && agent_scope_admits(primary_default, scope) &&
       (!(required_caps || min_context > 0) ||
        agent_satisfies_required_caps(primary_default, required_caps, min_context, scope)))
      return primary_default;

   /* prefer_local must be decided BEFORE min_tier, not after. Applying it to the
    * cheapest-tier candidate list only ever preferred a local seat among peers
    * that had already won on price - so an eligible local at tier 1 could never
    * beat a paid remote at tier 0, which is the opposite of "try free local
    * delegates first". Decide up front whether any ELIGIBLE seat is local, and if
    * so treat non-local seats as out of contention for the whole selection. */
   int locals_only = 0;
   if (sys_cfg && sys_cfg->prefer_local)
   {
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *ag = &cfg->agents[i];
         if (!ag->enabled || !agent_supports_role(ag, role) ||
             !agent_is_available_for_routing(ag) || !agent_is_local(ag))
            continue;
         if (!agent_scope_admits(ag, scope))
            continue;
         if ((required_caps || min_context > 0) &&
             !agent_satisfies_required_caps(ag, required_caps, min_context, scope))
            continue;
         locals_only = 1;
         break;
      }
   }

   /* Prefer HEALTHY over DEGRADED, same shape as prefer_local: if any eligible
    * seat is healthy, drop degraded seats from contention. A degraded seat is
    * still a valid fallback - excluded only WHEN a healthy peer can serve the
    * role - so this narrows the cheapest-with-capability choice to reliable seats
    * rather than letting a flapping one keep winning on price. Routing half of
    * the codex quota-outage fix: the breaker backoff keeps a hopeless seat out of
    * the set longer, this keeps a degraded-but-routable seat from winning while a
    * healthy peer exists.
    *
    * The degraded verdict is SNAPSHOT once per agent here and reused below,
    * rather than re-queried in each loop. The predicate reads (and mutates, via
    * the breaker half-open) live catalog health, so re-querying could let a seat
    * cross the healthy/degraded line mid-selection - decide "a healthy seat
    * exists", then drop every seat as degraded by candidate-collection time, and
    * return NULL though a seat existed at decision time. That is the exact
    * decide-against-stale, act-against-live bug this whole change is fixing.
    * Freezing the snapshot makes "a healthy seat exists" imply the candidate loop
    * finds it. A seat that flips to degraded after the snapshot is still routed
    * to, which is harmless - it is a preference, and the seat may well serve. */
   int degraded_snapshot[MAX_AGENTS];
   int healthy_only = 0;
   if (g_route_degraded_filter)
   {
      for (int i = 0; i < cfg->agent_count && i < MAX_AGENTS; i++)
      {
         agent_t *ag = &cfg->agents[i];
         degraded_snapshot[i] = agent_is_degraded(ag);
         if (degraded_snapshot[i] || !ag->enabled || !agent_supports_role(ag, role) ||
             !agent_is_available_for_routing(ag))
            continue;
         if (locals_only && !agent_is_local(ag))
            continue;
         if (!agent_scope_admits(ag, scope))
            continue;
         if ((required_caps || min_context > 0) &&
             !agent_satisfies_required_caps(ag, required_caps, min_context, scope))
            continue;
         healthy_only = 1;
      }
   }

   int min_tier = -1;
   int has_tmux = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      if (locals_only && !agent_is_local(ag))
         continue;
      if (healthy_only && i < MAX_AGENTS && degraded_snapshot[i])
         continue;

      /* The scope ceiling binds even when NOTHING else is required: it is a
       * property of the agent, not of the request's capability demands, so it
       * must not sit behind the required_caps/min_context guard. */
      if (!agent_scope_admits(ag, scope))
         goto next_agent;
      if (required_caps || min_context > 0)
      {
         if (!agent_satisfies_required_caps(ag, required_caps, min_context, scope))
            goto next_agent;
      }

      if (min_tier < 0 || ag->cost_tier < min_tier)
      {
         min_tier = ag->cost_tier;
         has_tmux = 0;
      }
      if (ag->cost_tier == min_tier && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) == 0)
         has_tmux = 1;
   next_agent:;
   }

   if (min_tier < 0)
      return NULL;

   agent_t *candidates[MAX_AGENTS];
   int count = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;
      if (ag->cost_tier != min_tier)
         continue;
      if (has_tmux && strcmp(ag->backend, AGENT_BACKEND_TMUX_CLI) != 0)
         continue;

      if (locals_only && !agent_is_local(ag))
         continue;
      if (healthy_only && i < MAX_AGENTS && degraded_snapshot[i])
         continue;
      if (!agent_scope_admits(ag, scope))
         continue;
      if (required_caps || min_context > 0)
      {
         if (!agent_satisfies_required_caps(ag, required_caps, min_context, scope))
            continue;
      }

      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_balanced(candidates, count);
}

/* Effective context window: the operator's policy ceiling when set, else the
 * model catalog, else a CLI adapter's declared window. Mirrors the resolution
 * order in agent_satisfies_required_caps(). */
static int agent_effective_context(const agent_t *ag)
{
   int declared = agent_declared_context_window(ag);
   if (declared > 0)
      return declared;
   if (ag->cli_kind[0])
   {
      const provider_cli_adapter_t *adapter = provider_cli_adapter_get(ag->cli_kind);
      if (adapter && adapter->caps.max_context_tokens > 0)
         return adapter->caps.max_context_tokens;
   }
   return 0;
}

/* FAIL UPWARD (see the capability-routing design): when capability filtering
 * eliminates every candidate, escalate to the most capable seat rather than
 * returning "no route".
 *
 * Two operator invariants collide here: routing must never fail a request
 * because nothing qualified, and a packet must never go to a model that cannot
 * complete it. When nothing qualifies, both cannot hold — so resolve toward the
 * BEST available agent rather than either failing outright or silently falling
 * back to the cheapest. The cost of being wrong is then "we spent more than
 * necessary", which is visible and recoverable, instead of an outage or a
 * garbage result from an under-powered model.
 *
 * Deliberately NOT agent_route(): that returns the CHEAPEST candidate, which is
 * precisely the seat the capability gate just rejected. Prefer the configured
 * default (the operator's most-capable choice), then the largest effective
 * context window, then the highest cost_tier as a capability proxy. */
/* The single candidate-eligibility predicate shared by ordinary routing and
 * escalation. Factored so the two cannot DRIFT: escalation re-deriving its own
 * copy would silently bypass any gate added to the normal pass later. Note the
 * policy filter (primary_only), the health filter, and the Claude-CLI structural
 * rule all live inside agent_is_available_for_routing(), so they are enforced
 * here for both paths. `min_context` is the only axis escalation relaxes. */
static int agent_route_candidate_eligible(const agent_t *ag, const char *role,
                                          unsigned required_caps, int min_context,
                                          agent_scope_t scope)
{
   if (!ag || !ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
      return 0;
   if (required_caps || min_context > 0)
      return agent_satisfies_required_caps(ag, required_caps, min_context, scope);
   return agent_scope_admits(ag, scope);
}

static agent_t *agent_route_escalate(agent_config_t *cfg, const char *role, unsigned required_caps,
                                     agent_scope_t scope)
{
   agent_t *best = NULL;
   int best_ctx = -1;

   if (cfg->default_agent[0])
   {
      agent_t *def = agent_find(cfg, cfg->default_agent);
      if (def && agent_route_candidate_eligible(def, role, required_caps, 0, scope))
         return def;
   }

   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      /* Escalation only helps a DEGREE shortfall - the prompt exceeds every
       * window - where a bigger seat is a genuine best effort. It cannot help a
       * KIND shortfall: if no agent has tools, none can be conjured, and
       * dispatching anyway trades a clear, actionable config error for a doomed
       * request. So required_caps still bind; only min_context is relaxed. */
      if (!agent_route_candidate_eligible(ag, role, required_caps, 0, scope))
         continue;
      int ctx = agent_effective_context(ag);
      if (!best || ctx > best_ctx || (ctx == best_ctx && ag->cost_tier > best->cost_tier))
      {
         best = ag;
         best_ctx = ctx;
      }
   }
   return best;
}

/* Target for a MISPLACEMENT escalation: the most capable eligible seat strictly
 * dearer than the one whose work failed verification.
 *
 * Most capable, not merely one step up. The escalation allowance is spent once,
 * so there is no second chance to correct an under-shoot - and the operator's
 * rule is that over-selecting beats laddering, since one capable session plus a
 * review costs less in tokens AND wall-clock than another failed attempt.
 *
 * The scope ceiling still BINDS here. An escalation is a placement correction,
 * not a licence to hand a packet to a seat declared unable to serve it.
 * Returns NULL when nothing dearer is eligible - the caller must then fail for
 * review rather than re-running the same class of seat. */
agent_t *agent_route_escalation_target(agent_config_t *cfg, const char *role, int failed_tier,
                                       unsigned required_caps, agent_scope_t scope)
{
   if (!cfg)
      return NULL;
   agent_t *best = NULL;
   int best_ctx = -1;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (ag->cost_tier <= failed_tier)
         continue; /* same class of seat: re-running it proves nothing */
      if (!agent_route_candidate_eligible(ag, role, required_caps, 0, scope))
         continue;
      /* Rank by the operator's DECLARED capability ordering (cost_tier), using
       * the context window only to break ties.
       *
       * This deliberately differs from agent_route_escalate, which ranks by
       * window: that path exists for a DEGREE shortfall - the prompt exceeds
       * every window - so a bigger seat is the genuine best effort. This path is
       * a MISPLACEMENT correction: verification failed because the seat was not
       * good enough, and a larger window does not make a cheaper model better at
       * the work. Ranking by window here sent escalations to a cheaper seat that
       * merely had more context - with real catalog data a mid-tier model can
       * easily out-window the top seat - which is the opposite of "escalate to
       * the most capable" and would burn the one-shot allowance on a seat of
       * roughly the class that just failed. */
      int ctx = agent_effective_context(ag);
      if (!best || ag->cost_tier > best->cost_tier ||
          (ag->cost_tier == best->cost_tier && ctx > best_ctx))
      {
         best = ag;
         best_ctx = ctx;
      }
   }
   return best;
}

agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role,
                               const agent_route_policy_t *sys_cfg, unsigned required_caps,
                               int min_context)
{
   return agent_route_with_caps_scoped(cfg, role, sys_cfg, required_caps, min_context,
                                       AGENT_SCOPE_UNSET);
}

void agent_route_policy_current(agent_route_policy_t *out)
{
   if (!out)
      return;
   out->capability_routing = config_model_meta_capability_routing();
   out->prefer_local = config_prefer_local_agents();
}

agent_t *agent_route_with_caps_scoped(agent_config_t *cfg, const char *role,
                                      const agent_route_policy_t *sys_cfg, unsigned required_caps,
                                      int min_context, agent_scope_t scope)
{
   agent_t *r = agent_route_with_caps_inner(cfg, role, sys_cfg, required_caps, min_context, scope);
   /* Modality caps (vision/pdf/audio) are inferred from prompt text and are
    * best-effort: if no model satisfies them, relax them and route on the hard
    * caps (tools) + min_context rather than returning no route at all. Mirrors
    * delegate_filter_route_capabilities so both routing gates agree. */
   if (!r && sys_cfg && sys_cfg->capability_routing && (required_caps & MODEL_CAP_MODALITY_SOFT))
      r = agent_route_with_caps_inner(cfg, role, sys_cfg, required_caps & ~MODEL_CAP_MODALITY_SOFT,
                                      min_context, scope);
   /* Still nothing: escalate rather than report no route. Only reachable with
    * capability routing ON, so plain cost-tier routing is unaffected. */
   if (!r && sys_cfg && sys_cfg->capability_routing)
   {
      r = agent_route_escalate(cfg, role, required_caps & ~MODEL_CAP_MODALITY_SOFT, scope);
      if (r)
         aimee_log(LOG_INFO, "agent",
                   "capability gate matched no agent for role '%s' (caps=0x%x min_context=%d); "
                   "escalating to '%s'",
                   role ? role : "", required_caps, min_context, r->name);
   }
   return r;
}

/* --- Exec role check --- */

static const char *default_exec_roles[] = {
    /* The roles ANY agent may execute when it declares no explicit exec_roles.
     * Canonical names only: `test` and `implement` were listed here but are
     * ALIASES (delegate_role.c maps them to validate/code), so they could never
     * match a canonicalised role and were dead entries.
     *
     * Novel/songwriter work is a PERSONA concern, not a role. prose, line-edit,
     * lyric, hook, prosody and songform were removed outright: songwriter
     * delegates nothing at all (policy "none") and novel is read-only and
     * declares only continuity/beat-check/review/research, so no persona could
     * ever reach them — yet every agent was exec-eligible for them by default. */
    "deploy", "validate", "diagnose", "execute", "review", "code", "refactor", "draft",
    /* Novel-mode read-only checks the novel persona genuinely delegates. */
    "continuity", "beat-check"};
#define DEFAULT_EXEC_ROLE_COUNT 10

int agent_is_exec_role(const agent_t *agent, const char *role)
{
   if (agent->exec_role_count > 0)
   {
      for (int i = 0; i < agent->exec_role_count; i++)
      {
         if (strcmp(agent->exec_roles[i], role) == 0)
            return 1;
      }
      return 0;
   }
   for (int i = 0; i < DEFAULT_EXEC_ROLE_COUNT; i++)
   {
      if (strcmp(default_exec_roles[i], role) == 0)
         return 1;
   }
   return 0;
}
