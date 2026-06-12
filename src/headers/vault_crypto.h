#ifndef DEC_VAULT_CRYPTO_H
#define DEC_VAULT_CRYPTO_H 1

#include <stddef.h>
#include <stdint.h>

/* vault_crypto: the envelope-encryption primitives for the per-user credential
 * vault (WP-C.1). Pure, OpenSSL-backed, fail-closed — no file I/O, no global
 * state, no logging — so the cryptographic core can be unit-tested exhaustively
 * in isolation before the store (vault file) and cache wire it together.
 *
 * Envelope scheme (D12/D13):
 *   - KEK  = HKDF-SHA256(root_key, salt=per_principal_salt, info=KEK_INFO), 32 B.
 *   - DEK  = a fresh random 32-byte key per credential.
 *   - The secret is AES-256-GCM-encrypted under the DEK with a fresh 96-bit
 *     nonce and AAD = "principal|agent|cred" (binds the ciphertext to its
 *     identity slot — blocks vault-file/row substitution + rollback).
 *   - The DEK is AES-KW-wrapped (RFC 3394, no wrap-nonce hazard) under the KEK.
 * The vault file therefore stores only {wrapped_dek, nonce, ciphertext, tag} +
 * {kdf_version, per_principal_salt} — never the KEK, DEK, or plaintext.
 *
 * Every function returns 0 on success and -1 on ANY failure (bad length, RNG/KDF
 * failure, GCM-tag mismatch, AES-KW integrity failure). On failure, secret
 * outputs are OPENSSL_cleanse'd. Callers MUST treat -1 as fatal for the op and
 * never use a partially-written output. */

#define VAULT_ROOT_KEY_LEN   32 /* client-held high-entropy root key */
#define VAULT_KEK_LEN        32 /* HKDF-derived key-encryption key */
#define VAULT_DEK_LEN        32 /* per-credential data-encryption key */
#define VAULT_GCM_NONCE_LEN  12 /* 96-bit AES-GCM nonce */
#define VAULT_GCM_TAG_LEN    16 /* AES-GCM authentication tag */
#define VAULT_WRAPPED_DEK_LEN 40 /* RFC 3394 wrap of a 32-byte key = 40 bytes */
#define VAULT_SALT_LEN       16 /* per-principal HKDF salt */

/* The HKDF `info` label + a recorded version tag for the vault-file header so
 * the derivation can evolve without ambiguity. */
#define VAULT_KEK_INFO       "aimee-vault-kek-v1"
#define VAULT_KDF_VERSION    "hkdf-sha256-v1"

/* Fill out[0..n) with cryptographically-strong random bytes. 0 on success;
 * -1 (fail-closed) if the RNG is unavailable — callers must abort the vault op,
 * never proceed with a zero/short key or nonce. */
int vault_crypto_random(uint8_t *out, size_t n);

/* Derive the 32-byte KEK from the client root key and per-principal salt via
 * HKDF-SHA256 with the fixed VAULT_KEK_INFO label. root_key_len must be
 * VAULT_ROOT_KEY_LEN. 0 on success, -1 on failure (KEK cleansed). */
int vault_kek_derive(const uint8_t *root_key, size_t root_key_len, const uint8_t *salt,
                     size_t salt_len, uint8_t kek[VAULT_KEK_LEN]);

/* AES-KW (RFC 3394) wrap/unwrap of a DEK under the KEK. Unwrap returns -1 if the
 * built-in integrity check fails (wrong KEK or tampered wrapped_dek) — DEK
 * cleansed. */
int vault_dek_wrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t dek[VAULT_DEK_LEN],
                   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);
int vault_dek_unwrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN],
                     uint8_t dek[VAULT_DEK_LEN]);

/* AES-256-GCM encrypt `plaintext` under `dek`, binding `aad`. A fresh random
 * nonce is generated internally (returned in `nonce`) — the caller never chooses
 * it, so a (DEK, nonce) pair is never reused. ciphertext must have room for
 * pt_len bytes; tag receives the 16-byte authentication tag. 0 on success, -1 on
 * failure (outputs cleansed). aad may be NULL iff aad_len == 0. */
int vault_secret_encrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                         const uint8_t *plaintext, size_t pt_len, uint8_t nonce[VAULT_GCM_NONCE_LEN],
                         uint8_t *ciphertext, uint8_t tag[VAULT_GCM_TAG_LEN]);

/* AES-256-GCM decrypt + authenticate. Writes plaintext ONLY if the GCM tag
 * verifies (EVP_DecryptFinal_ex == 1); on any failure plaintext is cleansed and
 * -1 returned. plaintext must have room for ct_len bytes. */
int vault_secret_decrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                         const uint8_t nonce[VAULT_GCM_NONCE_LEN], const uint8_t *ciphertext,
                         size_t ct_len, const uint8_t tag[VAULT_GCM_TAG_LEN], uint8_t *plaintext);

#endif /* DEC_VAULT_CRYPTO_H */
