/* kb_audit_worm.h: aimee-kb WORM audit producer seam. Producers commit
 * immutable PostgreSQL outbox intents; the separately credentialed worker
 * constructs the chain with the shared SQLite audit_worm implementation. */
#ifndef AIMEE_DB2_KB_AUDIT_WORM_H
#define AIMEE_DB2_KB_AUDIT_WORM_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   typedef void (*db2_audit_hash_fn)(long long seq, const char *actor_role,
                                     const char *actor_principal, const char *action,
                                     const char *subject, const char *verdict, const char *key_id,
                                     const char *detail, const char *prev_hash, char out_hex[65]);
   typedef void (*db2_audit_hash_v2_fn)(
       long long seq, const char *ts, const char *actor_role, const char *actor_principal,
       const char *actor_issuer, const char *actor_subject, const char *transport_cn,
       long long team_id, const char *selected_default_from, const char *action,
       const char *subject, const char *verdict, const char *key_id, const char *detail,
       const char *prev_hash, char out_hex[65]);

   /* Deprecated no-op retained for ABI compatibility. Hashing is owned by the
    * shared SQLite worker and is no longer injected into DB2. */
   void aimee_db2_register_audit_hash_provider(db2_audit_hash_fn provider);
   void aimee_db2_register_audit_hash_v2_provider(db2_audit_hash_v2_fn provider);

   /* Submit one governed action to the durable WORM outbox. detail is bounded
    * to AUDIT_WORM_DETAIL_MAX. Returns 0 when the intent commits, -1 on failure. */
   int db2_kb_audit_append(const char *actor_role, const char *actor_principal, const char *action,
                           const char *subject, const char *verdict, const char *detail);

   /* Transaction-owned variant used when a mutation and its immutable audit
    * intent must commit atomically. |conn| must already be inside BEGIN; this
    * function neither commits nor rolls back and never writes the chain. */
   int db2_kb_audit_append_in_txn(void *conn, const char *actor_role, const char *actor_principal,
                                  const char *action, const char *subject, const char *verdict,
                                  const char *detail);

   /* Durable producer backlog. Returns 0 and fills the available outputs, or
    * -1 when the database/function is unavailable. Age is zero for an empty
    * queue. Runtime has read-only EXECUTE on the aggregate, never table access. */
   int db2_kb_audit_pending(long long *count, long long *oldest_age_seconds);

   /* Capture gate: the aimee-kb service enables this at init from
    * config.audit_worm_enabled; the central kb audit seam (db2_audit_event_write)
    * dual-writes into the WORM store only when enabled. Default-off. */
   void db2_kb_audit_worm_set_enabled(int enabled);
   int db2_kb_audit_worm_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_DB2_KB_AUDIT_WORM_H */
