#ifndef DEC_VAULT_SERVER_KEY_H
#define DEC_VAULT_SERVER_KEY_H 1

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
 * Not for production callers. */
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

#endif /* DEC_VAULT_SERVER_KEY_H */
