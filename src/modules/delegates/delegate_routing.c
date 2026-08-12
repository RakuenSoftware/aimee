/* delegate_routing.c: shared delegate route override helpers. */
#include "cmd_agent_delegate_impl.h"
#include "aimee_errors.h"
#include "util.h"
#include "model_registry.h"
#include "provider_cli_adapter.h" /* declared context window for tmux-CLI agents */
#include "log.h"
#include <ctype.h>
#include <string.h>
#include <aimee/delegates/delegate_launch_args.h>

static delegate_capability_provider_fn g_capability_provider;

void delegate_routing_register_capability_provider(delegate_capability_provider_fn provider)
{
   g_capability_provider = provider;
}

/* What a prompt implies a model must be able to do is the delegates module's
 * rule; this asks it. It used to be restated here -- the marker lists, the
 * four-chars-per-token estimate, the 4096 threshold -- with nothing keeping the
 * two copies in step.
 *
 * Fails closed: no capabilities and no context floor when the module cannot be
 * reached. Guessing would route work to a model that cannot see half its input,
 * which is worse than routing nothing. */
void delegate_infer_capability_requirements(const char *prompt, int tools_enabled,
                                            unsigned *required_caps_out, int *min_context_out)
{
   unsigned required = 0;
   int min_context = 0;
   if (g_capability_provider)
      (void)g_capability_provider(prompt, tools_enabled, &required, &min_context);
   if (required_caps_out)
      *required_caps_out = required;
   if (min_context_out)
      *min_context_out = min_context;
}

/* Resolve one agent's facts for the router.
 *
 * This is the half that STAYS: it reads the model capability catalog, the
 * agent's own registry record and the vendor CLI adapter. What those facts MEAN
 * -- whether the agent qualifies, which of the three context windows applies,
 * and whether inferred modality should be relaxed -- is decided by the module,
 * which reads none of them.
 *
 * The three windows are all sent rather than resolved here on purpose: the
 * PRECEDENCE between them is part of the rule, and resolving it locally would
 * put half the rule back on this side. */
static unsigned route_agent_facts(const agent_t *ag, int has_role, unsigned *cap_flags_out,
                                  int *catalog_ctx_out, int *cli_ctx_out)
{
   model_capability_t cap;
   int have_cap = model_capability_get(agent_catalog_provider(ag), ag->model, &cap);

   unsigned flags = AIMEE_DELEGATES_RF_ENABLED;
   if (!ag->enabled)
      flags = 0;
   if (has_role)
      flags |= AIMEE_DELEGATES_RF_HAS_ROLE;
   if (have_cap)
      flags |= AIMEE_DELEGATES_RF_HAVE_CAP;
   if (have_cap && cap.deprecated)
      flags |= AIMEE_DELEGATES_RF_DEPRECATED;
   if (ag->tools_enabled)
      flags |= AIMEE_DELEGATES_RF_TOOLS;

   *cap_flags_out = have_cap ? cap.flags : 0u;
   *catalog_ctx_out = have_cap ? cap.context_window : 0;

   /* A tmux-CLI agent usually carries no `model` to resolve a window from -- the
    * vendor's CLI picks the model itself -- so its adapter's declared window is
    * offered as the last candidate. Whether it is USED is the module's call. */
   *cli_ctx_out = 0;
   if (ag->cli_kind[0])
   {
      const provider_cli_adapter_t *adapter = provider_cli_adapter_get(ag->cli_kind);
      if (adapter && adapter->caps.max_context_tokens > 0)
         *cli_ctx_out = adapter->caps.max_context_tokens;
   }
   return flags;
}

/* Disable every agent whose declared ceiling cannot serve this packet's scope.
 *
 * The error deliberately LISTS the fleet and its ceilings: the panel's operational
 * caveat was that "no agent can serve this" leaves the operator guessing unless it
 * says which seats exist and what each is limited to. */
