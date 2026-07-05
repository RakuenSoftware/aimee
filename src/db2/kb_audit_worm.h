/* kb_audit_worm.h: aimee-kb Postgres WORM audit store (S5). Same hash-chained
 * record as the aimee-server store (audit_worm_chain); WORM enforced by the
 * kb_audit_event triggers + a writer role granted only INSERT/SELECT. */
#ifndef AIMEE_DB2_KB_AUDIT_WORM_H
#define AIMEE_DB2_KB_AUDIT_WORM_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Append one governed kb action, hash-chained to the current head. detail is
    * bounded to AUDIT_WORM_DETAIL_MAX. Returns 0 on success, -1 on failure. */
   int db2_kb_audit_append(const char *actor_role, const char *actor_principal, const char *action,
                           const char *subject, const char *verdict, const char *detail);

   /* Recompute the whole chain (row_hash + prev linkage + gap-free seq). 0 if intact,
    * -1 on the first break (reason in err). */
   int db2_kb_audit_verify_chain(char *err, size_t errlen);

   /* Row count (test/introspection). -1 on error. */
   long db2_kb_audit_count(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_KB_AUDIT_WORM_H */
