#ifndef DEC_VAULT_PG_H
#define DEC_VAULT_PG_H 1

/* db2/vault_pg.h: the Postgres storage backend for the kb credential vault (P10
 * slice 2). Implements the vault_store_backend_t vtable over DB2 (db2_conn() +
 * aimee_pg_*), storing ONLY ciphertext in org_vault_secret / org_vault_current /
 * org_vault_salt. The envelope crypto is byte-identical to the jsonfile backend
 * (same vault_crypto); only the persistence differs (Postgres rows vs a JSON file),
 * which is the P10 anti-drift property.
 *
 * kb-only: this source joins the KB/DB2 link, NEVER SERVER_SRCS. aimee-kb binds it
 * at startup with vault_store_set_backend(&vault_pg_backend) after db2_init.
 *
 * AAD = "principal|agent|cred|version" (binds the ciphertext to its identity slot
 * AND version, so a restored older-version ciphertext cannot be presented as
 * current). The server-autonomous dual-wrap ops (set_dual/set_server/get_server/
 * add_server_wraps) are unsupported on this backend — they encode the server's
 * "server can read a user credential without the user unlocking" model, which the
 * single-KEK org vault does not use. They log and return -1. */

#include "vault_internal.h" /* vault_store_backend_t (-Imodules/vault) */

typedef struct
{
   int (*aad_build_v2)(const char *principal, const char *agent, const char *cred, int64_t version,
                       uint8_t *out, size_t cap, size_t *out_len);
   int (*aad_build_v1_safe)(const char *principal, const char *agent, const char *cred,
                            int64_t version, uint8_t *out, size_t cap, size_t *out_len);
   int (*random)(uint8_t *out, size_t len);
   int (*dek_wrap)(const uint8_t kek[VAULT_KEK_LEN], const uint8_t dek[VAULT_DEK_LEN],
                   uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);
   int (*dek_unwrap)(const uint8_t kek[VAULT_KEK_LEN], const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN],
                     uint8_t dek[VAULT_DEK_LEN]);
   int (*secret_encrypt)(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                         const uint8_t *plaintext, size_t plaintext_len,
                         uint8_t nonce[VAULT_GCM_NONCE_LEN], uint8_t *ciphertext,
                         uint8_t tag[VAULT_GCM_TAG_LEN]);
   int (*secret_decrypt)(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                         const uint8_t nonce[VAULT_GCM_NONCE_LEN], const uint8_t *ciphertext,
                         size_t ciphertext_len, const uint8_t tag[VAULT_GCM_TAG_LEN],
                         uint8_t *plaintext);
   int (*kek_check_wrap)(const uint8_t kek[VAULT_KEK_LEN], uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);
   int (*kek_check_verify)(const uint8_t kek[VAULT_KEK_LEN],
                           const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);
} db2_vault_crypto_provider_t;

/* Internal declaration of the vtable contract exported publicly through
 * <aimee/db2/host_contracts.h>. */
void aimee_db2_register_vault_crypto_provider(const db2_vault_crypto_provider_t *provider);

/* The exported backend. Bound by kb_main via vault_store_set_backend(). */
extern const vault_store_backend_t vault_pg_backend;

/* Validating DB2 side of the injected vault-crypto boundary. These declarations
 * are exposed for focused contract tests; the backend uses the same helpers. */
int db2_vault_crypto_random(uint8_t *out, size_t len);
int db2_vault_aad_build_v2(const char *principal, const char *agent, const char *cred,
                           int64_t version, uint8_t *out, size_t cap, size_t *out_len);
int db2_vault_aad_build_v1_safe(const char *principal, const char *agent, const char *cred,
                                int64_t version, uint8_t *out, size_t cap, size_t *out_len);
int db2_vault_dek_wrap(const uint8_t kek[VAULT_KEK_LEN], const uint8_t dek[VAULT_DEK_LEN],
                       uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);
int db2_vault_dek_unwrap(const uint8_t kek[VAULT_KEK_LEN],
                         const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN], uint8_t dek[VAULT_DEK_LEN]);
int db2_vault_secret_encrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                             const uint8_t *plaintext, size_t plaintext_len,
                             uint8_t nonce[VAULT_GCM_NONCE_LEN], uint8_t *ciphertext,
                             uint8_t tag[VAULT_GCM_TAG_LEN]);
int db2_vault_secret_decrypt(const uint8_t dek[VAULT_DEK_LEN], const uint8_t *aad, size_t aad_len,
                             const uint8_t nonce[VAULT_GCM_NONCE_LEN], const uint8_t *ciphertext,
                             size_t ciphertext_len, const uint8_t tag[VAULT_GCM_TAG_LEN],
                             uint8_t *plaintext);
int db2_vault_kek_check_wrap(const uint8_t kek[VAULT_KEK_LEN],
                             uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);
int db2_vault_kek_check_verify(const uint8_t kek[VAULT_KEK_LEN],
                               const uint8_t wrapped[VAULT_WRAPPED_DEK_LEN]);

#endif /* DEC_VAULT_PG_H */
