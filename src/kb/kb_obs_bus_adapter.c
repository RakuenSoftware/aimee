/* kb_obs_bus_adapter.c: KB-only persistence edge for generic durability rows. */
#include "kb_obs_bus_adapter.h"

#include <aimee/audit/obs_bus.h>

#include "modules/kb/c/kb_audit_worm.h"
#include "modules/kb/c/kb_store.h" /* kb_store_lease_release_idle */

static int persist_durable(const char *actor_role, const char *actor_principal, const char *action,
                           const char *subject, const char *verdict, const char *detail, void *ctx)
{
   (void)ctx;
   return kb_store_kb_audit_append(actor_role, actor_principal, action, subject, verdict, detail);
}

/* The audit writer leases a PostgreSQL session. Return it between batches;
 * the bus consumer must stay free to route the provider's replies. */
static void release_idle_lease(void *ctx)
{
   (void)ctx;
   kb_store_lease_release_idle();
}

int kb_obs_bus_configure(void)
{
   if (obs_bus_set_durable_sink(persist_durable, NULL) != 0)
      return -1;
   return obs_bus_set_sink_idle_hook(release_idle_lease, NULL);
}
