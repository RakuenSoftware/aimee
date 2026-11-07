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

/* One credential in an immutable file snapshot. Buffers remain caller-owned;
 * present is false for an absent entry. Any read/decrypt failure clears all
 * buffers, so a caller can never use half of a credential pair. */
typedef struct
{
   const char *credential;
   char *value;
   size_t capacity;
   int present;
} vault_store_read_t;
#define VAULT_STORE_SNAPSHOT_UNSUPPORTED (-2)
int vault_store_read_snapshot(const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                              const char *agent, vault_store_read_t *reads, size_t count);

/* All fields are staged under one write lock and committed by one atomic file
 * replacement. NULL secret removes a field. Duplicate names and unsupported
 * backends refuse; never emulate this contract with sequential mutations. */
typedef struct
{
   const char *credential;
   const char *secret;
} vault_store_write_t;
#define VAULT_STORE_BATCH_UNSUPPORTED (-2)
int vault_store_write_batch(const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                            const char *agent, const vault_store_write_t *writes, size_t count);

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

/* Strong retained deployment contract: immutable generations plus Retired slots. */
extern const unsigned aimee_vault_immutable_custody_contract_v2;
extern const unsigned aimee_vault_generation_fenced_use_contract_v1;
#define VAULT_IMMUTABLE_GENERATION_CAP 33
#define VAULT_STORE_GENERATION_MISMATCH 3
#define VAULT_STORE_RETIRED 4
#define VAULT_STORE_GENERATION_UNSUPPORTED (-2)
#define VAULT_STORE_USE_FAILED 5 /* callback may already have consumed/acted */
typedef int (*vault_store_secret_use_fn)(const char *secret, size_t length, void *context);

/* Closed result for provider-atomic server-wrapped immutable creation. The
 * exact value comparison occurs inside custody; no value fingerprint escapes. */
typedef enum
{
   VAULT_CREATE_ONCE_CREATED = 0,
   VAULT_CREATE_ONCE_REPLAY = 1,
   VAULT_CREATE_ONCE_CONFLICT = 2,
   VAULT_CREATE_ONCE_RETIRED = 3,
   VAULT_CREATE_ONCE_ERROR = -1,
   VAULT_CREATE_ONCE_UNSUPPORTED = -2,
   VAULT_CREATE_ONCE_BADARG = -3,
   VAULT_CREATE_ONCE_CRYPTO = -4,
   VAULT_CREATE_ONCE_IO = -5,
} vault_create_once_result_t;
/* Creates the principal file and entry in one provider transaction if absent.
 * New entries carry an authenticated immutable generation. Existing unmarked
 * slots conflict; marked equal plaintext returns Replay, unequal Conflict. Both leave
 * ciphertext/file bytes unchanged. Only Created/Replay return generation.
 * Mutable setters/batch cannot overwrite marked entries; deletion is allowed
 * and retains a no-value Retired tombstone: same ID cannot be recreated by
 * delayed retries. A new logical write requires a fresh slot ID.
 * Reserved instance/server principal
 * is Unsupported: it uses the existing primary-wrap contract, not this API.
 * Missing backend support refuses;
 * NEVER emulate with get followed by set. Secret <=64KiB; principal/slot names
 * use existing bounded identifiers. A failed durable commit may have committed:
 * retry the SAME tuple/value to reconcile, never assume failure means absence. */
vault_create_once_result_t vault_store_create_server_once(
    const char *principal, const uint8_t server_kek[VAULT_KEK_LEN], const char *agent,
    const char *cred, const char *secret, char *generation, size_t generation_capacity);
/* Marked entries refuse legacy raw get/get_server/snapshot paths.
 * Trusted low-level versioned use. Expected generation and marker must match;
 * ordinary raw getters do NOT provide this contract. Non-success clears out.
 * Missing backend support is explicit, never emulated with separate reads. */
int vault_store_get_server_generation(const char *principal,
    const uint8_t server_kek[VAULT_KEK_LEN], const char *agent, const char *cred,
    const char *expected_generation, char *out, size_t out_len);

/* Optional provider-backed entry-retirement/write fence spans exact generation
 * check, decryption and one synchronous trusted callback. No plaintext result.
 * Missing support refuses; NEVER substitute the snapshot getter. Callback must
 * return promptly, retain no bytes and not reenter Vault/key/native operations.
 * Known same-thread Vault/key lock reentry fails closed. This is not a timed-IO
 * or native-ACL guard, seal-epoch admission or an exactly-once external effect.
 * Callback failure/VAULT_STORE_USE_FAILED may follow effects: never auto-retry.
 * Existing master-key rotation remains an OFFLINE operation. */
int vault_store_use_server_generation_fenced(const char *principal,
    const uint8_t server_kek[VAULT_KEK_LEN], const char *agent, const char *cred,
    const char *expected_generation, vault_store_secret_use_fn use, void *context);

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

/* Explicit expected-generation retirement. No absent-as-erased
 * result. Confirmed means the matching immutable tombstone was file+directory
 * fsynced under the same provider lock, including replay after unknown I/O. */
typedef enum
{
   VAULT_RETIRE_GENERATION_CONFIRMED = 0,
   VAULT_RETIRE_GENERATION_MISSING = 1,
   VAULT_RETIRE_GENERATION_MISMATCH = 2,
   VAULT_RETIRE_GENERATION_UNSUPPORTED = 3,
   VAULT_RETIRE_GENERATION_BADARG = 4,
   VAULT_RETIRE_GENERATION_IO = 5
} vault_retire_generation_result_t;
vault_retire_generation_result_t
vault_store_retire_server_generation(const char *principal, const char *agent, const char *cred,
                                     const char *expected_generation);
extern const unsigned aimee_vault_generation_retirement_contract_v1;

/* Explicit permanent closure of an immutable slot, including an absent slot.
 * This is stronger than expected-generation retirement: the trusted caller must
 * authorize closure of the SLOT itself, not merely a previously observed value.
 * Under the same provider lock as create-once, persist a value-free tombstone
 * and return its opaque generation only after file+directory durability barriers.
 * Existing mutable/legacy entries refuse; there is no read/delete fallback.
 * No plaintext, selected-value digest, CLI or model capability is returned.
 * A confirmed closure must survive ordinary deletion and prevent every future
 * create-once of this slot. Existing retire-generation missing semantics remain
 * unchanged. On failure the output is empty when capacity permits. */
vault_retire_generation_result_t
vault_store_close_server_slot_once(const char *principal, const char *agent, const char *cred,
                                  char *closed_generation, size_t generation_capacity);
extern const unsigned aimee_vault_slot_closure_contract_v1;

#endif /* DEC_VAULT_STORE_H */
