/* obs_bus_adapter.c: server-only persistence edges for the shared daemon bus. */
#include "obs_bus_adapter.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/audit/audit_worm.h>

#include "db1_client/guardrail_events.h"

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

static int persist_durable_batch(const obs_bus_durable_record_t *records, size_t count, void *ctx)
{
   (void)ctx;
   _Static_assert(OBS_BUS_DURABLE_BATCH_MAX == AUDIT_WORM_BATCH_MAX, "batch bounds differ");
   if (!records || !count || count > AUDIT_WORM_BATCH_MAX)
      return -1;
   audit_worm_record_t rows[AUDIT_WORM_BATCH_MAX];
   for (size_t i = 0; i < count; i++)
      rows[i] = (audit_worm_record_t){"system",           "event-bus",        records[i].action,
                                      records[i].subject, records[i].verdict, records[i].detail};
   return audit_worm_append_batch(rows, count);
}

int server_obs_bus_configure(void)
{
   if (obs_bus_set_durable_batch_sink(persist_durable, persist_durable_batch, NULL) != 0)
      return -1;
   return obs_bus_set_guardrail_sink(persist_guardrail, NULL);
}
