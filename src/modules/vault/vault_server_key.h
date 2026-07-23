#ifndef DEC_VAULT_SERVER_KEY_H
#define DEC_VAULT_SERVER_KEY_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_crypto.h" /* VAULT_KEK_LEN */

/* vault_server_key: the SERVER-sealed key-encryption-key for dual-access
 * credentials (WP-C.4).
 *
 * The per-user vaults (vault_store/vault_service) are zero-knowledge: a
 * credential's data-encryption key (DEK) is AES-KW-wrapped under a KEK derived
 * from the client root key (uid:) or login password (webuser:), so the server
 * cannot read a user's secret without that client unlocking. That makes a
 * delegate UNRUNNABLE after a restart / KEK-cache TTL expiry — the very thing we
 * must fix: aimee-server has to drive delegates autonomously.
 *
 * Dual-access wrap: every vaulted credential's DEK is wrapped a SECOND time under
 * this server KEK (in addition to the user KEK). The server can then decrypt the
 * credential on its own — across restarts, with no client — while the user's
 * zero-knowledge path is unchanged. The secret is still encrypted at rest (AES-
 * 256-GCM under a random DEK); the server KEK only unwraps the DEK.
 *
 * Trust boundary (deliberate, per the credential-storage directive): the master
 * key lives 0600 on the server data volume beside the vault. This defeats backup
 * / disk theft and plaintext-in-config, NOT a root-level host compromise (root
 * can read both key and data) — the same boundary the rest of aimee-server runs
 * under. The module is structured so an operator passphrase could wrap the master
 * key file later without a data migration.
 *
 * The 32-byte master key is generated with vault_crypto_random on first use and
 * persisted at <config_default_dir>/.vault/.server-master.key. The server KEK is
 * HKDF-derived from it (domain-separated from the user KEKs) and cached process-
 * wide. Fail-closed: any entropy/IO/derivation failure returns -1 and the caller
 * must abort the vault op rather than store/read an unprotected credential. */

/* Load-or-create the master key file, derive the server KEK, and write it to
 * `kek` (VAULT_KEK_LEN bytes). The derived KEK is cached after the first call.
 * Returns 0 on success, -1 on any failure (kek cleansed). The caller should
 * OPENSSL_cleanse its copy of `kek` after use. */
int vault_server_kek(uint8_t kek[VAULT_KEK_LEN]);

/* Test seam: drop the cached server KEK so a test that re-points
 * config_default_dir at a fresh temp dir re-derives from that dir's master key.
 * Not for production callers. This is also a local seal transition: it advances
 * the use generation and invalidates durable-epoch synchronization. */
void vault_server_key_reset_for_test(void);

/* Rotate the `.server-master.key` (D13). Mint a fresh master key, derive the new
 * server KEK, and RE-WRAP every principal's server wrap from the old KEK to the
 * new one — a re-wrap, not a re-encrypt: no credential plaintext is touched.
 *
 * This is an OFFLINE maintenance operation: the server caches one process-wide
 * server KEK, so a live rotation would leave autonomous decrypts failing for the
 * window between re-wrapping a credential and swapping the key. Run it with the
 * server STOPPED (e.g. `aimee-server --rotate-master-key`).
 *
 * Crash/error safety: the whole `.vault/` directory is copied to a 0700 backup
 * BEFORE any mutation; if ANY principal's re-wrap fails, or the new master cannot
 * be persisted, the vault is restored from that backup and the rotation aborts
 * with the vault unchanged (fail-closed — never a new-wrapped vault under an old
 * master, nor the reverse). On success the backup path is reported so the
 * operator can verify, then remove it.
 *
 * `server_principal` is the principal whose PRIMARY wrap ("wrapped_dek") is the
 * server KEK (i.e. VAULT_SERVER_PRINCIPAL); every other principal carries the
 * server KEK only in its dual-access "wrapped_dek_server" field. Passed in so the
 * key module need not depend on the vault_service naming layer.
 *
 * On success returns 0 and (if non-NULL) sets *out_principals / *out_creds to the
 * number of principals scanned and credentials re-wrapped, and writes the backup
 * directory path into `backup_path`. On failure returns -1 with a human-readable
 * reason in `errbuf`. A vault with no master key yet returns 0 (nothing to do). */
