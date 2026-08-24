/* obs_bus_adapter.c: server-only persistence edges for the shared daemon bus. */
#include "obs_bus_adapter.h"

#include <aimee/audit/obs_bus.h>

#include "db1_client/guardrail_events.h"

static int persist_guardrail(const guardrail_event_t *event, void *ctx)
{
   (void)ctx;
   /* The DB1 implementation owns its transaction gate, so it is safe to call
    * from the bus consumer thread. */
   return db1_guardrail_event_insert(event);
}

int server_obs_bus_configure(void)
{
   return obs_bus_set_guardrail_sink(persist_guardrail, NULL);
}
