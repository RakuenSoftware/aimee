/* vault_audit_bridge.c: forwards vault_service credential-access events onto the
 * audit event bus. The one place that links vault_service to the (D7-confined)
 * audit bus — see vault_audit_bridge.h. Non-secret fields only ever cross here;
 * the plaintext secret is never in scope. */
#include "vault_audit_bridge.h"

#include <stdio.h>

#include "audit_action.h"  /* audit_args_hash, AUDIT_ARGS_HASH_LEN */
#include "audit_bus.h"     /* audit_bus_emit */
#include "vault_service.h" /* vault_audit_hook_fn, vault_status_t/_str, attested_transport_t */

/* Short, stable label for the attestation behind the access (the audit row's
 * `mode`). get/set/delete carry ATTEST_NONE — they run under the already-cached
 * KEK, so the current transport is not meaningful; only unlock is transport-gated. */
static const char *transport_label(attested_transport_t t)
{
   switch (t)
   {
   case ATTEST_NONE:
      return "n/a";
   case ATTEST_TCP_BEARER:
      return "tcp";
   case ATTEST_UDS_PEERCRED:
      return "uds";
   case ATTEST_WEBCHAT_TRUSTED:
      return "webchat";
   case ATTEST_TLS_BEARER:
      return "tls";
   case ATTEST_MTLS_CLIENT:
      return "mtls";
   }
   return "unknown";
}

/* Map a vault outcome to the audit verdict: a real hit is allowed, a clean miss
 * (no such credential -> caller falls back to env) is neither grant nor denial,
 * and everything else (locked / unattested / wrong-transport / crypto) is a
 * denial. The precise status string rides along in the row's reason field. */
static const char *verdict_of(vault_status_t st)
{
   if (st == VAULT_OK)
      return "allow";
   if (st == VAULT_NO_ENTRY)
      return "miss";
   return "deny";
}

/* vault_audit_hook_fn: publish one credential-access audit row over the bus
 * (KIND_AUDIT_ACTION -> the audit ledger + capture/replay). NON-SECRET only:
 * principal (actor), op (tool), the (agent, cred) identity (command + a
 * correlation hash), the transport (mode), and the outcome (verdict + the precise
 * status as reason). No task association -> task_id 0 (the "no task" sentinel). */
static void on_vault_access(const char *op, const char *principal, const char *agent,
                            const char *cred, attested_transport_t transport, vault_status_t st)
{
   char command[256];
   if (agent[0] || cred[0])
      snprintf(command, sizeof command, "%s/%s", agent, cred);
   else
      command[0] = '\0'; /* whole-vault op (unlock): no per-credential identity */

   /* Fingerprint the (agent, cred) identity for correlation without re-storing it
    * (a whole-vault op hashes just the op name). Non-secret either way. */
   char args_hash[AUDIT_ARGS_HASH_LEN];
   snprintf(args_hash, sizeof args_hash, "v1-");
   audit_args_hash(op, command, args_hash, sizeof args_hash);

   audit_bus_emit(principal, op, args_hash, command, transport_label(transport),
                  vault_status_str(st), verdict_of(st), /*task_id=*/0);
}

void vault_audit_bridge_install(void)
{
   vault_service_set_audit_hook(on_vault_access);
}
