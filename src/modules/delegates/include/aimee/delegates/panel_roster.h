/* panel_roster.h: required delegate-routing policy for panel membership. */
#ifndef AIMEE_DELEGATES_PANEL_ROSTER_H
#define AIMEE_DELEGATES_PANEL_ROSTER_H 1

#include "aimee.h"
#include "agent_types.h"
#include "roundtable_types.h" /* ensemble_panel_t */

/* Eligibility reads the primary provider from config itself; the panel carries
 * only the seats. */
int aimee_panelist_eligible(const agent_t *agent);
void aimee_panel_roster_default(ensemble_panel_t *panel, const agent_config_t *agents);
void aimee_panel_roster_resolve_random(ensemble_panel_t *panel, const agent_config_t *agents);
/* Resolves every $random seat before applying authorization policy. This order
 * is contractual: authorization must inspect selected agents, not placeholders. */
void aimee_panel_roster_filter_authorization(ensemble_panel_t *panel, const agent_config_t *agents);
void aimee_panel_roster_filter_availability(ensemble_panel_t *panel, const agent_config_t *agents);

#endif /* AIMEE_DELEGATES_PANEL_ROSTER_H */
