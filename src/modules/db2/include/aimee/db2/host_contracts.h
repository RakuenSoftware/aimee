/* host_contracts.h: bounded process-owned dependencies installed into the DB2 C owner. */
#ifndef AIMEE_DB2_HOST_CONTRACTS_H
#define AIMEE_DB2_HOST_CONTRACTS_H 1

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

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_HOST_CONTRACTS_H */
