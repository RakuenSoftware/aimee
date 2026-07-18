#ifndef DEC_VAULT_STORE_H
#define DEC_VAULT_STORE_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_crypto.h"    /* VAULT_KEK_LEN, VAULT_SALT_LEN */
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX */

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

/* Read an EXISTING principal's salt without creating the vault file. Returns 0 on
 * success, -1 if the vault does not exist or is corrupt. Used by rekey so a
 * password change against a non-existent principal fails closed without
 * materializing an (attacker-controlled) vault. */
int vault_store_salt_readonly(const char *principal, uint8_t salt[VAULT_SALT_LEN]);

/* Validate a derived KEK against the principal's stored key-check verifier — a
 * fixed sentinel wrapped under the KEK — so a wrong password is caught
 * immediately and INDEPENDENTLY of how many credentials exist (an empty vault is
 * the normal fresh state). On the FIRST unlock (no verifier yet) the verifier is
 * established from this KEK and 0 is returned. Thereafter: 0 if the KEK matches,
 * -1 if it does not (wrong password/root key) or the vault is missing/corrupt.
 * Call right after deriving the KEK at unlock, before caching it. */
int vault_store_unlock_check(const char *principal, const uint8_t kek[VAULT_KEK_LEN]);

/* Encrypt `secret` for (principal, agent, cred) under `kek` and upsert it into
 * the principal's vault file (fresh DEK + nonce each time; existing entry
 * replaced). The vault file must already exist (unlock establishes it). 0 on
 * success, -1 on error. */
int vault_store_set(const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
                    const char *cred, const char *secret);

/* Like vault_store_set, but ALSO wraps the per-credential DEK under `server_kek`
 * (stored as "wrapped_dek_server") so the SERVER can decrypt the credential
 * autonomously — dual-access wrap (WP-C.4). `server_kek` must be non-NULL; both
 * wraps are written or the upsert fails (fail-closed: never store a credential
 * the server cannot later read). 0 on success, -1 on error. */
int vault_store_set_dual(const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                         const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
                         const char *cred, const char *secret);

/* Encrypt `secret` for (principal, agent, cred) wrapped ONLY under `server_kek`
 * (the "wrapped_dek_server" field; NO user-KEK wrap) and upsert it into the
 * principal's vault file. The write-side counterpart of vault_store_get_server:
 * lets a per-`principal` credential (e.g. a webuser git SSH key) be SET with no
 * user unlock, yet read back autonomously by the server. The vault file must
 * already exist (the caller creates the salt). 0 on success, -1 on error. */
int vault_store_set_server(const char *principal, const uint8_t server_kek[VAULT_KEK_LEN],
                           const char *agent, const char *cred, const char *secret);

/* Decrypt the (agent, cred) credential for `principal` using the SERVER wrap
 * ("wrapped_dek_server") under `server_kek` — NO user KEK / unlock required. This
 * is the autonomous server use-path. Returns 0 on success, VAULT_STORE_NO_ENTRY
 * if there is no such credential OR the entry predates dual-wrap (no server wrap
 * — caller falls back / backfills at next user unlock), or -1 on error (wrong
 * server_kek, tamper, corrupt file — fail closed). `out` is cleansed on any
 * non-success. */
int vault_store_get_server(const char *principal, const uint8_t server_kek[VAULT_KEK_LEN],
                           const char *agent, const char *cred, char *out, size_t out_len);

/* Backfill the server wrap (WP-C.4) for every credential that lacks one: unwrap
 * each DEK under `user_kek`, wrap it under `server_kek`, add "wrapped_dek_server".
 * Called at unlock (when the user KEK is available) so credentials written before
 * dual-wrap become server-decryptable. Entries already carrying a server wrap are
 * left untouched. Atomic: if ANY unwrap fails (wrong user_kek / tamper) nothing
 * is written and -1 is returned. 0 on success (incl. nothing to do). */
int vault_store_add_server_wraps(const char *principal, const uint8_t user_kek[VAULT_KEK_LEN],
                                 const uint8_t server_kek[VAULT_KEK_LEN]);

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

/* Re-wrap every credential's DEK from `old_kek` to `new_kek` (the password-change
 * path, WP-C.2): unwrap each DEK under old_kek, re-wrap under new_kek; the DEKs,
 * nonces, ciphertext and tags are untouched (no secret is re-encrypted). Atomic:
 * if ANY unwrap fails (wrong old_kek / tamper) nothing is written and -1 is
 * returned (fail-closed). 0 on success (incl. an empty vault). */
int vault_store_rekey(const char *principal, const uint8_t old_kek[VAULT_KEK_LEN],
                      const uint8_t new_kek[VAULT_KEK_LEN]);

/* Re-wrap the named DEK-wrap `field` of every credential under `principal` from
 * `old_kek` to `new_kek` — the master-key rotation path (D13). `field` is the
 * wrap that is held under the SERVER KEK: "wrapped_dek" for the server principal
 * (whose primary wrap IS the server KEK) or "wrapped_dek_server" for a user
 * principal's dual-access wrap. The DEKs, nonces, ciphertext and tags are
 * untouched (a re-wrap, not a re-encrypt). A credential lacking `field` is left
 * as-is. Atomic per principal: if ANY present `field` fails to unwrap under
 * `old_kek` (wrong key / tamper) nothing is written and -1 is returned
 * (fail-closed). On success returns the number of credentials re-wrapped (>=0);
 * a principal whose vault file is absent returns 0. */
int vault_store_rekey_field(const char *principal, const char *field,
                            const uint8_t old_kek[VAULT_KEK_LEN],
                            const uint8_t new_kek[VAULT_KEK_LEN]);

/* Enumerate the principals that currently have a vault file, decoding each
 * `<b64url(principal)>.json` name back to its principal string. Writes up to
 * `max` names into `out` (each up to VAULT_PRINCIPAL_MAX). Returns the count
 * written (>=0), or -1 on error. Used by master-key rotation to re-wrap every
 * principal that may hold a server wrap, not just the "server" principal. */
int vault_store_list_principals(char (*out)[VAULT_PRINCIPAL_MAX], int max);

#endif /* DEC_VAULT_STORE_H */
