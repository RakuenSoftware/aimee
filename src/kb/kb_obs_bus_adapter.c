/* kb_obs_bus_adapter.c: KB-only persistence edge for generic durability rows. */
#include "kb_obs_bus_adapter.h"

#include <aimee/audit/obs_bus.h>

#include "modules/db2/c/kb_audit_worm.h"
#include "modules/db2/c/db2.h" /* db2_lease_release_idle */

static int persist_durable(const char *actor_role, const char *actor_principal, const char *action,
                           const char *subject, const char *verdict, const char *detail, void *ctx)
{
   (void)ctx;
   return db2_kb_audit_append(actor_role, actor_principal, action, subject, verdict, detail);
}

/* db2_kb_audit_append leases a pooled DB2 connection lazily, and the bus
 * consumer thread never ends a unit of work, so the lease outlived every burst
 * and pinned one pool member until the process exited. Handing the release back
 * on the bus's idle edge returns the connection between bursts while keeping it
 * for the duration of one. */
static void release_idle_lease(void *ctx)
{
   (void)ctx;
   db2_lease_release_idle();
}

int kb_obs_bus_configure(void)
{
   if (obs_bus_set_durable_sink(persist_durable, NULL) != 0)
      return -1;
   return obs_bus_set_sink_idle_hook(release_idle_lease, NULL);
}
