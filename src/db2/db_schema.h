#ifndef DEC_DB2_DB_SCHEMA_H
#define DEC_DB2_DB_SCHEMA_H 1

/* DB2 (Postgres) idempotent schema bootstrap. The DB1 (SQLite) half
 * of the split lives in db1/db_schema.h. See docs/STORAGE_TIERS.md. */

#include <stddef.h>

struct sqlite3;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Apply the consolidated Postgres schema to an already-open libpq
    * connection (PGconn *, passed as void * so this header stays
    * libpq-free). |embed_dim| is the deployment's configured embedding
    * dimension (e.g. 1024 for pplx-0.6b, 2560 for pplx-4b); the schema's
    * halfvec embedding columns are created at that dimension. A value <= 0 or
    * > EMBED_MAX_DIM falls back to the 1024 default. Returns 0 on success, -1
    * on failure (writes to errbuf/errlen). */
   int db_apply_schema_postgres(void *pg_conn, int embed_dim, char *errbuf, size_t errlen);

   /* embedder-runtime-fetch-autodim §2: record schema_embedding_dim in kb_meta on
    * first apply; on a later apply with a different dim, REFUSE (the halfvec
    * columns are already sized at the recorded dim — serving at another silently
    * breaks vector search). Returns 0 (recorded/matches), -1 (mismatch/DB error,
    * errbuf set). aimee_pg_*-based so it runs on Postgres and the sqlite shim.
    * |conn| is the aimee_pg connection handle. Called by db_apply_schema_postgres
    * after the schema applies; exposed for direct testing. */
   int db2_embedding_dim_record_or_check(void *conn, int embed_dim, char *errbuf, size_t errlen);

   /* embedder-runtime-fetch-autodim §2a: read the recorded kb_meta
    * .schema_embedding_dim as the *source* of the runtime dim (the companion read
    * to record_or_check's write/guard). Returns the recorded dim when in range
    * (1..EMBED_MAX_DIM), else 0 — the "absent" signal db2_effective_dim() treats
    * as not-present: no row, empty/non-numeric/non-positive, or out of range
    * (guards strtol against an operator typo). Read-only; never writes; never
    * errors loudly (a missing/garbage row must not crash a read). |conn| is the
    * aimee_pg connection handle; aimee_pg_*-based so it runs on Postgres and the
    * sqlite shim. */
   int db2_embedding_dim_get(void *conn);

   /* §2b: tri-state read distinguishing a genuinely-absent recorded dim (expected
    * on a fresh DB) from a DB query ERROR — the §2b probe path must NOT treat a
    * lost connection / missing table as "absent" and bootstrap over it. *out is
    * set only on FOUND (an in-range 1..EMBED_MAX_DIM value); a no-row or
    * garbage/out-of-range row → ABSENT (quiet, as §2a); a prepare/step failure →
    * ERROR. (db2_embedding_dim_get stays as the value-or-0 wrapper for §2a.) */
   typedef enum
   {
      DB2_DIM_FOUND = 0,
      DB2_DIM_ABSENT = 1,
      DB2_DIM_ERROR = -1
   } db2_dim_read_t;
   db2_dim_read_t db2_embedding_dim_read(void *conn, int *out);

   /* unified-llm-container §2: record/check the EMBEDDER model identity
    * (repo@sha) in kb_meta.schema_embedder_model_id alongside the dim. A dim-only
    * guard is insufficient (two models can share a dim — pplx-embed and
    * Qwen3-0.6B are both 1024-d), so a same-dim swap would silently mix vector
    * spaces. model_id NULL/empty -> no-op (legacy torch embedder reports no
    * identity). compat_csv is a comma-separated list of admitted "old->new"
    * transitions (membership only; the cosine>=0.99 validation is the operator's
    * criterion for adding an entry). Returns 0 (recorded/match/admitted), -1
    * (unadmitted mismatch / DB error, errbuf set). Called by
    * db_apply_schema_postgres; exposed for direct testing. */
   int db2_embedding_model_record_or_check(void *conn, const char *model_id, const char *compat_csv,
                                           char *errbuf, size_t errlen);

   /* unified-llm-container §2: record the RERANKER identity + scoring contract in
    * kb_meta. Record-only (no corpus vectors, no persisted score cache to
    * invalidate) — never refuses. model_id NULL/empty -> no-op. Returns 0 / -1. */
   int db2_reranker_model_record(void *conn, const char *model_id, const char *contract,
                                 char *errbuf, size_t errlen);

   /* Apply the consolidated SQLite schema for DB2's libpq shim/test
    * compatibility path. Production DB2 remains Postgres-only. */
   int db2_apply_schema_sqlite_shim(struct sqlite3 *db, char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_DB_SCHEMA_H */
