/* obs_bus_adapter.c: server-only persistence edges for the shared daemon bus. */
#include "obs_bus_adapter.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/audit/audit_worm.h>

#include "guardrail_events.h"

static int persist_guardrail(const guardrail_event_t *event, void *ctx)
{
   (void)ctx;
   /* The DB1 implementation owns its transaction gate, so it is safe to call
    * from the bus consumer thread. */
   return db1_guardrail_event_insert(event);
}

static int persist_durable(const char *actor_role, const char *actor_principal, const char *action,
                           const char *subject, const char *verdict, const char *detail, void *ctx)
{
   (void)ctx;
   return audit_worm_append(actor_role, actor_principal, action, subject, verdict, detail);
}

int server_obs_bus_configure(void)
{
   if (obs_bus_set_durable_sink(persist_durable, NULL) != 0)
      return -1;
   return obs_bus_set_guardrail_sink(persist_guardrail, NULL);
}
