/* kb_memory_audit_bridge.c: forwards aimee-kb's authoritative memory mutations
 * (memory_core_crud) onto aimee-kb's OWN observability bus. The one place linking
 * the memory store to the bus in the KB; the memory module stays bus-free. Only
 * NON-CONTENT fields cross, and the (kind, key) identity is fingerprinted — the
 * raw key/kind, which can embed PII the KB's content gates do not cover, never
 * reach the ledger. Mirrors server/memory_audit_bridge.c, but at the store side
 * (every caller) and via memory_set_audit_hook. */
#include "kb_memory_audit_bridge.h"

#include <stdint.h>
#include <stdio.h>

#include "aimee.h"  /* prerequisites for memory.h (KIND_*, legacy_config_record, ...) */
#include "memory.h" /* memory_set_audit_hook */
#include <aimee/audit/obs_bus.h> /* obs_bus_emit, obs_bus_key_fingerprint */

/* memory_audit_hook_fn: publish one memory-mutation audit row over the KB's bus
 * (KIND_ACTION -> the KB audit ledger + capture/replay). NON-SECRET only: actor =
 * the session that made the change (or "memory"), tool = the op, a PII-safe
 * FINGERPRINT of the (kind, key) identity in command (insert only; id-only ops
 * leave it empty), tier in mode, confidence in reason_code, and the numeric memory
 * id as task_id. The store fires the hook only on success, so verdict is "ok". */
static void on_memory_mutation(const char *op, int64_t id, const char *tier, const char *kind,
                               const char *key, double confidence, const char *session_id)
{
   char command[32];
   if ((kind && kind[0]) || (key && key[0]))
      obs_bus_key_fingerprint(kind, key, command, sizeof command);
   else
      command[0] = '\0';

   char reason[48];
   reason[0] = '\0';
   if (confidence > 0.0)
      snprintf(reason, sizeof reason, "conf=%.2f", confidence);

   obs_bus_emit(session_id && session_id[0] ? session_id : "memory", op, "v1-", command,
                tier ? tier : "", reason, "ok", (long long)id);
}

void kb_memory_audit_bridge_install(void)
{
   memory_set_audit_hook(on_memory_mutation);
}
