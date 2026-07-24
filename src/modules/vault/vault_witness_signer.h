#ifndef AIMEE_VAULT_WITNESS_SIGNER_H
#define AIMEE_VAULT_WITNESS_SIGNER_H

#include <stddef.h>
#include <stdint.h>

#include "vault_witness_checkpoint.h"

/* P7-witness-e2: the vault-held witness checkpoint signer.
 *
 * Per architect review 11384, the checkpoint is signed by a vault-held Ed25519
 * key (not an external KMS): here signatures are transport-integrity and rotation
 * hygiene, not the load-bearing defense (comparison against retained copies is),
 * so the hot cadence must not couple to KMS availability. The key is derived
 * deterministically from the server KEK by domain-separated HKDF, so it inherits
 * the KEK's custody sealing, rotates with the KEK, needs no separate storage, and
 * its public key is the out-of-band anchor consumers pin.
 *
 * The private seed lives only transiently on the stack during a sign, is `mlock`ed
 * while live, and is `OPENSSL_cleanse`d immediately after. It never persists and
 * never leaves this module. This is the `vault_witness_sign` seam the review
 * called for: a future KMS implementation drops in behind the same functions with
 * no schema change.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#define VAULT_WITNESS_SIGNER_SEED_INFO "aimee-vault-witness-signing-ed25519-v1"
#define VAULT_WITNESS_SIGNER_ID_LABEL "aimee-vault-witness-signer-id-v1"

/* KEK-parameterized cores (pure, deterministic — the same KEK always yields the
 * same keypair, so they are directly unit-testable with a fixed KEK). The seed is
 * HKDF-SHA256(ikm=kek, info=SEED_INFO); the key id is the leading 16 bytes of
 * SHA-256(ID_LABEL || pubkey). Return 0 on success, -1 on failure (outputs
 * cleansed). */
int vault_witness_signer_identity_from_kek(const uint8_t kek[32],
                                           uint8_t pub[VAULT_WITNESS_ED25519_PUB_LEN],
                                           uint8_t key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN]);
int vault_witness_signer_sign_from_kek(const uint8_t kek[32], const uint8_t *msg, size_t msg_len,
                                       uint8_t sig[VAULT_WITNESS_ED25519_SIG_LEN]);

/* Production entry points: fetch the server KEK (vault_server_kek), call the core,
 * and cleanse the KEK. Return 0 on success, -1 on any failure. */
int vault_witness_signer_identity(uint8_t pub[VAULT_WITNESS_ED25519_PUB_LEN],
                                  uint8_t key_id[VAULT_WITNESS_SIGNER_KEY_ID_LEN]);

/* Populate the checkpoint's signer_key_id / sig_alg / sig_version from the current
 * witness key, compute its signable body, and sign it in place. The private key
 * never surfaces to the caller. Returns 0 on success, -1 on failure. */
int vault_witness_checkpoint_sign(vault_witness_checkpoint_t *cp);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_VAULT_WITNESS_SIGNER_H */
