/* test_embedding_dim.c: embedder-runtime-fetch-autodim §2 — kb_meta records the
 * schema embedding dim on first apply and REFUSES a later mismatch (the dim-drift
 * guard). Runs against the sqlite shim (schema_sqlite.sql provides kb_meta).
 *
 * Note: db2_test_shim_open() already applies the schema, which records the shim's
 * default dim — so the test first clears the row to exercise the fresh-record path
 * deterministically. */
#include "../modules/db2/c/db_schema.h"
#include "../modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/db2_internal.h"
#include "../modules/db2/c/db_postgres.h"
#include "../modules/db2/c/lifecycle.h" /* db2_effective_dim */
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

   /* ---- dim-drift refusal (pure) ----
    * UPGRADING.md promises DB2 "refuses to start" when the embedder cannot produce the
    * recorded width. It did not: a v0.2.192 corpus recorded at 1024 came up under the
    * 384-dim bundled embedder reporting healthy and embed_ok:true, recorded that
    * embedder's serving identity over the corpus, and left Postgres to bounce every
    * write ("expected 1024 dimensions, not 384"). Inert while claiming to be well. */
   assert(db2_dim_drift_refuses(0, 384, 1024) == 1);  /* answered, and disagrees */
   assert(db2_dim_drift_refuses(0, 768, 1024) == 1);  /* the other bundled width too */
   assert(db2_dim_drift_refuses(0, 1024, 1024) == 0); /* answered, and agrees */
   /* No answer is NOT evidence of drift: a slow or silent embedder must still start,
    * or the fix takes down deployments that were working. */
   assert(db2_dim_drift_refuses(-1, 0, 1024) == 0);
   assert(db2_dim_drift_refuses(-1, 384, 1024) == 0); /* rc wins over a stale out-param */
   assert(db2_dim_drift_refuses(0, 0, 1024) == 0);    /* answered with nothing usable */
   assert(db2_dim_drift_refuses(0, 384, 0) == 0);     /* fresh DB: nothing recorded yet */

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

   /* The vector-space guard: pooling/prefix changes keep the dim AND the model name,
    * so this is the only guard that can see them. Unlike the model guard there is no
    * compat list — a different prefix pair is definitionally a different space. */
   err[0] = '\0';
   assert(db2_embedder_serving_record_or_check(conn, NULL, err, sizeof err) == 0); /* no-op */
   assert(db2_embedder_serving_record_or_check(conn, "", err, sizeof err) == 0);   /* no-op */
   assert(db2_embedder_serving_record_or_check(conn, "nomic/aaaa", err, sizeof err) == 0); /* rec */
   assert(db2_embedder_serving_record_or_check(conn, "nomic/aaaa", err, sizeof err) == 0); /* == */
   assert(err[0] == '\0');
   /* Same model, different digest = the prefix/pooling flip this guard exists for. */
   assert(db2_embedder_serving_record_or_check(conn, "nomic/bbbb", err, sizeof err) == -1);
   assert(strstr(err, "Re-embed") != NULL);
   /* The identity is a digest, so the message must not claim which field moved. */
   assert(strstr(err, "same dim") == NULL);
   assert(strstr(err, "nomic/aaaa") != NULL && strstr(err, "nomic/bbbb") != NULL);
   /* A refusal must not overwrite the recorded identity: the corpus is still the old
    * space until it is actually re-embedded. */
   err[0] = '\0';
   assert(db2_embedder_serving_record_or_check(conn, "nomic/aaaa", err, sizeof err) == 0);
   /* An endpoint that stops reporting an identity must not wipe the record either —
    * it degrades to a no-op, so the next endpoint that does report is still checked. */
   assert(db2_embedder_serving_record_or_check(conn, "", err, sizeof err) == 0);
   assert(db2_embedder_serving_record_or_check(conn, "nomic/bbbb", err, sizeof err) == -1);
   assert(db2_embedder_serving_record_or_check(NULL, "nomic/aaaa", err, sizeof err) == -1);

   /* The builtin lexical embedder declares an identity too, and switching off it must be
    * caught ONCE SOMETHING IS EMBEDDED. This is the one transition nothing else can see:
    * the builtin is 384-dim and so is the bundled model, so the dim guard is silent.
    *
    * An EMPTY corpus is the exception. The builtin serves a kb whose embedder has not
    * been chosen yet, so the first deploy records the placeholder before the wizard's
    * choice can reach the container; refusing there left a brand-new kb unable to adopt
    * the embedder it had just been given, with the documented escape (`aimee kb reembed`)
    * gated behind a setting inside the kb's own container. */
   err[0] = '\0';
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedder_serving_id'", err,
                        sizeof err) == 0);
   assert(aimee_pg_exec(conn, "DELETE FROM memory_embeddings", err, sizeof err) == 0);
   assert(db2_embedder_serving_record_or_check(conn, "builtin/lexical-v1", err, sizeof err) == 0);
   assert(db2_embedder_serving_record_or_check(conn, "builtin/lexical-v1", err, sizeof err) == 0);
   /* Empty corpus: the placeholder yields, and the new identity is what is now recorded. */
   assert(db2_embedder_serving_record_or_check(conn, "bekko-a25m/abcd", err, sizeof err) == 0);
   assert(err[0] == '\0');
   assert(db2_embedder_serving_record_or_check(conn, "bekko-a25m/abcd", err, sizeof err) == 0);
   /* Having yielded, it is an ordinary recorded identity — the next change is refused
    * even though the corpus is still empty. The escape is for the placeholder only. */
   assert(db2_embedder_serving_record_or_check(conn, "bekko-a25m/zzzz", err, sizeof err) == -1);
   assert(strstr(err, "bekko-a25m/abcd") != NULL && strstr(err, "bekko-a25m/zzzz") != NULL);

   /* Same placeholder, but the corpus HAS vectors: refused, and the record is unchanged.
    * This is the lexically-embedded corpus the guard was added for. */
   err[0] = '\0';
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedder_serving_id'", err,
                        sizeof err) == 0);
   assert(db2_embedder_serving_record_or_check(conn, "builtin/lexical-v1", err, sizeof err) == 0);
   assert(aimee_pg_exec(conn, "INSERT INTO memory_embeddings (point_id) VALUES (1)", err,
                        sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedder_serving_record_or_check(conn, "bekko-a25m/abcd", err, sizeof err) == -1);
   assert(strstr(err, "builtin/lexical-v1") != NULL && strstr(err, "bekko-a25m/abcd") != NULL);
   /* The refusal left the corpus on the placeholder. */
   err[0] = '\0';
   assert(db2_embedder_serving_record_or_check(conn, "builtin/lexical-v1", err, sizeof err) == 0);
   assert(aimee_pg_exec(conn, "DELETE FROM memory_embeddings", err, sizeof err) == 0);

   /* An UNREADABLE vector table is not an absent one. Emptiness is only proof when every
    * listed table was either shown absent or actually read; a table that exists but
    * cannot be queried must block adoption, or one broken table beside one empty table
    * reports a corpus as provably empty and the placeholder yields over real vectors.
    *
    * Renaming kb_embeddings away and leaving a VIEW that cannot be selected reproduces
    * "it is there, and I cannot read it" without needing a permissions system. */
   err[0] = '\0';
   assert(aimee_pg_exec(conn, "DELETE FROM kb_meta WHERE key = 'schema_embedder_serving_id'", err,
                        sizeof err) == 0);
   assert(db2_embedder_serving_record_or_check(conn, "builtin/lexical-v1", err, sizeof err) == 0);
   assert(aimee_pg_exec(conn, "ALTER TABLE kb_embeddings RENAME TO kb_embeddings_hidden", err,
                        sizeof err) == 0);
   assert(aimee_pg_exec(conn, "CREATE VIEW kb_embeddings AS SELECT * FROM missing_relation_xyz",
                        err, sizeof err) == 0);
   err[0] = '\0';
   assert(db2_embedder_serving_record_or_check(conn, "bekko-a25m/abcd", err, sizeof err) == -1);
   /* and the placeholder still stands */
   err[0] = '\0';
   assert(aimee_pg_exec(conn, "DROP VIEW kb_embeddings", err, sizeof err) == 0);
   assert(aimee_pg_exec(conn, "ALTER TABLE kb_embeddings_hidden RENAME TO kb_embeddings", err,
                        sizeof err) == 0);
   assert(db2_embedder_serving_record_or_check(conn, "builtin/lexical-v1", err, sizeof err) == 0);

   /* NULL conn -> -1. */
   assert(db2_embedding_model_record_or_check(NULL, "x", NULL, err, sizeof err) == -1);

   db2_test_shim_close();
   printf("ok\n");
   return 0;
}
