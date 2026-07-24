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

#endif /* AIMEE_VAULT_AUDIT_BRIDGE_H */
