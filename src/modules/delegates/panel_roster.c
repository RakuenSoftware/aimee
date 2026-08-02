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

static int aggregator_is_random(const char *aggregator)
{
   return aggregator && strcmp(aggregator, PANEL_RANDOM_SEAT) == 0;
}

int aimee_panelist_eligible(const agent_t *agent)
{
   if (!agent || !agent->enabled || !agent->name[0])
      return 0;
   /* The primary provider does not sit on its own review panel. Copied out of the
    * accessor's thread-local buffer because both comparisons below read it and an
    * intervening accessor call would move it. */
   char primary[128];
   snprintf(primary, sizeof(primary), "%s", config_provider());
   if (primary[0] && (strcasecmp(agent->name, primary) == 0 ||
                      (agent->provider[0] && strcasecmp(agent->provider, primary) == 0)))
      return 0;
   if (agent->primary_only)
      return 0;
   if (agent_is_claude_cli(agent) && !agent->is_server_hosted)
      return 0;
   return 1;
}

void aimee_panel_roster_default(ensemble_panel_t *panel, const agent_config_t *agents)
{
   if (!panel || !agents || panel->reference_count > 0)
      return;
   int n = 0;
   for (int i = 0; i < agents->agent_count && n < AIMEE_PANEL_MAX_PARTICIPANTS; i++)
   {
      if (!aimee_panelist_eligible(&agents->agents[i]))
         continue;
      snprintf(panel->reference_models[n], sizeof(panel->reference_models[n]), "%s",
               agents->agents[i].name);
      n++;
   }
   panel->reference_count = n;
   if (!panel->aggregator[0] && n > 0)
      snprintf(panel->aggregator, sizeof(panel->aggregator), "%s",
               panel->reference_models[0]);
}

void aimee_panel_roster_resolve_random(ensemble_panel_t *panel, const agent_config_t *agents)
{
   if (!panel || !agents)
      return;
   char models[AIMEE_PANEL_MAX_PARTICIPANTS][sizeof panel->reference_models[0]];
   char personas[AIMEE_PANEL_MAX_PARTICIPANTS][sizeof panel->reference_personas[0]];
   const char *used[AIMEE_PANEL_MAX_PARTICIPANTS];
   int n = 0, nused = 0;

   for (int i = 0; i < panel->reference_count && i < AIMEE_PANEL_MAX_PARTICIPANTS; i++)
      if (!seat_is_random(panel->reference_models[i]))
         used[nused++] = panel->reference_models[i];

   for (int i = 0; i < panel->reference_count && i < AIMEE_PANEL_MAX_PARTICIPANTS; i++)
   {
      const char *persona =
          (i < panel->reference_persona_count) ? panel->reference_personas[i] : "";
      if (!seat_is_random(panel->reference_models[i]))
      {
         snprintf(models[n], sizeof models[n], "%s", panel->reference_models[i]);
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
      snprintf(panel->reference_models[i], sizeof panel->reference_models[i], "%s",
               models[i]);
      snprintf(panel->reference_personas[i], sizeof panel->reference_personas[i],
               "%s", personas[i]);
   }
   panel->reference_count = n;
   panel->reference_persona_count = n;

   if (aggregator_is_random(panel->aggregator))
   {
      int idx = delegate_pick_for_role((agent_config_t *)agents, "review", used, nused);
      if (idx >= 0)
         snprintf(panel->aggregator, sizeof panel->aggregator, "%s",
                  agents->agents[idx].name);
      else
         panel->aggregator[0] = '\0';
   }
}

void aimee_panel_roster_filter_authorization(ensemble_panel_t *panel, const agent_config_t *agents)
{
   if (!panel || !agents)
      return;
   aimee_panel_roster_resolve_random(panel, agents);
   int n = 0;
   for (int i = 0; i < panel->reference_count; i++)
   {
      const char *name = panel->reference_models[i];
      const agent_t *agent = agent_find((agent_config_t *)agents, name);
      if (agent && !aimee_panelist_eligible(agent))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unauthorized panelist '%s' from the roundtable panel", name);
         continue;
      }
      if (n != i)
         snprintf(panel->reference_models[n], sizeof(panel->reference_models[n]),
                  "%s", name);
      n++;
   }
   panel->reference_count = n;
   if (panel->aggregator[0])
   {
      const agent_t *aggregate = agent_find((agent_config_t *)agents, panel->aggregator);
      if (aggregate && !aimee_panelist_eligible(aggregate))
         panel->aggregator[0] = '\0';
   }
   if (!panel->aggregator[0] && n > 0)
      snprintf(panel->aggregator, sizeof(panel->aggregator), "%s",
               panel->reference_models[0]);
}

void aimee_panel_roster_filter_availability(ensemble_panel_t *panel, const agent_config_t *agents)
{
   if (!panel || !agents)
      return;
   int n = 0;
   for (int i = 0; i < panel->reference_count; i++)
   {
      const char *name = panel->reference_models[i];
      const agent_t *agent = agent_find((agent_config_t *)agents, name);
      if (agent && !agent_is_available_for_routing(agent))
      {
         aimee_log(LOG_WARN, "delegate.panel",
                   "dropping unavailable panelist '%s' (no key / unhealthy / command missing)",
                   name);
         continue;
      }
      if (n != i)
         snprintf(panel->reference_models[n], sizeof(panel->reference_models[n]),
                  "%s", name);
      n++;
   }
   panel->reference_count = n;
   if (panel->aggregator[0])
   {
      const agent_t *aggregate = agent_find((agent_config_t *)agents, panel->aggregator);
      if (aggregate && !agent_is_available_for_routing(aggregate))
         panel->aggregator[0] = '\0';
   }
   if (!panel->aggregator[0] && n > 0)
      snprintf(panel->aggregator, sizeof(panel->aggregator), "%s",
               panel->reference_models[0]);
}
