/* Server-only adapter from the pure turn-integrity hook to the durable event bus. */
#include "turn_integrity_audit_bridge.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/turn_integrity.h>

#include <stdio.h>
#include <string.h>

static void on_turn_integrity_event(const ti_event_t *event, void *userdata)
{
   (void)userdata;
   if (!event)
      return;
   char subject[192];
   char detail[768];
   int knowledge = strcmp(event->event, "knowledge.invalidated") == 0;
   snprintf(subject, sizeof subject, "%s:%s", knowledge ? "knowledge" : "turn", event->turn_id);
   snprintf(detail, sizeof detail, "%s=%s principal=%s state=%s sequence=%llu detail=%s",
            knowledge ? "domain" : "session", event->session_id, event->principal,
            ti_turn_state_name(event->state), (unsigned long long)event->sequence, event->detail);
   obs_bus_emit_durable_event(event->event, subject,
                              ti_turn_state_terminal(event->state) ? "terminal" : "observed",
                              detail);
}

void turn_integrity_audit_bridge_install(void)
{
   ti_set_event_callback(on_turn_integrity_event, NULL);
}
