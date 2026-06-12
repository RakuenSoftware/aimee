#ifndef DEC_VAULT_STORE_H
#define DEC_VAULT_STORE_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_crypto.h" /* VAULT_KEK_LEN, VAULT_SALT_LEN */

/* vault_store: the on-disk substrate for the credential vault (WP-C.1). One
 * 0600 JSON file per attested PRINCIPAL under <aimee_home>/.vault/, holding the
 * per-principal HKDF salt + a list of envelope-encrypted credentials. Stores
 * ONLY ciphertext: {kdf_version, salt} per principal and
 * {agent, cred, wrapped_dek, nonce, ciphertext, tag} per credential — never the
 * KEK, DEK, or plaintext (D13).
 *
 * The filename is the URL-safe base64 of the principal string, so an
 * attacker-influenced webuser name can neither traverse the path nor collide
 * with another principal. The principal is the only identity key: a credential's
 * AEAD AAD is "principal|agent|cred", so a row/file moved to another principal
 * fails to decrypt (fail-closed).
 *
 * These calls do file I/O but no caching and no key derivation — the caller
 * supplies the KEK (from vault_kek_cache after unlock). Return convention: 0 on
 * success, -1 on error, and a positive sentinel where noted. */

/* A non-secret listing entry (agent + cred name only; never the value). */
typedef struct
{
   char agent[64];
   char cred[64];
} vault_store_entry_t;

/* VAULT_STORE_NO_ENTRY: returned by _get when the principal has no such
 * (agent,cred) — the caller falls back to the env/agents.json path (D15),
 * distinct from -1 (a real error: KEK can't unwrap, corrupt file, decrypt
 * failure) which must fail closed. */
#define VAULT_STORE_NO_ENTRY 1

/* Load the principal's per-principal HKDF salt, creating the vault file (empty
 * credential list) with a fresh random salt if it does not yet exist. The salt
 * is what vault.unlock feeds to vault_kek_derive, so it must be stable once
 * created. 0 on success (salt filled), -1 on error. */
int vault_store_get_or_create_salt(const char *principal, uint8_t salt[VAULT_SALT_LEN]);

/* Encrypt `secret` for (principal, agent, cred) under `kek` and upsert it into
 * the principal's vault file (fresh DEK + nonce each time; existing entry
 * replaced). The vault file must already exist (unlock establishes it). 0 on
 * success, -1 on error. */
int vault_store_set(const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                    const char *cred, const char *secret);

/* Decrypt the (agent, cred) credential for `principal` under `kek` into `out`.
 * Returns 0 on success, VAULT_STORE_NO_ENTRY if no such credential (caller falls
 * back), or -1 on error (wrong KEK, tamper, corrupt file — fail closed). `out` is
 * cleansed on any non-success. */
int vault_store_get(const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                    const char *cred, char *out, size_t out_len);

/* 1 if (principal, agent, cred) exists in the vault file (no decryption), else 0.
 * Lets the caller distinguish "locked" (entry exists but KEK unavailable -> hard
 * error) from "missing" (fall back to env) per D15. */
int vault_store_has_entry(const char *principal, const char *agent, const char *cred);

/* List the principal's stored (agent, cred) pairs into `out` (up to max). Returns
 * the count written (>=0), or -1 on error. No secrets are returned. */
int vault_store_list(const char *principal, vault_store_entry_t *out, int max);

/* Remove the (agent, cred) credential from the principal's vault file. 0 on
 * success (incl. already-absent), -1 on error. */
int vault_store_delete(const char *principal, const char *agent, const char *cred);

#endif /* DEC_VAULT_STORE_H */
