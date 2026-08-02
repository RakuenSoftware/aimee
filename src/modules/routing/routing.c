/* routing.c: agent routing — role dispatch, capability/tier selection, delegate
 * pick, availability and route-block reasons. Extracted from server/agent_config.c
 * (core modularization); the routing surface is declared in the shared
 * agent_config.h contract, which this module implements. The config/auth half of
 * agent_config.c stays in the server and is called through the same header. */
#include "aimee.h"
#include "aimee_home.h"
#include "util.h"
#include "agent_config.h"
#include "vault_principal.h"
#include "vault_service.h"
#include "model_registry.h"
#include "platform_path.h"
#include "provider_cli_adapter.h"
#include "oauth_flow.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "log.h"
#include <fcntl.h>
#include <unistd.h>

static int agent_supports_role(const agent_t *agent, const char *role)
{
   if (agent_has_role(agent, role))
      return 1;

   /* Execution roles can be handled by any enabled agent.  The tools_enabled
    * flag controls whether tool use is offered during execution, not whether
    * the agent is eligible for selection. */
   if (agent_is_exec_role(agent, role))
      return 1;

   return 0;
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

/* Optional route-time delegate-policy filter; see agent_set_route_policy_filter. */
static int (*g_route_policy_filter)(const agent_t *agent) = NULL;

void agent_set_route_policy_filter(int (*fn)(const agent_t *agent))
{
   g_route_policy_filter = fn;
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

/* Pick randomly from an array of candidates using monotonic-clock nanoseconds.
 * Thread-safe: no shared mutable state. */
static agent_t *agent_pick_random(agent_t **candidates, int count)
{
   if (count <= 0)
      return NULL;
   if (count == 1)
      return candidates[0];
   struct timespec _ts;
   clock_gettime(CLOCK_MONOTONIC, &_ts);
   unsigned int seed = (unsigned int)_ts.tv_nsec ^ (unsigned int)_ts.tv_sec;
   return candidates[seed % (unsigned int)count];
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
   return elig[delegate_role_rand() % (unsigned)n];
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

agent_t *agent_route(agent_config_t *cfg, const char *role)
{
   agent_t *def = NULL;
   if (cfg->default_agent[0])
      def = agent_find(cfg, cfg->default_agent);

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
      if (ag == def)
         return ag;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_random(candidates, count);
}

/* Route to a randomly selected enabled agent at exactly the given cost_tier. */
agent_t *agent_route_at_tier(agent_config_t *cfg, const char *role, int tier)
{
   agent_t *def = NULL;
   if (cfg->default_agent[0])
      def = agent_find(cfg, cfg->default_agent);

   agent_t *candidates[MAX_AGENTS];
   int count = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || ag->cost_tier != tier || !agent_is_available_for_routing(ag))
         continue;
      if (role && !agent_supports_role(ag, role))
         continue;
      if (ag == def)
         return ag;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_random(candidates, count);
}

static int agent_satisfies_required_caps(const agent_t *ag, unsigned required_caps, int min_context)
{
   if (!ag)
      return 0;

   unsigned missing_caps = required_caps;
   if (ag->tools_enabled)
      missing_caps &= ~MODEL_CAP_TOOLS;

   if (required_caps == 0 && min_context <= 0)
      return 1;

   /* An explicit per-agent context_window override (agents.json
    * `middleware.context_window`, set via `aimee agent --ctx` or auto-detected
    * by `ag_probe_slots`) is authoritative for the min_context gate, so a model
    * the capability catalog doesn't know about is a config change rather than a
    * code change to the registry table. */
   int override_ctx = ag->middleware.context_window;

   model_capability_t caps;
   if (model_capability_get(ag->provider, ag->model, &caps) == 0)
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
   int effective_ctx = override_ctx > 0 ? override_ctx : caps.context_window;
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
                                            const agent_route_policy_t *sys_cfg, unsigned required_caps,
                                            int min_context)
{
   if (!sys_cfg || !sys_cfg->capability_routing)
      return agent_route(cfg, role);

   int min_tier = -1;
   int has_tmux = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_supports_role(ag, role) || !agent_is_available_for_routing(ag))
         continue;

      if (required_caps || min_context > 0)
      {
         if (!agent_satisfies_required_caps(ag, required_caps, min_context))
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

   agent_t *def = NULL;
   if (cfg->default_agent[0])
      def = agent_find(cfg, cfg->default_agent);

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

      if (required_caps || min_context > 0)
      {
         if (!agent_satisfies_required_caps(ag, required_caps, min_context))
            continue;
      }

      if (ag == def)
         return ag;
      if (count < MAX_AGENTS)
         candidates[count++] = ag;
   }
   return agent_pick_random(candidates, count);
}

agent_t *agent_route_with_caps(agent_config_t *cfg, const char *role, const agent_route_policy_t *sys_cfg,
                               unsigned required_caps, int min_context)
{
   agent_t *r = agent_route_with_caps_inner(cfg, role, sys_cfg, required_caps, min_context);
   /* Modality caps (vision/pdf/audio) are inferred from prompt text and are
    * best-effort: if no model satisfies them, relax them and route on the hard
    * caps (tools) + min_context rather than returning no route at all. Mirrors
    * delegate_filter_route_capabilities so both routing gates agree. */
   if (!r && sys_cfg && sys_cfg->capability_routing &&
       (required_caps & MODEL_CAP_MODALITY_SOFT))
      r = agent_route_with_caps_inner(cfg, role, sys_cfg, required_caps & ~MODEL_CAP_MODALITY_SOFT,
                                      min_context);
   return r;
}
