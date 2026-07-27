#ifndef AIMEE_KB_MEMORY_AUDIT_BRIDGE_H
#define AIMEE_KB_MEMORY_AUDIT_BRIDGE_H 1

/* Install the KB-side memory-mutation audit hook. aimee-kb records the
 * AUTHORITATIVE memory event on its OWN observability bus at the store
 * (memory_core_crud), so it captures EVERY caller — agent-driven via kb_client,
 * KB-internal maintenance (conflict-merge, lifecycle, synthesis), and CLI — not
 * just the server-initiated requests the server-side bridge sees. NON-CONTENT
 * only; the key/kind are fingerprinted (obs_bus_key_fingerprint) so no PII
 * reaches the ledger. Call once at aimee-kb startup. */
void kb_memory_audit_bridge_install(void);

#endif /* AIMEE_KB_MEMORY_AUDIT_BRIDGE_H */
