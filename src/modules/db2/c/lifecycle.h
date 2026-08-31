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

   /* Read-only schema mode for one-shot callers that do not own the schema.
    *
    * db2_init APPLIES the schema by default, which is right for the daemon that
    * owns it and wrong for a CLI reading a running deployment: the DDL races the
    * daemon's own pass and Postgres reports "tuple concurrently updated", which
    * db2_init then reports as "DB2 not reachable" -- naming the wrong problem
    * entirely. Set this before db2_init to VERIFY the schema instead, the same
    * path AIMEE_KB_HARDENED already takes. Process-wide and off by default, so
    * the daemon is unaffected. */
   void db2_set_schema_readonly(int on);
   int db2_init(const char *libpq_url);

   /* Set the embedding dimension used to create the DB2 vector embedding
    * columns (one embedder per deployment). Call from startup — with
    * config_resolve_embedder_dims() — BEFORE db2_init() applies the schema, so
    * this layer stays config-free. Unset/<=0 means "nothing pinned", which lets
    * db2_init's §2a precedence fall through to the injected default. */
   void db2_set_embedding_dim(int dim);

   /* Inject the DEFAULT width, from config_embedder_dims_default(). This layer
    * holds no width literal of its own — the number is declared once, in config —
    * so call this beside db2_set_embedding_dim before db2_init. Without it
    * db2_embedding_dim() reports 0 and schema sizing fails loudly rather than
    * guessing a width the running embedder may not produce. */
   void db2_set_embedding_dim_default(int dim);

   /* The effective width: the pinned dim when set, else the injected default.
    * 0 means neither was supplied — treat as an error, never as a default. */
   int db2_embedding_dim(void);

   /* How many vector upserts have been refused for disagreeing with that width,
    * and the last width actually offered.
    *
    * A wrong EMBEDDER_DIMS is otherwise invisible: the kb starts, reports
    * healthy, accepts writes, and refuses every upsert with a per-row WARN, so a
    * deployment can store zero vectors indefinitely while looking fine. Health
    * publishes this so the condition is legible without reading kb logs. */
   long long db2_embedding_dim_refused_count(void);
   int db2_embedding_dim_last_offered(void);

   /* embedder-runtime-fetch-autodim §2a: mark whether the operator pinned the dim
    * (see config_embedder_dims_is_pinned). Call beside db2_set_embedding_dim,
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

   /* Should a start be refused because the embedder cannot produce the corpus's
    * recorded width? Pure. Refuses ONLY when the embedder answered (probe_rc == 0) with
    * a positive width that differs from a positive recorded width — an embedder that
    * did not answer is not evidence of drift. UPGRADING.md promises this refusal; a
    * v0.2 corpus at 1024 under a 384-dim bundled embedder used to come up healthy and
    * report embed_ok while Postgres bounced every write. */
   int db2_dim_drift_refuses(int probe_rc, int probed_dim, int recorded_dim);

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
      int n_tables;        /* vector vector tables discovered */
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
    * -1 = error; -2 = an UNKNOWN vector table exists (refuse, don't guess); -3 = an
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
    * unaffected). The embedder model_id is repo@sha; compat_csv is a
    * comma-separated list of admitted "old_id->new_id" transitions (see
    * db2_embedding_model_record_or_check). Reset by db2_shutdown. */
   void db2_set_embedder_model_id(const char *model_id);
   const char *db2_embedder_model_id(void);
   /* The serving endpoint's vector-space identity, PROBED from its /health rather than
    * read from config: pooling and prefixes are properties of what the embedder applies,
    * not of what the kb was told. Empty -> the guard is a no-op. */
   void db2_set_embedder_serving_id(const char *serving_id);
   const char *db2_embedder_serving_id(void);

   /* Fetch the identity AT THE POINT OF USE instead of before db2_init.
    *
    * The in-container embedder is a sibling process the entrypoint launches next to the
    * kb, so it is reliably NOT serving yet when the kb starts. Publishing the identity up
    * front therefore failed on every cold boot and left the guard inactive — the exact
    * hole it exists to close. db2_init calls this just before the guard, by which point
    * the dim probe has already waited for the embedder to be ready.
    *
    * Returns 0 on success, writing the identity (possibly EMPTY, meaning "this endpoint
    * reports none" — a legacy embedder, which must leave the guard inactive rather than
    * refuse). Non-zero means unreachable. */
   typedef int (*db2_embedder_serving_probe_fn)(char *out, size_t out_len, char *err,
                                                size_t errlen);
   void db2_set_embedder_serving_probe(db2_embedder_serving_probe_fn fn);
   /* Whether each probe seam is currently registered. The bug these exist for was a
    * registration decision no test could see: the caller skipped BOTH probes for the
    * builtin embedder because the DIM probe cannot work against it, which silently
    * disabled the serving-identity guard in the one transition it was written to catch.
    * A seam that decides whether a guard runs has to be observable. */
   int db2_embedder_probe_registered(void);
   int db2_embedder_serving_probe_registered(void);
   void db2_set_embedding_compat(const char *compat_csv);
   const char *db2_embedding_compat(void);

   /* Connection-pool size used by db2_init (default 16). Like
    * db2_set_embedding_dim, call before db2_init to keep this layer config-free. */
   void db2_set_pool_size(int size);
   /* Bracket a unit of work so the thread's pooled DB2 connection is returned to
    * the pool between units (instead of held for the thread's life). Re-entrant
    * (refcounted). db2_conn() lazily leases one if none is held; an unbracketed
    * lazy lease is returned when the thread exits. */
   /* See db2.h: the db2_lease_begin macro records the caller's file:line. */
   void db2_lease_begin_at(const char *site);
#ifndef db2_lease_begin
#define DB2_LEASE_STRINGIFY_(x) #x
#define DB2_LEASE_SITE_(f, l)   f ":" DB2_LEASE_STRINGIFY_(l)
#define db2_lease_begin()       db2_lease_begin_at(DB2_LEASE_SITE_(__FILE__, __LINE__))
#endif

   /* db2_conn() with the caller's file:line, so a LAZY acquire (outside any
    * db2_lease_begin scope) can be attributed too.
    *
    * That is the leak the reaper most needs to name: a long-lived worker that
    * takes a connection via db2_conn() at depth 0 and never calls
    * db2_lease_release_idle pins a pool member for its whole lifetime. Only
    * db2_lease_begin recorded a site, so exactly this case logged as
    * "unattributed" -- a prod kb sat with all 16 members held ~15h and the
    * reaper could not say by whom. The site is stored only when a connection is
    * actually acquired, not on every call. */
   void *db2_conn_at(const char *site);
#ifndef db2_conn
#define db2_conn() db2_conn_at(DB2_LEASE_SITE_(__FILE__, __LINE__))
#endif
   void db2_lease_end(void);

   int db2_fork_conn_url(char *out, size_t cap);
   int db2_health_probe(int *schema_ok, int *have_pg_trgm);
   int db2_kb_health_probe(int *kb_tables_ok);

   /* Returns the underlying postgres connection handle (or sqlite shim
    * handle in tests) for callers that need to dispatch SQL through
    * aimee_pg_* primitives directly. Production callers should prefer
    * the typed db2_* domain functions; this is exposed for KB-owned
    * migration tooling that copies arbitrary tables row-by-row. */
   void *(db2_conn)(void); /* parenthesised: the db2_conn() macro must not expand here */

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
