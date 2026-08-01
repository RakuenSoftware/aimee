#ifndef AIMEE_SANDBOX_AUDIT_BRIDGE_H
#define AIMEE_SANDBOX_AUDIT_BRIDGE_H 1

/* Install the sandbox degraded-isolation audit hook, forwarding each event
 * (a guarded exec that ran unsandboxed, or was refused, because the sandbox was
 * requested but unavailable) to the audit event bus — the same KIND_AUDIT_ACTION
 * ledger + capture/replay stream the governed-action, guardrail, and vault-access
 * events use. These rare, high-signal events surface when OS isolation the system
 * intended to apply did not.
 *
 * Server-only by design: this bridge is the single object linking sandbox.c to
 * the (D7-confined) audit bus, keeping sandbox.o itself bus-free and linkable into
 * every binary. Call once at server startup, after audit_ensure_key(). */
void sandbox_audit_bridge_install(void);

#endif /* AIMEE_SANDBOX_AUDIT_BRIDGE_H */
