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
   VAULT_NO_ENTRY,      /* no such credential — caller falls back to env/agents.json (D15) */
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

/* Store `secret` for (agent, cred) under the principal's cached KEK. Requires an
 * unlocked vault (VAULT_ERR_LOCKED otherwise). */
vault_status_t vault_service_set(const char *principal, const char *agent, const char *cred,
                                 const char *secret, long now_epoch);

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

#endif /* DEC_VAULT_SERVICE_H */
