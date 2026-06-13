#ifndef DEC_VAULT_SERVICE_H
#define DEC_VAULT_SERVICE_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_principal.h" /* attested_transport_t */
#include "vault_store.h"     /* vault_store_entry_t */

/* vault_service: the credential-vault business logic — gating + unlock/set/get/
 * list/delete — sitting between the thin server route handlers / delegate
 * use-path and the crypto/cache/store primitives (WP-C.1). Kept free of cJSON
 * and connection I/O so the security gates (fail-closed on un-attested or locked
 * vaults, locked-vs-missing per D15) are unit-testable directly.
 *
 * The PRINCIPAL is the only identity key — supplied by the caller from the
 * attested conn (WP-C.0), never a client-controlled value. */

typedef enum
{
   VAULT_OK = 0,
   VAULT_NO_ENTRY,       /* no such credential — caller falls back to env/agents.json (D15) */
   VAULT_ERR_UNATTESTED, /* empty principal / un-attested conn — refuse (no uid:0) */
   VAULT_ERR_TRANSPORT,  /* operation not permitted on this transport (e.g. root-key over TCP) */
   VAULT_ERR_LOCKED,     /* entry exists but the KEK is not cached/expired — HARD error (D15) */
   VAULT_ERR_BADARG,     /* missing/oversize argument */
   VAULT_ERR_CRYPTO,     /* KDF/decrypt/entropy failure — fail closed */
   VAULT_ERR_IO,         /* vault file read/write failure */
} vault_status_t;

/* A short human-readable label for a status (for error responses/logging). */
const char *vault_status_str(vault_status_t s);

/* Unlock the `uid:` vault: derive the KEK from the client root key + the
 * principal's stored salt and cache it (TTL'd). Requires an attested UDS peer
 * (ATTEST_UDS_PEERCRED) with a non-empty principal and a 32-byte root key — a
 * root key over any other transport is refused (VAULT_ERR_TRANSPORT). The caller
 * must OPENSSL_cleanse `root_key`. (The webuser: KEK is derived from the login
 * password at requireAuth in WP-C.2, not here.) */
vault_status_t vault_service_unlock(const char *principal, attested_transport_t transport,
                                    const uint8_t *root_key, size_t root_key_len, long now_epoch);

/* Unlock the `webuser:` vault (WP-C.2): derive the KEK from the login password
 * via scrypt and cache it. Requires an ATTEST_WEBCHAT_TRUSTED principal (the
 * webchat backend's server.token-gated assertion) with a non-empty principal and
 * password — refused on any other transport (VAULT_ERR_TRANSPORT). The caller
 * must OPENSSL_cleanse `password`. */
vault_status_t vault_service_unlock_password(const char *principal, attested_transport_t transport,
                                             const uint8_t *password, size_t password_len,
                                             long now_epoch);

/* Re-wrap a webuser vault on a login-password change (WP-C.2): derive the old +
 * new KEKs (scrypt over the principal's stored salt) and re-wrap every DEK; the
 * stored credentials are NOT re-encrypted. On success the new KEK is cached (the
 * vault stays unlocked under the new password). Requires ATTEST_WEBCHAT_TRUSTED.
 * Fail-closed: a wrong old password (KEK can't unwrap) leaves the vault
 * untouched. The caller OPENSSL_cleanse's both passwords. */
vault_status_t vault_service_rekey_password(const char *principal, attested_transport_t transport,
                                            const uint8_t *old_password, size_t old_len,
                                            const uint8_t *new_password, size_t new_len,
                                            long now_epoch);

/* Store `secret` for (agent, cred) under the principal's cached KEK. Requires an
 * unlocked vault (VAULT_ERR_LOCKED otherwise). */
vault_status_t vault_service_set(const char *principal, const char *agent, const char *cred,
                                 const char *secret, long now_epoch);

