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

   /* Build an ARG-FREE command preview for the audit row's human-readable
    * "command" field, into `out` (capacity out_sz; always NUL-terminated).
    *
    * For a shell tool (Bash / execute_script) it extracts the program basename(s)
    * invoked from the `command` argument — skipping leading env-assignments and
    * DROPPING every argument, redirect target, and quoted value — joined with
    * " ; " (e.g. `cd /x && rm -rf /secret` -> "cd ; rm"). Every emitted token is
    * filtered through a program-name charset and length-capped, so NO argument
    * value (path, token, PII, file content) can ever enter the audit log: the
    * preview is safe-by-construction, not merely redacted.
    *
    * For any other tool it writes "" — the row's `tool` field already names the
    * action and the arguments are never surfaced. Best-effort: on any failure
    * (unparseable JSON, oversize input, no room) it writes "". */
   void audit_command_preview(const char *tool_name, const char *args_json, char *out,
                              size_t out_sz);

   /* Ensure the dedicated audit HMAC key exists at $AIMEE_HOME/.audit-key (0600, 32
    * random bytes), provisioning it atomically if absent (mirrors
    * wfe_approval_ensure_key). Call once at server startup so hash time always has
    * a real key. Returns 0 if the key exists or was created, -1 otherwise. */
   int audit_ensure_key(void);

   /* Test-only seam: the internal HMAC-SHA256 used by audit_args_hash, exposed so
    * unit tests can pin it against RFC 4231 known-answer vectors (validates the
    * ipad/opad construction, 64-byte block, and key>64 pre-hash path). Not part of
    * the public API — do not use in product code. Returns 0 on success, -1 on
    * failure (allocation / length overflow). */
   int audit_hmac_sha256_testonly(const unsigned char *key, size_t keylen, const unsigned char *msg,
                                  size_t mlen, unsigned char mac[32]);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_AUDIT_ACTION_H */
