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

   /* embedder-runtime-fetch-autodim §2a: mark whether the operator pinned the dim
    * (see config_embedding_dim_is_pinned). Call beside db2_set_embedding_dim,
    * before db2_init. When UNpinned (0, the default), db2_init prefers a recorded
    * kb_meta.schema_embedding_dim over the configured default; when pinned (1) the
    * operator value is authoritative and a recorded mismatch is refused downstream.
    * Reset by db2_shutdown so a reopen cannot inherit a previous run's state. */
   void db2_set_embedding_dim_pinned(int pinned);

   /* §2a precedence, pure (no globals, no I/O): the effective dim under
    * pin > recorded > configured-default. |pinned|: operator pinned a positive
    * dim. |configured|: the dim already set pre-init (the pin when pinned, else
    * the default). |recorded|: kb_meta.schema_embedding_dim, or <=0 if absent.
    * Returns |configured| when pinned or when nothing is recorded; the recorded
    * dim otherwise. The one place §2a's precedence lives — db2_init and the unit
    * test both call it. (A future probe rung, §2b, slots between recorded and the
    * default.) */
   int db2_effective_dim(int pinned, int configured, int recorded);

   /* embedder-runtime-fetch-autodim §2b: fresh-DB probe rung. On a fresh DB2 (no
    * operator pin, no recorded schema_embedding_dim) db2_init derives the dim from
    * the running embedder's /health, under a Postgres advisory lock, instead of the
    * default. db2 stays config-free via a registered probe seam (mirrors the
    * wfe_set_delegate_provider / g_forge pattern). */
#define DB2_PROBE_ERR_LEN 256
   /* Probe the embedder for its loaded-model output dim. Return 0 and set *out_dim
    * (>0) ONLY when the embedder reports a loaded model with a STABLE dim within
    * budget_ms; return -1 otherwise (still loading / unreachable / unstable /
    * timeout), filling err. Called only from db2_init (single-threaded, under the
    * init lock); not required to be reentrant. */
   typedef int (*db2_embedder_probe_fn)(int *out_dim, int budget_ms, char *err, size_t errlen);
   /* Register/clear the probe. The caller (kb/server) MUST call
    * db2_set_embedder_probe(NULL) BEFORE db2_shutdown(); db2_shutdown also NULLs it
    * defensively (mirrors the g_embed_dim_pinned reset discipline). With no probe
    * registered (cmd_core, tools) the §2b path is skipped — behavior is unchanged. */
   void db2_set_embedder_probe(db2_embedder_probe_fn fn);
   /* Total wall-clock budget (ms) for the §2b lock-acquire + probe. Set from config
    * (kb.embedder.dim_probe_budget_ms; default 120000) before db2_init, beside
    * db2_set_embedding_dim, so db2 stays config-free. */
   void db2_set_dim_probe_budget_ms(int ms);
   /* §2c: probe the running embedder for its current output dim (the dim-change
    * reset target). 0 + *out on success; -1 if no probe registered or it fails. */
   int db2_probe_embedder_dim(int budget_ms, int *out);

   /* §2b precedence, pure (no globals, no I/O): which source supplies the dim, given
    * whether the operator pinned, a dim is recorded, and a probe is available.
    * Encodes pin > recorded > probe > default; unit-tested directly. */
   typedef enum
   {
      DB2_DIM_SRC_PIN = 0,      /* operator pin authoritative */
      DB2_DIM_SRC_RECORDED = 1, /* recorded kb_meta dim wins over probe/default */
      DB2_DIM_SRC_PROBE = 2,    /* fresh DB: derived from the embedder /health probe */
      DB2_DIM_SRC_DEFAULT = 3   /* fresh DB, no probe available: configured default */
   } db2_dim_source_t;
   db2_dim_source_t db2_dim_source(int pinned, int recorded_present, int probe_available);

   /* embedder-runtime-fetch-autodim §2c: the double-gated dim-change reset. Reports
    * what it (would) do; the caller gates execution (config flag + --confirm). */
   typedef struct
   {
      int recorded_dim;    /* current schema_embedding_dim (or 0/absent) */
      int target_dim;      /* the dim to reset to */
      int n_tables;        /* halfvec vector tables discovered */
      char tables[16][64]; /* their names */
      int n_dropped;       /* tables dropped+recreated this run */
      long long rows_cleared;
      int curator_requeued;  /* artifacts re-queued for re-embed */
      int evidence_requeued; /* evidence ops re-queued */
      char detail[2048];     /* human-readable plan/result */
   } db2_reembed_plan_t;
   /* Plan (dry_run=1: report only, no changes) or EXECUTE a dim-change reset to
    * target_dim. force=1 allows DROP ... CASCADE when an inbound FK exists. Returns
    * 0 = ok (incl. a no-op when recorded==target — see plan->recorded_dim/target_dim);
    * -1 = error; -2 = an UNKNOWN halfvec table exists (refuse, don't guess); -3 = an
    * inbound FK needs --force. *out (optional) carries the report + counts. */
   int db2_dim_change_reset(int target_dim, int force, int dry_run, db2_reembed_plan_t *out);
   /* The reembed_in_progress maintenance marker the kb's health + search consult. */
   int db2_reembed_in_progress_get(int *target_dim, long *started_epoch);
   int db2_reembed_in_progress_clear(void);
   /* §2c operator escape hatch: clear a stuck marker, refusing (-1) when the recorded
    * schema dim disagrees with the running dim unless force (the dangerous mid-
    * transition case is then explicit). 0 cleared, -2 error; out params optional. */
   int db2_reembed_clear_maintenance(int force, int *was_in_progress, int *recorded, int *running);

   /* Model-identity drift guard (unified-llm-container §2). Set from config
    * before db2_init, beside db2_set_embedding_dim, so this layer stays
    * config-free. ALL default empty -> the guard is a no-op (a deployment whose
    * embedder reports no identity, e.g. the legacy torch embedder, is
    * unaffected). The embedder model_id is repo@sha; the reranker carries its
    * scoring contract (e.g. "/v1/rerank,fa=on"); compat_csv is a comma-separated
    * list of admitted "old_id->new_id" transitions (see
    * db2_embedding_model_record_or_check). Reset by db2_shutdown. */
   void db2_set_embedder_model_id(const char *model_id);
   const char *db2_embedder_model_id(void);
   void db2_set_reranker_identity(const char *model_id, const char *contract);
   const char *db2_reranker_model_id(void);
   const char *db2_reranker_contract(void);
   void db2_set_embedding_compat(const char *compat_csv);
   const char *db2_embedding_compat(void);

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
