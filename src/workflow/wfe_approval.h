/* wfe_approval.h: non-repudiable human-gate approvals + the gate.human executor.
 *
 * An approval is an artifact a delegate cannot forge: it is HMAC-signed with an
 * operator key at $AIMEE_HOME/.approval-key (mode 0600) that delegate processes
 * have no read access to. The signature binds {work_item, gate, content_hash,
 * timestamp}; editing the artifact changes its hash so a stale approval no
 * longer matches and the gate re-opens. */
#ifndef DEC_WFE_APPROVAL_H
#define DEC_WFE_APPROVAL_H 1

#include <stddef.h>

/* Path to the operator approval key ($AIMEE_HOME/.approval-key). */
const char *wfe_approval_key_path(char *buf, size_t cap);

/* Ensure the approval key exists (generate 32 random bytes, mode 0600 if not).
 * Returns 0 on success. */
int wfe_approval_ensure_key(void);

/* HMAC-SHA256 sign the approval tuple into out_hex (65 bytes). Returns 0 on
 * success, -1 if the key is unavailable. */
int wfe_approval_sign(const char *work_item_id, const char *gate, const char *content_hash,
                      const char *actor, const char *ts, char out_hex[65]);

/* Constant-time verify (the MAC also binds the actor). Returns 1 if valid. */
int wfe_approval_verify(const char *work_item_id, const char *gate, const char *content_hash,
                        const char *actor, const char *ts, const char *sig_hex);

/* Record a signed approval for a gate against the current artifact content hash
 * (stored as a lifecycle_event of kind "approve"). Returns 0 on success. */
int wfe_approval_record(const char *work_item_id, const char *gate, const char *content_hash,
                        const char *actor);

/* Is there a valid, non-stale approval for (work_item, gate) matching
 * content_hash? Returns 1 if yes. */
int wfe_approval_present(const char *work_item_id, const char *gate, const char *content_hash);

/* Register the gate.human executor behind the wfe_iface vtable. */
void wfe_register_human_gate(void);

#endif /* DEC_WFE_APPROVAL_H */
