/* vault_internal.h: private backend-seam vtables for the credential vault.
 *
 * Used by modules/vault/vault_store.c and modules/vault/vault_server_key.c to
 * dispatch every public facade call through a swappable backend, so a future
 * stateful backend (e.g. a networked KMS or an alternate on-disk store) can be
 * dropped in behind the unchanged public headers. Not for general callers —
 * mirrors the db1/secrets_internal.h "facade + private vtable" pattern.
 *
 * Every function pointer takes `void *ctx` as its FIRST parameter so a future
 * stateful backend needs no globals: the facade forwards g_*->ctx as that arg.
 * The current backends are stateless (they resolve paths from
 * config_default_dir()), so ctx is NULL and unused. */
#ifndef DEC_VAULT_INTERNAL_H
#define DEC_VAULT_INTERNAL_H 1

#include <stddef.h>
#include <stdint.h>
#include "vault_crypto.h"    /* VAULT_KEK_LEN, VAULT_SALT_LEN */
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX */
#include "vault_store.h"     /* vault_store_entry_t */

/* ── Storage backend ──────────────────────────────────────────────────────────
 * One fn-pointer per public vault_store_* operation, with identical remaining
 * parameters/return types (see vault_store.h for the per-op contract). The
 * public vault_store_<op>() facade is a thin forwarder:
 *     return g_store_backend->op(g_store_backend->ctx, <args>);
 * The default backend ("jsonfile") is the 0600 per-principal JSON file store. */
typedef struct
{
   const char *name;
   void *ctx;

   int (*get_or_create_salt)(void *ctx, const char *principal, uint8_t salt[VAULT_SALT_LEN]);
   int (*salt_readonly)(void *ctx, const char *principal, uint8_t salt[VAULT_SALT_LEN]);
   int (*unlock_check)(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN]);
   int (*set)(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
              const char *cred, const char *secret);
   int (*set_dual)(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN],
                   const uint8_t server_kek[VAULT_KEK_LEN], const char *agent, const char *cred,
                   const char *secret);
   int (*set_server)(void *ctx, const char *principal, const uint8_t server_kek[VAULT_KEK_LEN],
                     const char *agent, const char *cred, const char *secret);
   int (*get_server)(void *ctx, const char *principal, const uint8_t server_kek[VAULT_KEK_LEN],
                     const char *agent, const char *cred, char *out, size_t out_len);
   int (*add_server_wraps)(void *ctx, const char *principal, const uint8_t user_kek[VAULT_KEK_LEN],
                           const uint8_t server_kek[VAULT_KEK_LEN]);
   int (*get)(void *ctx, const char *principal, const uint8_t kek[VAULT_KEK_LEN], const char *agent,
              const char *cred, char *out, size_t out_len);
   int (*has_entry)(void *ctx, const char *principal, const char *agent, const char *cred);
   int (*list)(void *ctx, const char *principal, vault_store_entry_t *out, int max);
   int (*delete)(void *ctx, const char *principal, const char *agent, const char *cred);
   int (*rekey)(void *ctx, const char *principal, const uint8_t old_kek[VAULT_KEK_LEN],
                const uint8_t new_kek[VAULT_KEK_LEN]);
   int (*rekey_field)(void *ctx, const char *principal, const char *field,
                      const uint8_t old_kek[VAULT_KEK_LEN], const uint8_t new_kek[VAULT_KEK_LEN]);
   int (*list_principals)(void *ctx, char (*out)[VAULT_PRINCIPAL_MAX], int max);
} vault_store_backend_t;

/* ── Custody provider ─────────────────────────────────────────────────────────
 * The source of the SERVER KEK for dual-access credentials (WP-C.4) and the
 * master-key rotation operation. get_kek wraps vault_server_kek()'s
 * derive-and-return-KEK logic; rotate wraps vault_server_key_rotate(). The
 * default provider ("file") reads/mints the 0600 .server-master.key file.
 *
 * HWM callbacks carry a version plus a detached signed attestation. */
