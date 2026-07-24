#ifndef AIMEE_MEMORY_AUDIT_BRIDGE_H
#define AIMEE_MEMORY_AUDIT_BRIDGE_H 1

/* Install the server-side memory-mutation audit hook, forwarding each memory
 * change the server requests via aimee-kb (insert / update / delete / reject) to
 * the observability bus — the same KIND_ACTION ledger + capture/replay stream the
 * governed-action, guardrail, vault-access, and sandbox events use. This is the
 * SERVER's view of "what the agent chose to remember / change / forget"; aimee-kb
 * records the authoritative event on its own bus at the mutation site.
 *
 * Server-only by design: this bridge is the single object linking kb_client to
 * the (D7-confined) obs_bus, keeping kb_client itself bus-free and linkable into
 * every binary. Only NON-CONTENT fields cross — the memory content / use_cases /
 * reject reason (the PII payload) is never in scope. Call once at server startup,
 * after audit_ensure_key(). */
void memory_audit_bridge_install(void);

#endif /* AIMEE_MEMORY_AUDIT_BRIDGE_H */
