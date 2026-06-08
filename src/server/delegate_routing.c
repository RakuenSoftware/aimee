/* delegate_routing.c: shared delegate route override helpers. */
#include "cmd_agent_delegate_impl.h"
#include "util.h"
#include "model_registry.h"
#include <ctype.h>
#include <string.h>

void delegate_infer_capability_requirements(const char *prompt, int tools_enabled,
                                            unsigned *required_caps_out, int *min_context_out)
{
   unsigned required = 0;
   int min_context = 0;

   if (tools_enabled)
      required |= MODEL_CAP_TOOLS;

   if (prompt && prompt[0])
   {
      if (str_contains_ci(prompt, ".png") || str_contains_ci(prompt, ".jpg") ||
          str_contains_ci(prompt, ".jpeg") || str_contains_ci(prompt, ".webp") ||
          str_contains_ci(prompt, ".gif") || str_contains_ci(prompt, "screenshot") ||
          str_contains_ci(prompt, "image_url") || str_contains_ci(prompt, "!["))
         required |= MODEL_CAP_VISION;
      if (str_contains_ci(prompt, ".pdf") || str_contains_ci(prompt, " pdf "))
         required |= MODEL_CAP_PDF;
      /* Require audio capability only when the prompt references actual audio
       * file formats — not when it merely describes audio features in code.
       * The bare word "audio" was removed because prompts implementing
       * speech/audio features in code (e.g. gateway STT adapter) contain the
       * word "audio" but do not need the model to process audio files. */
      if (str_contains_ci(prompt, ".mp3") || str_contains_ci(prompt, ".wav") ||
          str_contains_ci(prompt, ".m4a") || str_contains_ci(prompt, ".ogg") ||
          str_contains_ci(prompt, ".flac") || str_contains_ci(prompt, ".aac"))
         required |= MODEL_CAP_AUDIO;

      /* Approximate 1 token ~= 4 chars. Only enforce minimum context on
       * materially large prompts so short prompts do not over-filter. */
      size_t chars = strlen(prompt);
      int estimated_tokens = (int)(chars / 4) + 1;
      if (estimated_tokens > 4096)
         min_context = estimated_tokens + 1024;
   }

   if (required_caps_out)
      *required_caps_out = required;
   if (min_context_out)
      *min_context_out = min_context;
}

int delegate_filter_route_capabilities(agent_config_t *cfg, const char *role,
                                       unsigned required_caps, int min_context, int drop_deprecated,
                                       char *errbuf, size_t errbuf_sz)
{
   if (errbuf && errbuf_sz > 0)
      errbuf[0] = '\0';
   if (!cfg || !role || !role[0])
      return 0;
   if (!required_caps && min_context <= 0 && !drop_deprecated)
      return 0;

   int role_candidates = 0;
   int kept = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled)
         continue;
      if (!agent_has_role(ag, role) && !agent_is_exec_role(ag, role))
         continue;
      role_candidates++;

      model_capability_t cap;
      int have_cap = model_capability_get(ag->provider, ag->model, &cap);
      if (have_cap)
      {
         if (ag->tools_enabled)
            cap.flags |= MODEL_CAP_TOOLS;
         else
            cap.flags &= ~MODEL_CAP_TOOLS;
      }
      if (drop_deprecated && have_cap && cap.deprecated)
      {
         ag->enabled = 0;
         continue;
      }
      if (required_caps)
      {
         unsigned effective_flags = have_cap ? cap.flags : 0;
         if (ag->tools_enabled)
            effective_flags |= MODEL_CAP_TOOLS;
         else
            effective_flags &= ~MODEL_CAP_TOOLS;
         if ((effective_flags & required_caps) != required_caps)
         {
            ag->enabled = 0;
            continue;
         }
      }
      if (min_context > 0)
      {
         if (!have_cap || cap.context_window <= 0 || cap.context_window < min_context)
         {
            ag->enabled = 0;
            continue;
         }
      }
      kept++;
   }

   if (role_candidates > 0 && kept == 0)
   {
      char caps[128];
      model_capability_flags_string(required_caps, caps, sizeof(caps));
      if (caps[0] == '\0')
         snprintf(caps, sizeof(caps), "-");
      if (errbuf && errbuf_sz > 0)
         snprintf(errbuf, errbuf_sz,
                  "no configured model supports required capabilities (caps=%s, min_context=%d)",
                  caps, min_context > 0 ? min_context : 0);
      return -1;
   }
   return 0;
}

agent_t *delegate_route_by_provider(agent_config_t *cfg, const char *role, const char *provider)
{
   agent_t *best = NULL;
   agent_t *def = NULL;
   if (!cfg || !provider || !provider[0])
      return NULL;
   if (cfg->default_agent[0])
      def = agent_find(cfg, cfg->default_agent);

   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || strcmp(ag->provider, provider) != 0 ||
          !agent_is_available_for_routing(ag))
         continue;
      if (role && !agent_has_role(ag, role) && !agent_is_exec_role(ag, role))
         continue;
      if (!best || ag->cost_tier < best->cost_tier)
      {
         best = ag;
         continue;
      }
      if (ag->cost_tier == best->cost_tier && def == ag && def != best)
         best = ag;
   }
   return best;
}

/* Highest cost_tier among enabled, routing-available agents that can serve the
 * role (or -1 if none). Used by the delegate_routing bandit to translate a
 * "premium" arm into a concrete tier_override, deployment-independently. */
int delegate_max_cost_tier(agent_config_t *cfg, const char *role)
{
   int max_tier = -1;
   if (!cfg)
      return -1;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_is_available_for_routing(ag))
         continue;
      if (role && !agent_has_role(ag, role) && !agent_is_exec_role(ag, role))
         continue;
      if (ag->cost_tier > max_tier)
         max_tier = ag->cost_tier;
   }
   return max_tier;
}

