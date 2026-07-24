#ifndef DEC_KB_VAULT_POLICY_H
#define DEC_KB_VAULT_POLICY_H 1

#include <stddef.h>

/* kb_vault_policy: kb-only custody selection + P7 §3 live-key policy (slice 3b).
 *
 * The kb reads `vault.custody` (config; default "file") and, at startup after the
 * vault store backend is bound, selects the matching custody PROVIDER for the
 * server KEK:
 *   file           -> the default file provider (self-unsealing 0600 master key)
 *   mock           -> the TEST/DEV anchor (seal/unseal state machine; never a
 *                     live-key basis)
 *   tpm2|pkcs11|kms -> a REAL external anchor — declared but NOT yet implemented,
 *                     so selection FAILS CLOSED with a typed error (they land with
 *                     hardware in a later slice).
 * An unknown/typo value is REJECTED (validation), never silently downgraded. */

/* Validate `custody` against the known enum and bind the matching provider. On an
 * unknown value, or an unimplemented anchor (tpm2/pkcs11/kms), returns -1 and
 * writes a human-readable reason into `errbuf`. Returns 0 on success (file/mock).
 * `custody` may be NULL/empty -> treated as "file". Call once at kb startup. */
int kb_vault_policy_select(const char *custody, char *errbuf, size_t errlen);

/* P7 §3 gate: may a LIVE key (CA key / org vendor key) be held in the vault under
 * the currently-selected custody? TRUE only if custody is a REAL external anchor
 * (tpm2/pkcs11/kms) AND that anchor is currently UNSEALED. `file` AND `mock` are
 * ALWAYS false — `mock` can never hold a live key (test/dev only). Since no real
 * anchor is implemented yet, this returns false in every runnable config today. */
int kb_vault_live_keys_allowed(void);

/* The online management-status key additionally requires the KMS provider's
 * signed high-water mark; TPM/PKCS#11 live eligibility is insufficient. */
int kb_vault_management_status_keys_allowed(void);

/* P2b release gate. Production stays disabled until the off-host WORM witness
 * lands; a conspicuous compile-time integration override exists only for CT260. */
int kb_egress_release_allowed(void);

#endif /* DEC_KB_VAULT_POLICY_H */