int vault_server_key_rotate(const char *server_principal, int *out_principals, int *out_creds,
                            char *backup_path, size_t backup_path_len, char *errbuf, size_t errlen);

/* ── Seal barrier (P7 §3; slice 3b) ───────────────────────────────────────────
 * Thin seam-side accessors that dispatch to the currently-bound custody provider's
 * OPTIONAL seal slots (see vault_internal.h). The default `file` provider has NULL
 * seal slots, so under it these are no-ops: vault_is_sealed()==0 always, and
 * vault_seal()/vault_unseal() return 0 without effect — the SERVER profile is
 * UNAFFECTED and never sees a sealed state.
 *
 * vault_is_sealed() -> 1 if the bound provider reports sealed (its get_kek will
 *                      fail), else 0. A provider with no is_sealed slot => 0.
 * vault_unseal(params,len) -> forward opaque provider-specific unseal params.
 *                      Returns 0 on success, -1 on failure. NULL slot => 0.
 * vault_seal() -> seal the provider AND flush the process KEK cache
 *                 (vault_kek_cache_clear), so no cached KEK survives a seal.
 *                 Returns 0 on success, -1 if the provider's seal fails. */
int vault_is_sealed(void);
int vault_unseal(const void *params, size_t len);
int vault_seal(void);

/* Secret-free status of the selected custody provider's process-local state.
 * The query performs no provider/backend I/O and spends at most 50ms acquiring
 * provider-local locks.  UNAVAILABLE means the selected provider has no status
 * seam, its context cannot be read in time, or the process is fork-invalid.
 * MALFORMED is reserved for internally contradictory local state. */
typedef enum
{
   VAULT_CUSTODY_LOCAL_AVAILABLE_SEALED = 0,
   VAULT_CUSTODY_LOCAL_AVAILABLE_UNSEALED = 1,
   VAULT_CUSTODY_LOCAL_UNAVAILABLE = 2,
   VAULT_CUSTODY_LOCAL_MALFORMED = 3,
} vault_custody_local_status_t;

vault_custody_local_status_t vault_custody_selected_local_status(void);

/* Closed result of a root-local custody authorization operation.  These values
 * are deliberately not provider return codes: callers must never infer a bad
 * credential by parsing ESYS/KMS/PKCS#11 prose. */
typedef enum
{
   VAULT_CUSTODY_AUTHORIZED = 0,
   VAULT_CUSTODY_AUTH_WRONG_SECRET = 1,
   VAULT_CUSTODY_AUTH_BACKEND_UNAVAILABLE = 2,
   VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE = 3,
   VAULT_CUSTODY_AUTH_UNSUPPORTED = 4,
} vault_custody_auth_result_t;

#define VAULT_CUSTODY_AUTH_SECRET_MAX 4096U

/* Read-only live authorization preflight for the selected provider.  The TPM2
 * implementation authenticates the configured NV counter, verifies its public
 * identity/attributes and current value, and proves that the active blob is
 * PolicyNV-bound to expected_generation.  It never unseals or mutates NV.
 * expected_generation must be positive. */
vault_custody_auth_result_t
vault_custody_selected_authorization_preflight(const void *secret, size_t secret_len,
                                               uint64_t expected_generation);

/* Authenticate and discover the active TPM PolicyNV generation in one live,
 * read-only proof.  *generation is zeroed on every non-authorized result and is
 * positive only when the canonical NV counter and active blob agree exactly. */
vault_custody_auth_result_t
vault_custody_selected_authorization_preflight_current(const void *secret, size_t secret_len,
                                                       uint64_t *generation);

