/* audit_action.h: governed-action audit primitives (P2 of the governance
 * decision-records + per-action-audit proposal).
 *
 * This slice (S1) provides the tamper-evident argument digest used by the
 * per-action audit row. The row emitter (audit_action_log) and its wiring into
 * pre_tool_check land in S2; the trajectory_export reader in S3.
 *
 * ---- args_hash contract (version "v1-") ------------------------------------
 *
 * The digest is HMAC-SHA256(audit-key, canon), rendered as "v1-<64 lowercase
 * hex>". `canon` is a deterministic serialization of a PER-TOOL ALLOWLIST
 * projection of the tool's JSON arguments:
 *
 *   - Only fields on the tool's allowlist contribute to the hash. Every other
 *     field — including any field a future tool adds — is DROPPED and never
 *     enters the hash. This is an allowlist BY CONSTRUCTION: a new tool, or a
 *     new argument on an existing tool, can never silently leak a secret or PII
 *     value into the append-only audit log.
 *   - A tool with no allowlist entry hashes its NAME ONLY (no argument values).
 *   - The canonical form fixes field order from the allowlist (not from the
 *     input JSON), so it is independent of input key order and insignificant
 *     whitespace without relying on a general sorted-key JSON serializer.
 *   - Inputs are bounded: oversized input, oversized field values, and values
 *     beyond the per-value cap are truncated with a stable marker folded INTO
 *     the hash input, so the digest stays reproducible and verifiable.
 *
 * The digest is keyed (HMAC, not a bare hash) so low-entropy arguments cannot be
 * recovered by dictionary/rainbow attack against the public audit log. The key
 * is dedicated ($AIMEE_HOME/.audit-key) and MUST NOT be the wfe_approval key —
 * the two have different threat models and rotation cadences.
 */
#ifndef AIMEE_AUDIT_ACTION_H
#define AIMEE_AUDIT_ACTION_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* "v1-" (3) + 64 hex + NUL. */
#define AUDIT_ARGS_HASH_LEN 68

/* Compute the args hash for (tool_name, args_json) into `out` (capacity
 * >= AUDIT_ARGS_HASH_LEN). `args_json` may be NULL/empty (hashes the tool name
 * only). Returns 0 on success.
 *
 * Best-effort: on any failure (key unavailable, allocation) it writes the stable
 * sentinel "v1-" followed by 64 '0' and returns -1. Callers audit best-effort
 * and MUST NOT block a tool on a non-zero return. */
int audit_args_hash(const char *tool_name, const char *args_json, char *out, size_t out_sz);

/* Ensure the dedicated audit HMAC key exists at $AIMEE_HOME/.audit-key (0600, 32
 * random bytes), provisioning it atomically if absent (mirrors
 * wfe_approval_ensure_key). Call once at server startup so hash time always has
 * a real key. Returns 0 if the key exists or was created, -1 otherwise. */
int audit_ensure_key(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_ACTION_H */
