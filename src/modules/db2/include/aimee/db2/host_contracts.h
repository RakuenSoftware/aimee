/* host_contracts.h: bounded process-owned dependencies installed into the DB2 C owner. */
#ifndef AIMEE_DB2_HOST_CONTRACTS_H
#define AIMEE_DB2_HOST_CONTRACTS_H 1

#include <stddef.h>

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

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_HOST_CONTRACTS_H */
