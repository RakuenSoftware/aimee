#ifndef AIMEE_VAULT_AUDIT_BRIDGE_H
#define AIMEE_VAULT_AUDIT_BRIDGE_H 1

/* Install the vault credential-access audit hook, forwarding each access op
 * (unlock / get / set / delete, successes AND denials) to the audit event bus —
 * the same KIND_AUDIT_ACTION ledger + capture/replay stream the governed-action
 * rows use, so "who read/mutated which credential, over what channel, with what
 * outcome" joins the one tamper-evident, replayable audit trail.
 *
 * Server-only by design: this bridge is the SINGLE object that depends on both
 * vault_service and the (D7-confined) audit bus, which is what keeps
 * vault_service.o itself bus-free and linkable into every binary. Call once at
 * server startup, after audit_ensure_key(); the bus lazily starts on the first
 * emit and drains at graceful exit. */
void vault_audit_bridge_install(void);

/* Publish a server-principal credential WRITE onto the same audit bus stream.
 * Unlike the access ops above there is no vault_service hook to install — the
 * server-principal write is driven from the HTTP layer, so the call site invokes
 * this directly. Kept here, beside the access hook, so this file remains the
 * SINGLE object depending on both the vault surface and the audit bus.
 *
 * NON-SECRET arguments only: `fingerprint` is a key fingerprint, never the key. */
void vault_audit_bridge_server_write(const char *principal, const char *agent, const char *cred,
                                     const char *fingerprint, const char *transport);

/* Publish a shared-credential DELETE, attributed to the human principal rather
 * than to the server vault the credential lived in. Same reason as above: the
 * actor must reach the ordered tap and the ledger, not just a local file. */
void vault_audit_bridge_server_delete(const char *principal, const char *agent, const char *cred);

/* Publish a shared-credential enumeration, attributed to the human principal.
 * `count` is how many names were returned; no credential names enter the row. */
void vault_audit_bridge_server_list(const char *principal, int count);

#endif /* AIMEE_VAULT_AUDIT_BRIDGE_H */
