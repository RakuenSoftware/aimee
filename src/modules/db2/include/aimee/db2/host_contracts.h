/* host_contracts.h: bounded process-owned dependencies installed into the DB2 C owner. */
#ifndef AIMEE_DB2_HOST_CONTRACTS_H
#define AIMEE_DB2_HOST_CONTRACTS_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Produce the canonical lowercase SHA-256 row hash for the shared audit WORM
    * record. out_hex has room for 64 hex bytes plus NUL. */
   typedef void (*aimee_db2_audit_hash_fn)(long long seq, const char *actor_role,
                                           const char *actor_principal, const char *action,
                                           const char *subject, const char *verdict,
                                           const char *key_id, const char *detail,
                                           const char *prev_hash, char out_hex[65]);

   /* Install the audit module's canonical hash provider during process startup.
    * NULL removes it. Audit appends and verification fail closed while absent or
    * when the provider returns anything other than 64 lowercase hex bytes. */
   void aimee_db2_register_audit_hash_provider(aimee_db2_audit_hash_fn provider);

   /* Score one synthesis candidate against its evidence bundle. The provider
    * returns 0 and fills all three outputs on success, or -1 when scoring is
    * unavailable. */
   typedef int (*aimee_db2_mdl_score_fn)(const char *candidate, const char *evidence,
                                         double *l_candidate, double *l_residual, double *total);

   /* Install the KB host's canonical MDL scorer during process startup. NULL
    * removes it; artifact commits continue without optional MDL features. */
   void aimee_db2_register_mdl_score_provider(aimee_db2_mdl_score_fn provider);

   enum
   {
      AIMEE_DB2_FACT_GATE_ACCEPT = 0,
      AIMEE_DB2_FACT_GATE_REJECT_KIND = 1,
      AIMEE_DB2_FACT_GATE_NOVEL = 2,
      AIMEE_DB2_FACT_GATE_BADARG = 3,
   };

   /* Validate one typed-fact relation through the host's memory gate. The
    * provider returns 0 and writes one AIMEE_DB2_FACT_GATE_* verdict, or -1
    * when no authoritative verdict is available. */
   typedef int (*aimee_db2_fact_gate_fn)(int head_kind, const char *rel_type, int tail_kind,
                                         int *verdict);

   /* Install the host's canonical memory fact gate during process startup. NULL
    * removes it; DB2 then defers typed-fact commits rather than writing without
    * an authoritative verdict. */
   void aimee_db2_register_fact_gate_provider(aimee_db2_fact_gate_fn provider);

#define AIMEE_DB2_FACT_SUBJECT_MAX  128
#define AIMEE_DB2_FACT_REL_TYPE_MAX 64
#define AIMEE_DB2_FACT_OBJECT_MAX   128
#define AIMEE_DB2_FACT_ATTR_MAX     128

   typedef struct
   {
      char subject[AIMEE_DB2_FACT_SUBJECT_MAX];
      char rel_type[AIMEE_DB2_FACT_REL_TYPE_MAX];
      char object[AIMEE_DB2_FACT_OBJECT_MAX];
      int subject_kind;
      int object_kind;
   } aimee_db2_fact_candidate_t;

   /* Extract bounded typed-fact candidates through the host memory module. The
    * provider returns 0 and a count in [0,max], or -1 when it cannot answer. */
   typedef int (*aimee_db2_fact_extract_fn)(const char *text, aimee_db2_fact_candidate_t *out,
                                            int max, int *count);

   /* Scan one turn for a retraction cue and optional attribute. Both flags must
    * be 0 or 1, and attr is non-empty exactly when has_attr is 1. */
   typedef int (*aimee_db2_fact_scan_fn)(const char *text, int *is_retraction, int *has_attr,
                                         char attr[AIMEE_DB2_FACT_ATTR_MAX]);

   /* Install the memory module's extraction and retraction-scan adapters. NULL
    * removes a provider; extraction then fails and scanning cannot delete. */
   void aimee_db2_register_fact_extract_provider(aimee_db2_fact_extract_fn provider);
   void aimee_db2_register_fact_scan_provider(aimee_db2_fact_scan_fn provider);

   enum
   {
      AIMEE_DB2_EMBED_DOCUMENT = 0,
      AIMEE_DB2_EMBED_QUERY = 1,
   };

   /* Embed one text through the process-owned memory stage. A successful
    * provider returns a dimension in [1,max_dim] and fills exactly that many
    * finite floats. Zero means unavailable or failed; it is never a lexical
    * substitute. */
   typedef int (*aimee_db2_embed_fn)(const char *text, const char *command, int input_type,
                                     float *out, int max_dim);

   /* Install the memory embedding adapter. NULL removes it; DB2 then fails
    * embedding closed instead of linking or falling back to memory internals. */
   void aimee_db2_register_embed_provider(aimee_db2_embed_fn provider);

   enum
   {
      AIMEE_DB2_PRINCIPAL_NONE = 0,
      AIMEE_DB2_PRINCIPAL_OIDC = 1,
      AIMEE_DB2_PRINCIPAL_CERT = 2,
      AIMEE_DB2_PRINCIPAL_OWNER = 3,
      AIMEE_DB2_PRINCIPAL_HOST = 4,
   };

   /* Derive the canonical tenant identity from verifier-owned principal fields.
    * The provider returns 0 and writes one NUL-terminated key on success. DB2
    * independently validates the returned owner/OIDC/certificate/host grammar;
    * an absent provider or malformed answer leaves tenant entry fail closed. */
   typedef int (*aimee_db2_identity_key_fn)(int kind, const char *issuer, const char *subject,
                                            int authenticated, char *out, size_t cap);

   /* Install the KB identity owner's canonical key adapter during process
    * startup. NULL removes it. */
   void aimee_db2_register_identity_key_provider(aimee_db2_identity_key_fn provider);

   struct kb_mgmt_token_authority_record;
   struct kb_identity_token_authority_record;

   /* Revalidate authority-only database records through the protected token
    * owner's canonical binding checks. Exactly 1 means valid; absence, provider
    * failure, and every other result fail the DB2 decode closed. */
   typedef int (*aimee_db2_mgmt_token_record_valid_fn)(
       const struct kb_mgmt_token_authority_record *record);
   typedef int (*aimee_db2_identity_token_record_valid_fn)(
       const struct kb_identity_token_authority_record *record);

   /* Install both record validators as one startup contract. NULL removes the
    * corresponding validator. */
   void
   aimee_db2_register_token_record_validators(aimee_db2_mgmt_token_record_valid_fn management,
                                              aimee_db2_identity_token_record_valid_fn identity);

   /* Compare retained computed-style JSON through the CSS owner's canonical
    * parser and oracle. A successful provider returns 0 and writes binary
    * validity/verdict flags plus a nonnegative diff count. */
   typedef int (*aimee_db2_css_render_compare_fn)(const char *before_json, const char *after_json,
                                                  int *before_valid, int *after_valid,
                                                  int *available, int *equivalent, int *diff_count);

   /* Install the CSS render-oracle adapter during process startup. NULL removes
    * it; DB2 then fails evaluation closed without changing a stored verdict. */
   void aimee_db2_register_css_render_compare_provider(aimee_db2_css_render_compare_fn provider);

   struct css_stylesheet;
