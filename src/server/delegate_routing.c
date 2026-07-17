/* delegate_routing.c: shared delegate route override helpers. */
#include "cmd_agent_delegate_impl.h"
#include "aimee_errors.h"
#include "util.h"
#include "model_registry.h"
#include "provider_cli_adapter.h" /* declared context window for tmux-CLI agents */
#include "log.h"
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
      /* Vision is INFERRED from text and is a SOFT routing preference (see
       * MODEL_CAP_MODALITY_SOFT): the router relaxes it when no model qualifies.
       * Markdown image syntax ("![") is deliberately NOT a trigger — it appears in
       * any doc/diff under review and never means the model must decode an image. */
      if (str_contains_ci(prompt, ".png") || str_contains_ci(prompt, ".jpg") ||
          str_contains_ci(prompt, ".jpeg") || str_contains_ci(prompt, ".webp") ||
          str_contains_ci(prompt, ".gif") || str_contains_ci(prompt, "screenshot") ||
          str_contains_ci(prompt, "image_url"))
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

/* Does this agent satisfy the filter (caps + min_context, and not a dropped
 * deprecated model)? Pure predicate — no mutation — so the relaxation pass below
 * can dry-run it before deciding which capabilities to actually enforce.
 * Effective context window prefers the agent's explicit override (agents.json
 * `middleware.context_window`, via `aimee agent --ctx` or `ag_probe_slots`), then
 * the model capability catalog, so onboarding a model is a config/catalog change
 * not a registry code edit. */
static int agent_meets_filter(const agent_t *ag, unsigned required_caps, int min_context,
                              int drop_deprecated)
{
   model_capability_t cap;
   int have_cap = model_capability_get(ag->provider, ag->model, &cap);
   if (drop_deprecated && have_cap && cap.deprecated)
      return 0;
   unsigned effective_flags = have_cap ? cap.flags : 0;
   if (ag->tools_enabled)
      effective_flags |= MODEL_CAP_TOOLS;
   else
      effective_flags &= ~MODEL_CAP_TOOLS;
   if (required_caps && (effective_flags & required_caps) != required_caps)
      return 0;
   if (min_context > 0)
   {
      int effective_ctx = ag->middleware.context_window > 0 ? ag->middleware.context_window
                                                            : (have_cap ? cap.context_window : 0);
      /* Still no window: a tmux-CLI agent (codex/claude-oauth) usually carries no
       * `model` to resolve one from — the vendor CLI picks the model itself — so
       * fall back to the window its CLI adapter declares. Reached only when the
       * model produced nothing (effective_ctx<=0), so model-backed agents are
       * untouched. Covers records written before context_window was persisted
       * onto the agent (no re-registration needed). */
      if (effective_ctx <= 0 && ag->cli_kind[0])
      {
         const provider_cli_adapter_t *adapter = provider_cli_adapter_get(ag->cli_kind);
         if (adapter && adapter->caps.max_context_tokens > 0)
            effective_ctx = adapter->caps.max_context_tokens;
      }
      if (effective_ctx <= 0 || effective_ctx < min_context)
         return 0;
   }
   return 1;
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
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (ag->enabled && (agent_has_role(ag, role) || agent_is_exec_role(ag, role)))
         role_candidates++;
   }
   if (role_candidates == 0)
      return 0;

   /* Modality caps (vision/pdf/audio) are inferred from prompt TEXT and so are
    * best-effort: if requiring them would disable EVERY role candidate but the
    * hard caps (tools + min_context) alone would keep some, relax the modality
    * caps and route on the hard set — never hard-fail a text task fleet-wide just
    * because it mentioned an image/pdf/audio filename. */
   unsigned eff = required_caps;
   if (required_caps & MODEL_CAP_MODALITY_SOFT)
   {
      int kept_full = 0, kept_hard = 0;
      unsigned hard = required_caps & ~MODEL_CAP_MODALITY_SOFT;
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *ag = &cfg->agents[i];
         if (!ag->enabled || (!agent_has_role(ag, role) && !agent_is_exec_role(ag, role)))
            continue;
         if (agent_meets_filter(ag, required_caps, min_context, drop_deprecated))
            kept_full++;
         if (agent_meets_filter(ag, hard, min_context, drop_deprecated))
            kept_hard++;
      }
      if (kept_full == 0 && kept_hard > 0)
      {
         char sc[128];
         model_capability_flags_string(required_caps & MODEL_CAP_MODALITY_SOFT, sc, sizeof(sc));
         aimee_log(LOG_WARN, "delegate.route",
                   "no model satisfies inferred modality caps (%s) for role '%s'; routing on hard "
                   "caps only",
                   sc[0] ? sc : "-", role);
         eff = hard;
      }
   }

   int kept = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled)
         continue;
      if (!agent_has_role(ag, role) && !agent_is_exec_role(ag, role))
         continue;
      if (!agent_meets_filter(ag, eff, min_context, drop_deprecated))
      {
         ag->enabled = 0;
         continue;
      }
      kept++;
   }

   if (kept == 0)
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
      char rdetail[128];
      agent_route_block_t rblock = agent_routing_block_reason(selected, rdetail, sizeof(rdetail));
      if (rblock != AGENT_ROUTE_OK)
      {
         /* Report the ACTUAL reason routing declined, tagged with the matching
          * aimee error SLUG. Collapsing every cause into "health down / breaker
          * open" sent operators chasing a provider outage when the real block was
          * a delegate-policy decision (Primary Agent Only) or a missing CLI. Slug
          * (not the numeric code) to stay clear of substring-based classifiers. */
         const char *reason;
         int code;
         switch (rblock)
         {
         case AGENT_ROUTE_HEALTH_DOWN:
            reason = "its provider health is marked DOWN (circuit breaker open after repeated "
                     "failures)";
            code = AIMEE_ERR_BREAKER_OPEN;
            break;
         case AGENT_ROUTE_CLIENT_ONLY_CLAUDE:
            reason = "it is a client-only claude CLI agent; only a server-hosted claude can run "
                     "as a delegate";
            code = AIMEE_ERR_DELEGATE_INELIGIBLE;
            break;
         case AGENT_ROUTE_POLICY_EXCLUDED:
            reason = rdetail[0] ? rdetail
                                : "it is excluded by delegate policy (e.g. it is the configured "
                                  "primary provider, which never delegates to itself)";
            code = AIMEE_ERR_DELEGATE_INELIGIBLE;
            break;
         case AGENT_ROUTE_NO_CREDENTIALS:
            reason = "its credentials could not be resolved";
            code = AIMEE_ERR_ROUTE_UNRESOLVED;
            break;
         case AGENT_ROUTE_MISSING_COMMAND:
         default:
            reason = NULL; /* built below with the missing-command detail */
            code = AIMEE_ERR_ROUTE_UNRESOLVED;
            break;
         }
         char emsg[320];
         if (rblock == AGENT_ROUTE_MISSING_COMMAND)
            snprintf(emsg, sizeof(emsg),
                     "agent '%s' cannot be routed: required command '%s' is not on PATH "
                     "[aimee_err=%s]",
                     via_name, rdetail[0] ? rdetail : "(unknown)", aimee_err_slug(code));
         else
            snprintf(emsg, sizeof(emsg), "agent '%s' cannot be routed: %s [aimee_err=%s]", via_name,
                     reason, aimee_err_slug(code));
         route_err(errbuf, errbuf_sz, "%s", emsg, NULL);
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
