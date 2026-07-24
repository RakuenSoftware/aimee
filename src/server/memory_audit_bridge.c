/* memory_audit_bridge.c: forwards the server's kb_client memory mutations onto
 * the observability bus. The one place that links kb_client to the (D7-confined)
 * obs_bus — see memory_audit_bridge.h. Only NON-CONTENT fields cross here; the
 * memory content (the PII payload) is never in scope — the hook already reduced
 * the event to identity + outcome. */
#include "memory_audit_bridge.h"

#include <stdio.h>

#include "audit_action.h" /* audit_args_hash, AUDIT_ARGS_HASH_LEN */
#include "kb_client.h"    /* kb_client_set_memory_audit_hook */
#include "obs_bus.h"      /* obs_bus_emit */
#include "wfe_def.h"      /* wfe_sha256_raw */

/* A PII-safe fingerprint of the memory's (kind, key) identity. BOTH the key and
 * (on the MCP path) the kind are agent-supplied free text that can embed PII or a
 * value — the KB gates memory CONTENT but NOT the key — so the identity is NEVER
 * recorded verbatim. A one-way hash preserves correlation (same identity -> same
 * handle) without exposing it; the numeric memory id (task_id) is the handle for
 * an authorized KB lookup of the details. */
static void identity_fingerprint(const char *kind, const char *key, char *out, size_t out_len)
{
   if (out_len < 16)
   {
      if (out_len)
         out[0] = '\0';
      return;
   }
   char buf[1200];
   int n = snprintf(buf, sizeof buf, "%s\x1f%s", kind ? kind : "", key ? key : "");
   size_t len = (n < 0) ? 0 : ((size_t)n < sizeof buf ? (size_t)n : sizeof buf);
   unsigned char dig[32];
   if (wfe_sha256_raw(buf, len, dig) != 0)
   {
      snprintf(out, out_len, "mk:?");
      return;
   }
   static const char hx[] = "0123456789abcdef";
   out[0] = 'm';
   out[1] = 'k';
   out[2] = ':';
   for (int i = 0; i < 6; i++)
   {
      out[3 + i * 2] = hx[(dig[i] >> 4) & 0xf];
      out[3 + i * 2 + 1] = hx[dig[i] & 0xf];
   }
   out[15] = '\0';
}

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
      identity_fingerprint(kind, key, command, sizeof command);
   else
      command[0] = '\0';

   char reason[48];
   reason[0] = '\0';
   if (confidence > 0.0)
      snprintf(reason, sizeof reason, "conf=%.2f", confidence);

   char args_hash[AUDIT_ARGS_HASH_LEN];
   snprintf(args_hash, sizeof args_hash, "v1-");
   audit_args_hash(op, NULL, args_hash, sizeof args_hash);

   obs_bus_emit(session_id && session_id[0] ? session_id : "memory", op, args_hash, command,
                tier ? tier : "", reason, ok ? "ok" : "fail", (long long)id);
}

void memory_audit_bridge_install(void)
{
   kb_client_set_memory_audit_hook(on_memory_mutation);
}
