/* panel_roster.c -- panel membership is required delegate-routing policy. */
#include "aimee.h"

#include <aimee/delegates/panel_roster.h>
#include <aimee/ir/panel_result.h>

#include "agent_config.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define PANEL_RANDOM_SEAT "$random"

static int seat_is_random(const char *seat)
{
   return !seat || !seat[0] || strcmp(seat, PANEL_RANDOM_SEAT) == 0;
}

int aimee_panelist_eligible(const config_t *cfg, const agent_t *agent)
{
   if (!cfg || !agent || !agent->enabled || !agent->name[0])
      return 0;
   if (cfg->provider[0] &&
       (strcasecmp(agent->name, cfg->provider) == 0 ||
        (agent->provider[0] && strcasecmp(agent->provider, cfg->provider) == 0)))
      return 0;
   if (agent->primary_only)
      return 0;
   if (agent_is_claude_cli(agent) && !agent->is_server_hosted)
      return 0;
   return 1;
}

void aimee_panel_roster_default(config_t *cfg, const agent_config_t *agents)
{
   if (!cfg || !agents || cfg->ensemble_reference_count > 0)
      return;
   int n = 0;
   for (int i = 0; i < agents->agent_count && n < AIMEE_PANEL_MAX_PARTICIPANTS; i++)
   {
      if (!aimee_panelist_eligible(cfg, &agents->agents[i]))
         continue;
      snprintf(cfg->ensemble_reference_models[n], sizeof(cfg->ensemble_reference_models[n]), "%s",
               agents->agents[i].name);
      n++;
   }
   cfg->ensemble_reference_count = n;
   if (!cfg->ensemble_aggregator[0] && n > 0)
      snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s",
               cfg->ensemble_reference_models[0]);
}

void aimee_panel_roster_resolve_random(config_t *cfg, const agent_config_t *agents)
{
   if (!cfg || !agents)
      return;
   char models[AIMEE_PANEL_MAX_PARTICIPANTS][sizeof cfg->ensemble_reference_models[0]];
   char personas[AIMEE_PANEL_MAX_PARTICIPANTS][sizeof cfg->ensemble_reference_personas[0]];
   const char *used[AIMEE_PANEL_MAX_PARTICIPANTS];
   int n = 0, nused = 0;

   for (int i = 0; i < cfg->ensemble_reference_count && i < AIMEE_PANEL_MAX_PARTICIPANTS; i++)
      if (!seat_is_random(cfg->ensemble_reference_models[i]))
         used[nused++] = cfg->ensemble_reference_models[i];

   for (int i = 0; i < cfg->ensemble_reference_count && i < AIMEE_PANEL_MAX_PARTICIPANTS; i++)
   {
      const char *persona =
          (i < cfg->ensemble_reference_persona_count) ? cfg->ensemble_reference_personas[i] : "";
      if (!seat_is_random(cfg->ensemble_reference_models[i]))
      {
         snprintf(models[n], sizeof models[n], "%s", cfg->ensemble_reference_models[i]);
         snprintf(personas[n], sizeof personas[n], "%s", persona);
         n++;
         continue;
      }
      int idx = delegate_pick_for_role((agent_config_t *)agents, "review", used, nused);
      if (idx < 0)
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "no review-capable agent left for a $random seat -> dropping it");
         continue;
      }
      snprintf(models[n], sizeof models[n], "%s", agents->agents[idx].name);
      snprintf(personas[n], sizeof personas[n], "%s", persona);
      if (nused < AIMEE_PANEL_MAX_PARTICIPANTS)
         used[nused++] = agents->agents[idx].name;
      n++;
   }

   for (int i = 0; i < n; i++)
   {
      snprintf(cfg->ensemble_reference_models[i], sizeof cfg->ensemble_reference_models[i], "%s",
               models[i]);
      snprintf(cfg->ensemble_reference_personas[i], sizeof cfg->ensemble_reference_personas[i],
               "%s", personas[i]);
   }
   cfg->ensemble_reference_count = n;
   cfg->ensemble_reference_persona_count = n;

   if (seat_is_random(cfg->ensemble_aggregator))
   {
      int idx = delegate_pick_for_role((agent_config_t *)agents, "review", used, nused);
      if (idx >= 0)
         snprintf(cfg->ensemble_aggregator, sizeof cfg->ensemble_aggregator, "%s",
                  agents->agents[idx].name);
      else
         cfg->ensemble_aggregator[0] = '\0';
   }
}

void aimee_panel_roster_filter_authorization(config_t *cfg, const agent_config_t *agents)
{
   if (!cfg || !agents)
      return;
   aimee_panel_roster_resolve_random(cfg, agents);
   int n = 0;
   for (int i = 0; i < cfg->ensemble_reference_count; i++)
   {
      const char *name = cfg->ensemble_reference_models[i];
      const agent_t *agent = agent_find((agent_config_t *)agents, name);
      if (agent && !aimee_panelist_eligible(cfg, agent))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unauthorized panelist '%s' from the roundtable panel", name);
         continue;
      }
      if (n != i)
         snprintf(cfg->ensemble_reference_models[n], sizeof(cfg->ensemble_reference_models[n]),
                  "%s", name);
      n++;
   }
   cfg->ensemble_reference_count = n;
   if (cfg->ensemble_aggregator[0])
   {
      const agent_t *aggregate = agent_find((agent_config_t *)agents, cfg->ensemble_aggregator);
      if (aggregate && !aimee_panelist_eligible(cfg, aggregate))
         cfg->ensemble_aggregator[0] = '\0';
   }
   if (!cfg->ensemble_aggregator[0] && n > 0)
      snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s",
               cfg->ensemble_reference_models[0]);
}

void aimee_panel_roster_filter_availability(config_t *cfg, const agent_config_t *agents)
{
   if (!cfg || !agents)
      return;
   int n = 0;
   for (int i = 0; i < cfg->ensemble_reference_count; i++)
   {
      const char *name = cfg->ensemble_reference_models[i];
      const agent_t *agent = agent_find((agent_config_t *)agents, name);
      if (agent && !agent_is_available_for_routing(agent))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unavailable panelist '%s' (no key / unhealthy / command missing)",
                   name);
         continue;
      }
      if (n != i)
         snprintf(cfg->ensemble_reference_models[n], sizeof(cfg->ensemble_reference_models[n]),
                  "%s", name);
      n++;
   }
   cfg->ensemble_reference_count = n;
   if (cfg->ensemble_aggregator[0])
   {
      const agent_t *aggregate = agent_find((agent_config_t *)agents, cfg->ensemble_aggregator);
      if (aggregate && !agent_is_available_for_routing(aggregate))
         cfg->ensemble_aggregator[0] = '\0';
   }
   if (!cfg->ensemble_aggregator[0] && n > 0)
      snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s",
               cfg->ensemble_reference_models[0]);
}
