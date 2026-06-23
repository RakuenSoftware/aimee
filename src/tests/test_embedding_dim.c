/* test_embedding_dim.c: embedder-runtime-fetch-autodim §2 — kb_meta records the
 * schema embedding dim on first apply and REFUSES a later mismatch (the dim-drift
 * guard). Runs against the sqlite shim (schema_sqlite.sql provides kb_meta).
 *
 * Note: db2_test_shim_open() already applies the schema, which records the shim's
 * default dim — so the test first clears the row to exercise the fresh-record path
 * deterministically. */
#include "../db2/db_schema.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/lifecycle.h" /* db2_effective_dim */
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   printf("embedding-dim: ");
   db2_test_shim_open(); /* applies schema_sqlite.sql, incl. kb_meta */
   void *conn = db2_conn();
   assert(conn);
   char err[256];

   /* Start from a fresh kb_meta (shim apply already recorded a default dim). */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);

   /* First apply at 1024: records schema_embedding_dim, no error. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* Same dim again: matches, no-op, no error. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* A different dim is REFUSED (the columns are sized at the recorded 1024). */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 2560, err, sizeof err) == -1);
   assert(err[0] != '\0');                            /* a remediation message is set */
   assert(strstr(err, "1024") != NULL);               /* names the recorded dim */
   assert(strstr(err, "2560") != NULL);               /* and the configured dim */
   assert(strstr(err, "retrieval-stack.md") != NULL); /* and points at the remediation doc */

   /* The refusal did not change the recorded dim — 1024 still matches. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* A non-positive dim is rejected up front and must NOT be recorded (else the
    * guard would lock into an unrecoverable "matches forever" state). */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 0, err, sizeof err) == -1);
   assert(err[0] != '\0');
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, -5, err, sizeof err) == -1);
   /* ...and the existing 1024 row is untouched by those rejections. */
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* A corrupt / non-numeric kb_meta value is refused (not silently treated as
    * "no recorded dim" and re-created) — the operator must repair it. */
   assert(aimee_pg_exec(conn,
                        "UPDATE kb_meta SET value = 'garbage' WHERE key = 'schema_embedding_dim'",
                        err, sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == -1);
   assert(strstr(err, "corrupt") != NULL || strstr(err, "repair") != NULL);

   /* After a manual repair (clearing the bad row) recording succeeds again. */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 1024, err, sizeof err) == 0);

   /* ---- §2a: db2_effective_dim precedence (pure, pin > recorded > default) ---- */
   assert(db2_effective_dim(0, 1024, 2560) == 2560); /* unpinned: recorded wins over default */
   assert(db2_effective_dim(1, 1024, 2560) == 1024); /* pinned: operator value authoritative */
   assert(db2_effective_dim(0, 1024, 0) == 1024);    /* fresh DB: nothing recorded -> default */
   assert(db2_effective_dim(0, 2560, 2560) == 2560); /* match: caller sees eff==dim (no-op) */
   assert(db2_effective_dim(0, 1024, -1) == 1024);   /* non-positive recorded == absent */

   /* ---- §2a: db2_embedding_dim_get reader (quiet, bounded) ---- */
   /* The row is currently 1024 (record_or_check above). */
   assert(db2_embedding_dim_get(conn) == 1024);
   /* No row -> 0 (the "absent" signal). */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);
   assert(db2_embedding_dim_get(conn) == 0);
   /* A recorded in-range dim is returned verbatim. */
   assert(aimee_pg_exec(conn,
                        "INSERT INTO kb_meta (key, value) VALUES ('schema_embedding_dim', '2560')",
                        err, sizeof err) == 0);
   assert(db2_embedding_dim_get(conn) == 2560);
   /* Non-numeric -> 0 (no crash). */
   assert(aimee_pg_exec(conn,
                        "UPDATE kb_meta SET value = 'garbage' WHERE key = 'schema_embedding_dim'",
                        err, sizeof err) == 0);
   assert(db2_embedding_dim_get(conn) == 0);
   /* Out of range (> EMBED_MAX_DIM) -> 0 (guards against an operator typo). */
   assert(aimee_pg_exec(conn,
                        "UPDATE kb_meta SET value = '5000' WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);
   assert(db2_embedding_dim_get(conn) == 0);
   /* NULL conn -> 0. */
   assert(db2_embedding_dim_get(NULL) == 0);

   /* ---- §2b: db2_dim_source precedence (pure: pin > recorded > probe > default) ---- */
   assert(db2_dim_source(1, 1, 1) == DB2_DIM_SRC_PIN);      /* pin wins over all */
   assert(db2_dim_source(1, 0, 0) == DB2_DIM_SRC_PIN);      /* pin even with nothing else */
   assert(db2_dim_source(0, 1, 1) == DB2_DIM_SRC_RECORDED); /* recorded beats probe */
   assert(db2_dim_source(0, 0, 1) == DB2_DIM_SRC_PROBE);    /* fresh DB + probe -> probe */
   assert(db2_dim_source(0, 0, 0) == DB2_DIM_SRC_DEFAULT);  /* fresh DB, no probe -> default */

   /* ---- §2b: db2_embedding_dim_read tri-state (FOUND / ABSENT / ERROR) ---- */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);
   int rdim = -1;
   assert(db2_embedding_dim_read(conn, &rdim) == DB2_DIM_ABSENT); /* no row -> ABSENT */
   assert(rdim == 0);                                             /* out untouched on non-FOUND */
   assert(aimee_pg_exec(conn,
                        "INSERT INTO kb_meta (key, value) VALUES ('schema_embedding_dim', '768')",
                        err, sizeof err) == 0);
   rdim = 0;
   assert(db2_embedding_dim_read(conn, &rdim) == DB2_DIM_FOUND); /* in-range -> FOUND */
   assert(rdim == 768);
   assert(aimee_pg_exec(conn,
                        "UPDATE kb_meta SET value = 'garbage' WHERE key = 'schema_embedding_dim'",
                        err, sizeof err) == 0);
   rdim = 7;
   assert(db2_embedding_dim_read(conn, &rdim) == DB2_DIM_ABSENT); /* garbage -> ABSENT (quiet) */
   assert(rdim == 0);
   assert(db2_embedding_dim_read(NULL, &rdim) == DB2_DIM_ERROR); /* NULL conn -> ERROR */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);

   /* ---- EMBED_MAX_DIM bumped to 4000 (unified-llm-container §"8B truncation"):
    * a 4000-d dim now records cleanly (was rejected when the cap was 2560). ---- */
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedding_dim'", err,
                        sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedding_dim_record_or_check(conn, 4000, err, sizeof err) == 0);
   assert(db2_embedding_dim_get(conn) == 4000);

   /* ================= unified-llm-container §2: model-identity drift guard ====== */
   /* No-op when the embedder reports no identity (the legacy torch embedder). */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, NULL, NULL, err, sizeof err) == 0);
   assert(db2_embedding_model_record_or_check(conn, "", NULL, err, sizeof err) == 0);

   /* Fresh record, then a matching check is a no-op. */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "Qwen/Qwen3-Embedding-0.6B@abc", NULL, err,
                                              sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "Qwen/Qwen3-Embedding-0.6B@abc", NULL, err,
                                              sizeof err) == 0);

   /* The same-dim DIFFERENT-model swap is REFUSED (the footgun: pplx-embed and
    * Qwen3-0.6B are both 1024-d, so a dim-only guard would miss this). */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "perplexity-ai/pplx-embed-v1-0.6b", NULL, err,
                                              sizeof err) == -1);
   assert(err[0] != '\0');
   assert(strstr(err, "Qwen/Qwen3-Embedding-0.6B@abc") != NULL); /* names recorded */
   assert(strstr(err, "pplx-embed") != NULL);                    /* and configured */
   /* The refusal did not change the recorded identity. */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "Qwen/Qwen3-Embedding-0.6B@abc", NULL, err,
                                              sizeof err) == 0);

   /* A transition on the compat-list is ADMITTED (operator-validated cosine>=0.99)
    * and updates the recorded identity. Whitespace + multiple entries tolerated. */
   const char *compat =
       " other->x , Qwen/Qwen3-Embedding-0.6B@abc -> Qwen/Qwen3-Embedding-0.6B@def ";
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "Qwen/Qwen3-Embedding-0.6B@def", compat, err,
                                              sizeof err) == 0);
   /* Now the recorded id is @def; the old @abc would itself be a mismatch. */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "Qwen/Qwen3-Embedding-0.6B@abc", NULL, err,
                                              sizeof err) == -1);
   /* A compat entry that doesn't match the actual transition does NOT admit. */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "totally/different@ghi", "a->b,c->d", err,
                                              sizeof err) == -1);

   /* compat_admits edge cases (via the public guard; recorded id is @def now).
    * Malformed entries (no arrow, empty side) are silently skipped (not admitted);
    * newline-separated entries are tolerated. */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(conn, "Qwen/Qwen3-Embedding-0.6B@xyz",
                                              "no_arrow_here, ->, -> , a-->b", err,
                                              sizeof err) == -1); /* none admit */
   err[0] = '\0';
   assert(db2_embedding_model_record_or_check(
              conn, "Qwen/Qwen3-Embedding-0.6B@xyz",
              "junk->junk\nQwen/Qwen3-Embedding-0.6B@def->Qwen/Qwen3-Embedding-0.6B@xyz", err,
              sizeof err) == 0); /* newline-separated entry admits */

   /* Reranker identity is record-only (no corpus vectors / no score cache): a
    * swap never refuses, and the recorded value tracks the latest. */
   err[0] = '\0';
   assert(db2_reranker_model_record(conn, NULL, NULL, err, sizeof err) == 0); /* no-op */
   assert(db2_reranker_model_record(conn, "ettin-reranker-400m@v1", "/v1/rerank,fa=on", err,
                                    sizeof err) == 0);
   assert(db2_reranker_model_record(conn, "ettin-reranker-68m@v1", "/v1/rerank,fa=on", err,
                                    sizeof err) == 0); /* swap is fine */
   {
      char rr[160];
      assert(aimee_pg_exec(conn, "SELECT 1", err, sizeof err) == 0); /* conn ok */
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn, "SELECT value FROM kb_meta WHERE key = 'schema_reranker_model_id'",
                           err, sizeof err);
      assert(st && aimee_pg_step(st, err, sizeof err) == AIMEE_PG_ROW);
      snprintf(rr, sizeof rr, "%s", aimee_pg_column_text(st, 0));
      aimee_pg_finalize(st);
      assert(strcmp(rr, "ettin-reranker-68m@v1") == 0);
   }

   /* NULL conn -> -1 for both guards. */
   assert(db2_embedding_model_record_or_check(NULL, "x", NULL, err, sizeof err) == -1);
   assert(db2_reranker_model_record(NULL, "x", "y", err, sizeof err) == -1);

   db2_test_shim_close();
   printf("ok\n");
   return 0;
}