#define AIMEE_DB2_CSS_CLASS_TOKEN_MAX 128

   typedef struct css_stylesheet *(*aimee_db2_css_analyze_fn)(const char *text, size_t len);
   typedef void (*aimee_db2_css_stylesheet_free_fn)(struct css_stylesheet *stylesheet);
   typedef int (*aimee_db2_css_extract_class_tokens_fn)(const char *text, size_t len,
                                                        char (*out)[AIMEE_DB2_CSS_CLASS_TOKEN_MAX],
                                                        int max);

   /* Install the CSS owner's parser, matching release function, and bounded
    * static-class extractor as one startup contract. NULL removes a provider;
    * DB2 then skips that derived write rather than accepting unvalidated data. */
   void aimee_db2_register_css_analysis_providers(
       aimee_db2_css_analyze_fn analyze, aimee_db2_css_stylesheet_free_fn release,
       aimee_db2_css_extract_class_tokens_fn extract_class_tokens);

#define AIMEE_DB2_VAULT_KEK_LEN         32
#define AIMEE_DB2_VAULT_DEK_LEN         32
#define AIMEE_DB2_VAULT_NONCE_LEN       12
#define AIMEE_DB2_VAULT_TAG_LEN         16
#define AIMEE_DB2_VAULT_WRAPPED_DEK_LEN 40

   typedef struct
   {
      int (*aad_build_v2)(const char *principal, const char *agent, const char *cred,
                          int64_t version, uint8_t *out, size_t cap, size_t *out_len);
      int (*aad_build_v1_safe)(const char *principal, const char *agent, const char *cred,
                               int64_t version, uint8_t *out, size_t cap, size_t *out_len);
      int (*random)(uint8_t *out, size_t len);
      int (*dek_wrap)(const uint8_t kek[AIMEE_DB2_VAULT_KEK_LEN],
                      const uint8_t dek[AIMEE_DB2_VAULT_DEK_LEN],
                      uint8_t wrapped[AIMEE_DB2_VAULT_WRAPPED_DEK_LEN]);
      int (*dek_unwrap)(const uint8_t kek[AIMEE_DB2_VAULT_KEK_LEN],
                        const uint8_t wrapped[AIMEE_DB2_VAULT_WRAPPED_DEK_LEN],
                        uint8_t dek[AIMEE_DB2_VAULT_DEK_LEN]);
      int (*secret_encrypt)(const uint8_t dek[AIMEE_DB2_VAULT_DEK_LEN], const uint8_t *aad,
                            size_t aad_len, const uint8_t *plaintext, size_t plaintext_len,
                            uint8_t nonce[AIMEE_DB2_VAULT_NONCE_LEN], uint8_t *ciphertext,
                            uint8_t tag[AIMEE_DB2_VAULT_TAG_LEN]);
      int (*secret_decrypt)(const uint8_t dek[AIMEE_DB2_VAULT_DEK_LEN], const uint8_t *aad,
                            size_t aad_len, const uint8_t nonce[AIMEE_DB2_VAULT_NONCE_LEN],
                            const uint8_t *ciphertext, size_t ciphertext_len,
                            const uint8_t tag[AIMEE_DB2_VAULT_TAG_LEN], uint8_t *plaintext);
      int (*kek_check_wrap)(const uint8_t kek[AIMEE_DB2_VAULT_KEK_LEN],
                            uint8_t wrapped[AIMEE_DB2_VAULT_WRAPPED_DEK_LEN]);
      int (*kek_check_verify)(const uint8_t kek[AIMEE_DB2_VAULT_KEK_LEN],
                              const uint8_t wrapped[AIMEE_DB2_VAULT_WRAPPED_DEK_LEN]);
   } aimee_db2_vault_crypto_provider_t;

   /* Install the vault owner's complete envelope-crypto vtable at startup. The
    * value is copied. NULL removes it; absent operations and nonzero results
    * fail closed and DB2 cleanses their secret outputs. */
   void aimee_db2_register_vault_crypto_provider(const aimee_db2_vault_crypto_provider_t *provider);