/* The SERVER-owned principal (WP-C.4): a vault whose KEK is the server master key
 * (vault_server_key), needing no client root key / login / unlock. This is where
 * delegate credentials pushed from a remote (TCP) thin client live, so the server
 * can read them autonomously over any transport. Encrypted at rest like any other
 * vault; the server-key trust boundary applies (see vault_server_key.h).
 *
 * TENANCY (deliberate): this is a SINGLE shared namespace, not per-user. That is
 * correct because aimee-server is one person's personal agent and the TCP path is
 * gated only by the deploy bearer — i.e. a single trust domain (a TCP caller that
 * can push here can already drive the server). Webchat users, who DO share one
 * server under distinct logins, get isolated `webuser:` vaults and never land
 * here. If aimee-server ever becomes multi-tenant over TCP (distinct identities
 * behind one listener), this principal MUST be namespaced by the attested
 * identity, or one tenant's delegate would resolve another's key. */
#define VAULT_SERVER_PRINCIPAL "server"

/* Store `secret` for (agent, cred) under the server principal, encrypted under the
 * server master KEK. No unlock required — usable from a TCP client. Returns
 * VAULT_OK, or VAULT_ERR_CRYPTO/IO on a fail-closed error. */
vault_status_t vault_service_set_server(const char *agent, const char *cred, const char *secret);

/* Read (agent, cred) from the server principal's vault under the server master KEK
 * — no client, no unlock. VAULT_OK (plaintext written), VAULT_NO_ENTRY (no file or
 * no such entry), or VAULT_ERR_* (fail closed). `out` is cleansed on non-OK. */
vault_status_t vault_service_get_server_principal(const char *agent, const char *cred, char *out,
                                                  size_t out_len);

/* The use-path: resolve (agent, cred) for `principal` into `out`. Returns:
 *   VAULT_OK         + plaintext written;
 *   VAULT_NO_ENTRY   when there is no such credential (or no/blank principal) —
 *                    the caller falls back to env/agents.json;
 *   VAULT_ERR_LOCKED when the credential EXISTS but the vault is locked/expired
 *                    — a HARD error, never a silent downgrade (D15);
 *   VAULT_ERR_*      on a crypto/IO failure (fail closed).
 * `out` is cleansed on every non-OK return. */
vault_status_t vault_service_get(const char *principal, const char *agent, const char *cred,
                                 char *out, size_t out_len, long now_epoch);

/* List the principal's (agent, cred) pairs (no secrets). Returns VAULT_OK with
 * *count set, or an error. Does not require an unlocked vault. */
vault_status_t vault_service_list(const char *principal, vault_store_entry_t *out, int max,
                                  int *count);

/* Delete (agent, cred) from the principal's vault. Does not require unlock. */
vault_status_t vault_service_delete(const char *principal, const char *agent, const char *cred);

/* Lock the principal's vault: evict its cached KEK (the `vault lock` path). */
vault_status_t vault_service_lock(const char *principal);

/* The canonical credential name for an agent's primary key in the vault store
 * and in the delegate-credential cooldown pool (WP-C.3). */
#define VAULT_API_KEY_CRED "api_key"

/* WP-C.3 codex-oauth: a principal may vault their Codex OAuth token + account id
 * under these names; a vaulted token overrides the on-disk/session token. */
#define VAULT_CODEX_TOKEN_CRED   "codex_oauth_token"
#define VAULT_CODEX_ACCOUNT_CRED "codex_account_id"

/* Delegate use-path helper: if `principal` has a vaulted "api_key" credential for
 * `agent`, decrypt it and overwrite `api_key` (capacity api_key_len). On any
 * non-OK status `api_key` is left UNTOUCHED (so a config/env key survives a
 * miss). Returns VAULT_OK (injected), VAULT_NO_ENTRY (use env), VAULT_ERR_LOCKED
 * (caller must fail the delegate), or an error. The transient plaintext lives
 * only inside this call and is OPENSSL_cleanse'd. */
vault_status_t vault_service_inject_api_key(const char *principal, const char *agent, char *api_key,
                                            size_t api_key_len, long now_epoch);

#endif /* DEC_VAULT_SERVICE_H */
