/* vault_audit_bridge.c: forwards vault_service credential-access events onto the
 * audit event bus. The one place that links vault_service to the (D7-confined)
 * audit bus — see vault_audit_bridge.h. Non-secret fields only ever cross here;
 * the plaintext secret is never in scope. */
#include "vault_audit_bridge.h"

#include <stdio.h>

#include <aimee/audit/audit_action.h> /* audit_args_hash, AUDIT_ARGS_HASH_LEN */
#include <aimee/audit/obs_bus.h>      /* obs_bus_emit */
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

/* Publish one vault audit row over the bus (KIND_AUDIT_ACTION -> the audit ledger
 * + capture/replay). NON-SECRET only: principal (actor), op (tool), the
 * (agent, cred) identity (command), the transport (mode), and the outcome
 * (verdict + reason). No task association -> task_id 0 (the "no task" sentinel). */
static void emit_vault_row(const char *principal, const char *op, const char *agent,
                           const char *cred, const char *transport, const char *reason,
                           const char *verdict)
{
   if (!agent)
      agent = "";
   if (!cred)
      cred = "";

   /* The (agent, cred) identity is non-secret and rides HUMAN-READABLE in the
    * command field — this is the correlation key for vault rows (query the ledger
    * by actor+tool+command). It is never the secret value. */
   char command[256];
   if (agent[0] || cred[0])
      snprintf(command, sizeof command, "%s/%s", agent, cred);
   else
      command[0] = '\0'; /* whole-vault op (unlock): no per-credential identity */

   /* args_hash is the standard keyed per-op fingerprint. NOTE it does NOT fold in
    * the (agent, cred) identity: audit_args_hash only projects allowlisted tool
    * fields and these vault ops are not allowlisted, so it is tool-name-only. That
    * is fine — the identity is already carried, queryable, in `command` above; the
    * hash just keeps the row schema uniform with the governed-action rows. */
   char args_hash[AUDIT_ARGS_HASH_LEN];
   snprintf(args_hash, sizeof args_hash, "v1-");
   audit_args_hash(op, NULL, args_hash, sizeof args_hash);

   obs_bus_emit(principal, op, args_hash, command, transport, reason, verdict, /*task_id=*/0);
}

/* vault_audit_hook_fn: publish one credential-ACCESS audit row over the bus. */
static void on_vault_access(const char *op, const char *principal, const char *agent,
                            const char *cred, attested_transport_t transport, vault_status_t st)
{
   emit_vault_row(principal, op, agent, cred, transport_label(transport), vault_status_str(st),
                  verdict_of(st));
}

/* Server-principal credential WRITE (POST /v1/vault/set_server and the agent
 * API-key writes). These do not pass through vault_service's access hook — they
 * are a distinct, higher-privilege op: a CLIENT-SUPPLIED secret stored under the
 * server-owned principal for autonomous decrypt. Without this they reached only
 * the local audit_log file and never the ordered tap, capture/replay, or the WORM
 * ledger that every other vault row lands in.
 *
 * `fingerprint` is the caller's non-secret key fingerprint and rides in the
 * reason field; the plaintext secret is never in scope here. Reaching this
 * function means the write was authorized and performed, so the verdict is
 * "allow" — denials are rejected upstream before any write occurs. */
void vault_audit_bridge_server_write(const char *principal, const char *agent, const char *cred,
                                     const char *fingerprint, const char *transport)
{
   char reason[64];
   snprintf(reason, sizeof reason, "fp=%s", fingerprint ? fingerprint : "?");
   emit_vault_row(principal, "vault.set_server", agent, cred, transport ? transport : "unknown",
                  reason, "allow");
}

/* Server-principal credential DELETE (POST /v1/vault/delete). vault_service's own
 * row for this op is attributed to VAULT_SERVER_PRINCIPAL — the vault the
 * credential lives in — so it records WHAT was removed but not WHO removed it.
 * This publishes the human principal for the same (agent, cred), which is the
 * attribution an operator actually needs when a shared credential disappears.
 *
 * Reaching this function means the capability check passed and the delete
 * succeeded, so the verdict is "allow". */
void vault_audit_bridge_server_delete(const char *principal, const char *agent, const char *cred)
{
   emit_vault_row(principal, "vault.delete_server", agent, cred, "n/a", "deleted", "allow");
}

/* Shared-credential ENUMERATION (POST /v1/vault/list). No single (agent, cred)
 * identity applies, so the command field stays empty and the returned count rides
 * in the reason — enough to see who enumerated the shared vault and how much they
 * saw, without naming the credentials in the audit row. */
void vault_audit_bridge_server_list(const char *principal, int count)
{
   char reason[32];
   snprintf(reason, sizeof reason, "count=%d", count);
   emit_vault_row(principal, "vault.list_server", "", "", "n/a", reason, "allow");
}

void vault_audit_bridge_install(void)
{
   vault_service_set_audit_hook(on_vault_access);
}