#define AIMEE_DB2_VAULT_RESEAL_RECEIPT_LEN   208
#define AIMEE_DB2_VAULT_RESEAL_OPERATION_LEN 16
#define AIMEE_DB2_VAULT_RESEAL_OPERATION_HEX 32

   struct vault_tpm2_reseal_receipt;
   typedef struct
   {
      int64_t (*deadline_ms)(uint32_t per_call_ms);
      int (*operation_id_to_hex)(const uint8_t operation_id[AIMEE_DB2_VAULT_RESEAL_OPERATION_LEN],
                                 char out[AIMEE_DB2_VAULT_RESEAL_OPERATION_HEX + 1]);
      int (*operation_id_from_hex)(const char *hex,
                                   uint8_t operation_id[AIMEE_DB2_VAULT_RESEAL_OPERATION_LEN]);
      int (*receipt_decode)(const uint8_t *wire, size_t wire_len,
                            struct vault_tpm2_reseal_receipt *receipt);
      int (*receipt_digest)(const uint8_t wire[AIMEE_DB2_VAULT_RESEAL_RECEIPT_LEN],
                            uint8_t digest[32]);
   } aimee_db2_vault_reseal_provider_t;

   /* Install the vault owner's mutation-deadline and canonical reseal-codec
    * operations. The value is copied; NULL removes it and DB2 fails closed. */
   void aimee_db2_register_vault_reseal_provider(const aimee_db2_vault_reseal_provider_t *provider);

   struct vault_witness_checkpoint;
   struct vault_witness_anchor;
   struct vault_witness_leaf;
   struct vault_witness_record;
   typedef struct
   {
      int (*checkpoint_digest)(const struct vault_witness_checkpoint *checkpoint,
                               uint8_t digest[32]);
      int (*checkpoint_encode)(const struct vault_witness_checkpoint *checkpoint, uint8_t *out,
                               size_t cap, size_t *out_len);
      int (*checkpoint_sign)(struct vault_witness_checkpoint *checkpoint);
      int (*checkpoint_verify)(const struct vault_witness_checkpoint *checkpoint,
                               const struct vault_witness_anchor *anchors, size_t anchor_count);
      int (*export_frame)(int kind, const uint8_t *payload, size_t payload_len, uint8_t *out,
                          size_t cap, size_t *out_len);
      int (*leaf_hash)(const char *tenant, const char *provider, uint64_t sequence,
                       const uint8_t head_hash[32], uint8_t out[32]);
      int (*merkle_root)(const struct vault_witness_leaf *leaves, size_t count, uint8_t root[32]);
      int (*record_digest)(const struct vault_witness_record *record, uint8_t digest[32]);
      int (*record_encode)(const struct vault_witness_record *record, uint8_t *out, size_t cap,
                           size_t *out_len);
      int (*shard_key_hash)(const char *tenant, const char *provider, uint8_t out[8]);
      int (*signer_identity)(uint8_t public_key[32], uint8_t key_id[16]);
      int (*verify_checkpoint_run)(const struct vault_witness_checkpoint *checkpoints, size_t count,
                                   size_t *gap_after_index);
   } aimee_db2_vault_witness_provider_t;

   /* Install the vault owner's canonical witness codec, hashing, signing, and
    * verification operations at startup. The value is copied; NULL removes it,
    * and absent or malformed provider results fail closed. */
   void
   aimee_db2_register_vault_witness_provider(const aimee_db2_vault_witness_provider_t *provider);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_HOST_CONTRACTS_H */