int delegate_filter_route_scope(agent_config_t *cfg, agent_scope_t scope, char *errbuf,
                                size_t errbuf_sz)
{
   if (errbuf && errbuf_sz > 0)
      errbuf[0] = '\0';
   if (!cfg || scope == AGENT_SCOPE_UNSET)
      return 0; /* unset resolves to whole_task at the routing filter */

   int kept = 0;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled)
         continue;
      if (ag->max_scope != AGENT_SCOPE_UNSET && scope > ag->max_scope)
      {
         ag->enabled = 0;
         continue;
      }
      kept++;
   }
   if (kept > 0)
      return 0;

   size_t n = 0;
   n += (size_t)snprintf(errbuf + n, errbuf_sz - n,
                         "no agent can serve scope '%s'. Fleet ceilings:", agent_scope_name(scope));
   for (int i = 0; i < cfg->agent_count && n + 40 < errbuf_sz; i++)
   {
      const char *ceil = cfg->agents[i].max_scope == AGENT_SCOPE_UNSET
                             ? "unbounded"
                             : agent_scope_name(cfg->agents[i].max_scope);
      n += (size_t)snprintf(errbuf + n, errbuf_sz - n, " %s=%s", cfg->agents[i].name, ceil);
   }
   if (n + 60 < errbuf_sz)
      snprintf(errbuf + n, errbuf_sz - n,
               ". Raise a ceiling, lower the packet scope, or add a capable agent.");
   return -1;
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
   if (cfg->agent_count <= 0 || (unsigned)cfg->agent_count > AIMEE_DELEGATES_ROUTEFILTER_MAX_AGENTS)
      return 0;

   /* No agent serves this role at all: not this filter's refusal to phrase. The
    * preflight that follows says something far more useful than "no model
    * supports the required capabilities", so leave the fleet untouched and let
    * it. Refusing here reads as a capability problem when it is a roster one. */
   int role_candidates = 0;
   for (int i = 0; i < cfg->agent_count; i++)
      if (cfg->agents[i].enabled && agent_has_role(&cfg->agents[i], role))
         role_candidates++;
   if (role_candidates == 0)
      return 0;

   unsigned count = (unsigned)cfg->agent_count;
   size_t req_len = AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN +
                    (size_t)count * AIMEE_DELEGATES_ROUTEFILTER_AGENT_LEN;
   size_t resp_cap = 16u + (size_t)count * 4u;
   uint8_t *req = malloc(req_len);
   uint8_t *resp = malloc(resp_cap);
   int *keep = calloc(count, sizeof(*keep));
   if (!req || !resp || !keep)
   {
      free(req);
      free(resp);
      free(keep);
      return 0;
   }

   if (aimee_delegates_routefilter_request_begin(count, required_caps, min_context, drop_deprecated,
                                                 req, req_len) == 0)
   {
      free(req);
      free(resp);
      free(keep);
      return 0;
   }
   for (unsigned i = 0; i < count; i++)
   {
      const agent_t *ag = &cfg->agents[i];
      unsigned cap_flags = 0;
      int catalog_ctx = 0, cli_ctx = 0;
      unsigned flags =
          route_agent_facts(ag, agent_has_role(ag, role), &cap_flags, &catalog_ctx, &cli_ctx);
      aimee_delegates_routefilter_request_set(req, i, flags, cap_flags,
                                              ag->middleware.context_window, catalog_ctx, cli_ctx);
   }

   size_t resp_len = 0;
   int rc = delegate_route_filter_apply(req, req_len, resp, resp_cap, &resp_len);
   free(req);

   int kept = 0, relaxed = 0;
   unsigned effective = required_caps;
   if (rc == 0)
      rc = aimee_delegates_routefilter_response_decode(resp, resp_len, count, &kept, &relaxed,
                                                       &effective, keep, count);
   free(resp);

   if (rc != 0)
   {
      /* No verdict: refuse to route rather than route on requirements nothing
       * checked. The whole point of the filter is that a packet needing tools
       * or a large window does not land on an agent with neither. */
      free(keep);
      if (errbuf && errbuf_sz > 0)
         snprintf(errbuf, errbuf_sz, "could not evaluate model capabilities for role '%s'", role);
      return -1;
   }

   if (relaxed)
   {
      char sc[128];
      model_capability_flags_string(required_caps & MODEL_CAP_MODALITY_SOFT, sc, sizeof(sc));
      aimee_log(LOG_WARN, "delegate.route",
                "no model satisfies inferred modality caps (%s) for role '%s'; routing on hard "
                "caps only",
                sc[0] ? sc : "-", role);
   }

   /* Apply the answer. Only role candidates are touched: an agent that does not
    * serve this role keeps whatever state it had. */
   for (unsigned i = 0; i < count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->enabled || !agent_has_role(ag, role))
         continue;
      if (!keep[i])
         ag->enabled = 0;
   }
   free(keep);

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
      if (role && !agent_has_role(ag, role))
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
      if (role && !agent_has_role(ag, role))
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
   /* Callers load a request-local config today, but reset explicitly so a
    * reused config can never leak a previous request's positive pin. */
   cfg->route_pinned = 0;
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
      if (role && role[0] && !agent_has_role(selected, role))
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
      cfg->route_pinned = 1;
      for (int i = 0; i < cfg->agent_count; i++)
      {
         if (&cfg->agents[i] != selected)
            cfg->agents[i].enabled = 0;
      }
   }
   else if (selected_tier >= 0)
   {
      /* A tier is a pool, not a pin: every enabled role-eligible peer in the
       * selected tier remains a valid substitute. */
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
