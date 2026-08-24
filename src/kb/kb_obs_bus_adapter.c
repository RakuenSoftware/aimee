/* kb_obs_bus_adapter.c: KB-only persistence edge for generic durability rows. */
#include "kb_obs_bus_adapter.h"

#include <aimee/audit/obs_bus.h>

#include "modules/db2/c/kb_audit_worm.h"

static int persist_durable(const char *actor_role, const char *actor_principal, const char *action,
                           const char *subject, const char *verdict, const char *detail, void *ctx)
{
   (void)ctx;
   return db2_kb_audit_append(actor_role, actor_principal, action, subject, verdict, detail);
}

int kb_obs_bus_configure(void)
{
   return obs_bus_set_durable_sink(persist_durable, NULL);
}