static void route_err(char *errbuf, size_t errbuf_sz, const char *fmt, const char *a, const char *b)
{
   if (errbuf && errbuf_sz > 0)
      snprintf(errbuf, errbuf_sz, fmt, a ? a : "", b ? b : "");
}

int delegate_add_inline_acp_agent(agent_config_t *cfg, const char *command, const char *args,
                                  const char *role, char *name_out, size_t name_out_sz)
{
   if (name_out && name_out_sz > 0)
      name_out[0] = '\0';
   if (!cfg || !command || !command[0])
      return -1;
   if (cfg->agent_count >= MAX_AGENTS)
      return -1;

   agent_t *a = &cfg->agents[cfg->agent_count];
   memset(a, 0, sizeof(*a));
   snprintf(a->name, sizeof(a->name), "acp:inline");
   snprintf(a->backend, sizeof(a->backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
   snprintf(a->cli_kind, sizeof(a->cli_kind), "acp");
   if (args && args[0])
      snprintf(a->cli_cmd, sizeof(a->cli_cmd), "%s %s", command, args);
   else
      snprintf(a->cli_cmd, sizeof(a->cli_cmd), "%s", command);
   snprintf(a->roles[0], sizeof(a->roles[0]), "%s", (role && role[0]) ? role : "execute");
   a->role_count = 1;
   a->enabled = 1;
   a->tools_enabled = 1; /* ACP agents drive aimee's tools/worktree per the protocol */
   a->max_turns = -1;    /* no declared cap; the role floor applies */
   a->cost_tier = 3;     /* external agent: it bills its own model, outside aimee */

   cfg->agent_count++;
   if (name_out && name_out_sz > 0)
      snprintf(name_out, name_out_sz, "%s", a->name);
   return 0;
}

int delegate_apply_route_overrides(agent_config_t *cfg, const char *role, const char *via_name,
                                   int tier_override, const char *provider_override,
                                   const char *model_override, char *errbuf, size_t errbuf_sz)
{
   if (!cfg)
      return 0;
   if (errbuf && errbuf_sz > 0)
      errbuf[0] = '\0';

   if (via_name && tier_override >= 0)
   {
      route_err(errbuf, errbuf_sz, "%s", "--via and --tier are mutually exclusive", NULL);
      return -1;
   }
   if (provider_override && provider_override[0] && via_name)
   {
      route_err(errbuf, errbuf_sz, "%s", "--provider and --via are mutually exclusive", NULL);
      return -1;
   }
   if (provider_override && provider_override[0] && tier_override >= 0)
   {
      route_err(errbuf, errbuf_sz, "%s", "--provider and --tier are mutually exclusive", NULL);
      return -1;
   }

   agent_t *selected = NULL;
   int selected_tier = -1;
   if (via_name && via_name[0])
   {
      selected = agent_find(cfg, via_name);
      if (!selected)
      {
         route_err(errbuf, errbuf_sz, "no agent named '%s' (see 'aimee agent list')", via_name,
                   NULL);
         return -1;
      }
      if (!agent_is_available_for_routing(selected))
      {
         route_err(errbuf, errbuf_sz,
                   "agent '%s' is currently unavailable (health down after repeated "
                   "failures, or its provider-cli command is missing)",
                   via_name, NULL);
         return -1;
      }
      if (role && role[0] && !agent_has_role(selected, role) && !agent_is_exec_role(selected, role))
      {
         route_err(errbuf, errbuf_sz, "agent '%s' cannot handle role '%s'", via_name, role);
         return -1;
      }
   }
   else if (tier_override >= 0)
   {
      selected = agent_route_at_tier(cfg, role, tier_override);
      if (!selected)
      {
         char tier[32];
         snprintf(tier, sizeof(tier), "%d", tier_override);
         route_err(errbuf, errbuf_sz, "no agent at tier %s for role '%s' (see 'aimee agent list')",
                   tier, role);
         return -1;
      }
      selected = NULL;
      selected_tier = tier_override;
   }
   else if (provider_override && provider_override[0])
   {
      selected = delegate_route_by_provider(cfg, role, provider_override);
      if (!selected)
      {
         route_err(errbuf, errbuf_sz, "no agent for provider '%s' and role '%s'", provider_override,
                   role);
         return -1;
      }
   }

   if (selected)
   {
      for (int i = 0; i < cfg->agent_count; i++)
      {
         if (&cfg->agents[i] != selected)
            cfg->agents[i].enabled = 0;
      }
   }
   else if (selected_tier >= 0)
   {
      for (int i = 0; i < cfg->agent_count; i++)
      {
         if (cfg->agents[i].cost_tier != selected_tier)
            cfg->agents[i].enabled = 0;
      }
   }

   agent_t *target = agent_route(cfg, role);
   if (!target)
   {
      route_err(errbuf, errbuf_sz, "no agent available for role '%s'", role, NULL);
      return -1;
   }
   if (target && model_override && model_override[0])
      snprintf(target->model, sizeof(target->model), "%s", model_override);
   return 0;
}

int delegate_route_preflight(agent_config_t *cfg, const char *role, char *errbuf, size_t errbuf_sz)
{
   if (errbuf && errbuf_sz > 0)
      errbuf[0] = '\0';
   if (!cfg || !role || !role[0])
   {
      route_err(errbuf, errbuf_sz, "%s", "missing delegate role", NULL);
      return -1;
   }
   if (!agent_route(cfg, role))
   {
      route_err(errbuf, errbuf_sz, "no agent available for role '%s'", role, NULL);
      return -1;
   }
   return 0;
}
