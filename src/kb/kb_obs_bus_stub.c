/* kb_obs_bus_stub.c: aimee-kb-only stubs for obs_bus sinks the KB never drives.
 *
 * aimee-kb runs its OWN obs_bus instance to record the authoritative memory-
 * mutation events at the store (memory_core_crud). Those events use the ACTION
 * kind -> audit_action_log (log.c, already linked into aimee-kb). obs_bus's
 * consumer, however, is one translation unit that ALSO dispatches the GUARDRAIL
 * kind to db1_guardrail_event_insert — a db1 sink that lives only in aimee-server
 * and that the KB never emits. The call is dead code in the KB (the KB subscribes
 * to and publishes only the action kind), but the symbol is still referenced at
 * link time, so this stub satisfies the linker without pulling db1 into aimee-kb.
 * If it were ever reached it fails closed (returns -1). */
#include "guardrail_events.h"

int db1_guardrail_event_insert(const guardrail_event_t *e)
{
   (void)e;
   return -1; /* never reached in aimee-kb; fail closed if it somehow is */
}
