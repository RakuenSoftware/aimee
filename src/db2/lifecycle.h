/* db2/lifecycle.h: DB2 lifecycle/status API without the DB2 umbrella. */
#ifndef DEC_DB2_LIFECYCLE_H
#define DEC_DB2_LIFECYCLE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdint.h>

   int db2_is_initialized(void);
   int db2_init(const char *libpq_url);

   /* Set the embedding dimension used to create the DB2 halfvec embedding
    * columns (one embedder per deployment: 1024 for pplx-0.6b, 2560 for
    * pplx-4b). Call from startup — with the loaded config's embedding_dim —
    * BEFORE db2_init() applies the schema, so this layer stays config-free.
    * Unset/<=0 means the 1024 default. */
   void db2_set_embedding_dim(int dim);
   int db2_embedding_dim(void);

   /* Connection-pool size used by db2_init (default 16). Like
    * db2_set_embedding_dim, call before db2_init to keep this layer config-free. */
   void db2_set_pool_size(int size);
   /* Bracket a unit of work so the thread's pooled DB2 connection is returned to
    * the pool between units (instead of held for the thread's life). Re-entrant
    * (refcounted). db2_conn() lazily leases one if none is held; an unbracketed
    * lazy lease is returned when the thread exits. */
   void db2_lease_begin(void);
   void db2_lease_end(void);

   int db2_fork_conn_url(char *out, size_t cap);
   int db2_health_probe(int *schema_ok, int *have_pg_trgm);
   int db2_kb_health_probe(int *kb_tables_ok);

   /* Returns the underlying postgres connection handle (or sqlite shim
    * handle in tests) for callers that need to dispatch SQL through
    * aimee_pg_* primitives directly. Production callers should prefer
    * the typed db2_* domain functions; this is exposed for KB-owned
    * migration tooling that copies arbitrary tables row-by-row. */
   void *db2_conn(void);

   /* Postgres-native diagnostics for `aimee doctor`. Each out parameter
    * is set to -1 on probe failure; returns 0 when the connection is
    * alive. Skipped (returns 0 with all outputs left at -1) under the
    * test shim. */
   int db2_pg_stat_summary(int *active_conns, int *max_conns, int *is_replica,
                           int64_t *replica_lag_bytes);

   void db2_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_LIFECYCLE_H */