/* Anchor-authoritative per-key high-water operations (P7 §8). Providers that
 * do not supply the complete signed-HWM seam fail closed; there is deliberately
 * no database or process-local fallback. Returned attestations have already
 * been cryptographically verified by the provider. */
int vault_hwm_read(const char *key_id, uint64_t *version, uint8_t *att, size_t att_cap,
                   size_t *att_len);
int vault_hwm_cas(const char *key_id, uint64_t expected, uint64_t next, uint8_t *att,
                  size_t att_cap, size_t *att_len);
int vault_hwm_verify(const char *key_id, uint64_t version, const uint8_t *att, size_t att_len);

/* Local seal-generation guard for synchronous use-in-place. Snapshot before durable
 * admission, then begin immediately before KEK access. A successful begin holds a
 * shared guard until vault_use_end; seal takes the exclusive side and advances the
 * generation, so an admission cannot cross a seal/unseal cycle. */
uint64_t vault_use_epoch_snapshot(void);
int vault_use_begin(uint64_t expected_epoch, uint64_t admitted_primary_epoch,
                    uint8_t kek[VAULT_KEK_LEN]);
void vault_use_end(void);

/* Exclusive maintenance guard used by crash recovery and DEK re-wrap. The
 * handle is opaque and is validated against an internal registry before any
 * state is accessed. A successful begin disables cancellation for the owning
 * thread and drains all operational readers. */
typedef struct vault_maintenance_guard vault_maintenance_guard_t;
typedef int (*vault_maintenance_kek_fn)(const uint8_t kek[VAULT_KEK_LEN], void *ctx);

enum
{
   VAULT_MAINTENANCE_OK = 0,
   VAULT_MAINTENANCE_ERROR = -1,
   VAULT_MAINTENANCE_INVALID = -2,
   VAULT_MAINTENANCE_BUSY = -3,
   VAULT_MAINTENANCE_WRONG_OWNER = -4,
   VAULT_MAINTENANCE_EPOCH = -5,
   VAULT_MAINTENANCE_SEALED = -6,
};

int vault_maintenance_guard_begin(vault_maintenance_guard_t **guard);
int vault_maintenance_guard_sync_primary_epoch(vault_maintenance_guard_t *guard,
                                               uint64_t primary_epoch);
int vault_maintenance_guard_with_active_kek(vault_maintenance_guard_t *guard,
                                            vault_maintenance_kek_fn callback, void *ctx);
int vault_maintenance_guard_unseal(vault_maintenance_guard_t *guard, const void *params,
                                   size_t len);
vault_custody_auth_result_t vault_maintenance_guard_unseal_typed(vault_maintenance_guard_t *guard,
                                                                 const void *params, size_t len);
int vault_maintenance_guard_seal(vault_maintenance_guard_t *guard);
int vault_maintenance_guard_end(vault_maintenance_guard_t **guard);

/* Release an exact live guard while retaining an authenticated provider KEK.
 * This is intentionally separate from the ordinary seal-on-end primitive.  It
 * succeeds only for an unsealed provider whose KEK can be fetched, after the
 * primary epoch has been synchronized to the exact committed open epoch.  Any
 * failed precondition for an otherwise valid owned guard takes the ordinary
 * fail-closed sealing path and consumes the guard. */
int vault_maintenance_guard_end_operational(vault_maintenance_guard_t **guard,
                                            uint64_t committed_primary_epoch);

/* Startup-only synchronization with the durable kb_control.seal_epoch. No
 * maintenance guard or key use may be active. The selected provider may already
 * be unsealed by its startup login (PKCS#11/KMS); the caller is responsible for
 * first forcing a seal when durable control says sealed. Exact replay is
 * idempotent; a differing second initialization fails. */
int vault_primary_epoch_initialize(uint64_t primary_epoch);

#endif /* DEC_VAULT_SERVER_KEY_H */