typedef struct
{
   const char *name;
   void *ctx;

   int (*get_kek)(void *ctx, uint8_t kek[VAULT_KEK_LEN]);
   int (*rotate)(void *ctx, const char *server_principal, int *out_principals, int *out_creds,
                 char *backup_path, size_t backup_path_len, char *errbuf, size_t errlen);

   /* ── Seal barrier (P7 §3; slice 3b) — all OPTIONAL (may be NULL) ─────────────
    * These model an EXTERNAL-ANCHOR custody that boots SEALED and refuses to yield
    * a KEK until an out-of-band unseal runs. Seal state lives in the provider's
    * `ctx` (per-instance), never a shared module global. A NULL slot means "always
    * unsealed / no-op" — which is exactly the `file` provider: it self-unseals from
    * its 0600 master-key file, so the SERVER profile never seals and never observes
    * VAULT_ERR_SEALED. When a provider IS sealed, its get_kek MUST fail (-1); the
    * kb-facing accessor maps that to VAULT_ERR_SEALED after checking is_sealed().
    *
    *   is_sealed(ctx) -> 1 if sealed (get_kek must fail), 0 if unsealed. A NULL
    *                     slot is treated as 0 (always unsealed).
    *   unseal(ctx, params, len) -> provider-specific unseal from opaque `params`
    *                     (the mock: an unseal secret; a real anchor: a workload-
    *                     identity Decrypt / TPM policy session / PKCS#11 login).
    *                     Returns 0 on success. NULL slot => no-op success.
    *   seal(ctx) -> seal + flush the provider's derived/cached KEK (zeroize).
    *                Returns 0 on success. NULL slot => no-op success. The seam-level
    *                vault_seal() ALSO flushes the process KEK cache regardless. */
   int (*is_sealed)(void *ctx);
   int (*unseal)(void *ctx, const void *params, size_t len);
   int (*seal)(void *ctx);
   int (*hwm_read)(void *ctx, const char *key_id, uint64_t *version, uint8_t *att, size_t att_cap,
                   size_t *att_len);
   int (*hwm_cas)(void *ctx, const char *key_id, uint64_t expected, uint64_t next, uint8_t *att,
                  size_t att_cap, size_t *att_len);
   int (*hwm_verify)(void *ctx, const char *key_id, uint64_t version, const uint8_t *att,
                     size_t att_len);
   /* Async-signal-safe child-side invalidation. After fork the child must not
    * reuse inherited provider locks, sessions, handles, or plaintext caches;
    * the process must exec before using custody again. */
   void (*after_fork_child)(void *ctx);
   /* Read only process-local provider state.  This callback must not perform
    * backend I/O and must honor timeout_ms while acquiring its own state lock.
    * Kept last so legacy positional provider initializers remain compatible. */
   int (*local_status)(void *ctx, unsigned timeout_ms);
   /* Closed D3b authorization results (vault_custody_auth_result_t).  The
    * preflight is read-only and generation-bound; typed_unseal publishes a KEK
    * only on VAULT_CUSTODY_AUTHORIZED.  Kept last for source compatibility with
    * legacy positional test providers. */
   int (*authorization_preflight)(void *ctx, const void *secret, size_t secret_len,
                                  uint64_t expected_generation);
   int (*typed_unseal)(void *ctx, const void *secret, size_t secret_len);
   int (*authorization_preflight_current)(void *ctx, const void *secret, size_t secret_len,
                                          uint64_t *generation);
} vault_custody_provider_t;

/* ── Backend binders ──────────────────────────────────────────────────────────
 * Rebind the active backend/provider — the mechanism P7's kb profile uses to
 * compose {postgres store, external-anchor custody} without editing the core, and
 * the mechanism a test uses to drive the public facade against a mock. Passing
 * NULL restores the built-in default (jsonfile / file). These are the ONLY way the
 * file-static default pointers change; the defaults make the server profile work
 * with no binder call at all (behavior-preserving). Not thread-safe against
 * concurrent facade calls — bind at construction/startup, not mid-flight. */
void vault_store_set_backend(const vault_store_backend_t *backend);
void vault_custody_set_provider(const vault_custody_provider_t *provider);

#endif /* DEC_VAULT_INTERNAL_H */
