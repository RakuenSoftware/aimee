/* memory_audit_bridge.c: forwards the server's kb_client memory mutations onto
 * the observability bus. The one place that links kb_client to the (D7-confined)
 * obs_bus — see memory_audit_bridge.h. Only NON-CONTENT fields cross here; the
 * memory content (the PII payload) is never in scope — the hook already reduced
 * the event to identity + outcome. */
#include "memory_audit_bridge.h"

#include <stdio.h>

#include <aimee/audit/audit_action.h> /* audit_args_hash, AUDIT_ARGS_HASH_LEN */
#include "kb_client.h"                /* kb_client_set_memory_audit_hook */
#include <aimee/audit/obs_bus.h>      /* obs_bus_emit, obs_bus_key_fingerprint */

/* kb_client_memory_audit_hook_fn: publish one memory-mutation audit row over the
 * bus (KIND_ACTION -> the audit ledger + capture/replay). NON-SECRET only: actor
 * = the session that made the change (or "memory"), tool = the op, a PII-safe
 * FINGERPRINT of the memory's (kind, key) identity in the command field (insert /
 * supersede only; id-only ops leave it empty), the tier in mode, the confidence
 * in reason_code, the outcome as verdict, and the numeric memory id as task_id.
 * The memory CONTENT is never passed to the hook, and the raw key/kind never
 * reach the ledger (only their hash). */
static void on_memory_mutation(const char *op, int64_t id, const char *tier, const char *kind,
                               const char *key, double confidence, const char *session_id, int ok)
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

   char args_hash[AUDIT_ARGS_HASH_LEN];
   snprintf(args_hash, sizeof args_hash, "v1-");
   audit_args_hash(op, NULL, args_hash, sizeof args_hash);

   const char *actor = session_id && session_id[0] ? session_id : "memory";
   if (obs_bus_commit_action(actor, op, args_hash, command, tier ? tier : "", reason,
                             ok ? "ok" : "fail", (long long)id) != 0)
      fprintf(stderr, "audit: memory mutation WORM commit failed for %s\n", op ? op : "");
   obs_bus_emit(actor, op, args_hash, command, tier ? tier : "", reason, ok ? "ok" : "fail",
                (long long)id);
}

void memory_audit_bridge_install(void)
{
   kb_client_set_memory_audit_hook(on_memory_mutation);
}
