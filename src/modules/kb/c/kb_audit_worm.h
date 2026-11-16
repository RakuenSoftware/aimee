/* kb_audit_worm.h: aimee-kb WORM audit producer seam. Producers commit
 * immutable PostgreSQL outbox intents; the separately credentialed worker
 * constructs the chain with the shared SQLite audit_worm implementation. */
#ifndef AIMEE_KB_STORE_KB_AUDIT_WORM_H
#define AIMEE_KB_STORE_KB_AUDIT_WORM_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef void (*kb_store_audit_hash_fn)(long long seq, const char *actor_role,
                                          const char *actor_principal, const char *action,
                                          const char *subject, const char *verdict,
                                          const char *key_id, const char *detail,
                                          const char *prev_hash, char out_hex[65]);
   typedef void (*kb_store_audit_hash_v2_fn)(long long seq, const char *ts, const char *actor_role,
                                             const char *actor_principal, const char *actor_issuer,
                                             const char *actor_subject, const char *transport_cn,
                                             long long team_id, const char *selected_default_from,
                                             const char *action, const char *subject,
                                             const char *verdict, const char *key_id,
                                             const char *detail, const char *prev_hash,
                                             char out_hex[65]);

   /* Submit one governed action to the durable WORM outbox. detail is bounded
    * to AUDIT_WORM_DETAIL_MAX. Returns 0 when the intent commits, -1 on failure. */
   int kb_store_kb_audit_append(const char *actor_role, const char *actor_principal,
                                const char *action, const char *subject, const char *verdict,
                                const char *detail);

   /* Transaction-owned variant used when a mutation and its immutable audit
    * intent must commit atomically. |conn| must already be inside BEGIN; this
    * function neither commits nor rolls back and never writes the chain. */
   int kb_store_kb_audit_append_in_txn(void *conn, const char *actor_role,
                                       const char *actor_principal, const char *action,
                                       const char *subject, const char *verdict,
                                       const char *detail);

   /* Durable producer backlog. Returns 0 and fills the available outputs, or
    * -1 when the database/function is unavailable. Age is zero for an empty
    * queue. Runtime has read-only EXECUTE on the aggregate, never table access. */
   int kb_store_kb_audit_pending(long long *count, long long *oldest_age_seconds);

   int kb_store_kb_audit_worm_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_KB_STORE_KB_AUDIT_WORM_H */
