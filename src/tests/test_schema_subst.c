/* test_schema_subst.c: the DB2 schema is shipped with a __EMBED_DIM__
 * placeholder in its vector embedding columns so a deployment can run a single
 * embedder at its own dimension (768 for the default nomic embedder; older
 * deployments may still record the Qwen3 ladder's 1024 or 2560).
 * db_apply_schema_postgres() substitutes the configured dimension before
 * handing the DDL to Postgres. These tests capture the SQL the apply path would
 * execute and assert the substitution is complete and correct. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h" /* EMBED_MAX_DIM — the upper bound an unusable width is judged against */
#include "modules/db2/c/db_schema.h"
#include "modules/db2/c/db_postgres.h"

/* Capture the SQL db_apply_schema_postgres() hands to Postgres. */
static char *g_captured_sql = NULL;
static int g_fake_conn = 0;

int aimee_pg_exec(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   (void)errbuf;
   (void)errlen;
   assert(pg_conn == &g_fake_conn);
   free(g_captured_sql);
   g_captured_sql = strdup(sql ? sql : "");
   assert(g_captured_sql != NULL);
   return 0;
}

/* After applying the schema, db_apply_schema_postgres() records the embedding
 * dim in kb_meta via these aimee_pg_* calls (embedder-runtime-fetch-autodim §2):
 * an INSERT ... ON CONFLICT DO UPDATE ... RETURNING value upsert. This test only
 * cares about the captured DDL, so the prepared-statement path is stubbed to a
 * benign success: the upsert echoes back the dim it was handed (RETURNING the
 * just-written value), so the record-or-check sees recorded == embed_dim and
 * returns 0, leaving the apply rc==0. The record/refuse logic itself (mismatch,
 * invalid dim, corrupt row) is covered by test_embedding_dim.c. */
/* aimee_pg_stmt_t is opaque; a non-NULL sentinel is all the callee needs. */
static int g_fake_stmt;
static char g_bound_dim[16] = "0";

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   (void)pg_conn;
   (void)sql;
   (void)errbuf;
   (void)errlen;
   return (aimee_pg_stmt_t *)&g_fake_stmt;
}

void aimee_pg_finalize(aimee_pg_stmt_t *stmt)
{
   (void)stmt;
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *stmt, char *errbuf, size_t errlen)
{
   (void)stmt;
   (void)errbuf;
   (void)errlen;
   return AIMEE_PG_ROW; /* RETURNING yields one row carrying the recorded dim */
}

int aimee_pg_bind_text(aimee_pg_stmt_t *stmt, const char *name, const char *value)
{
   (void)stmt;
   (void)name;
   snprintf(g_bound_dim, sizeof(g_bound_dim), "%s", value ? value : "0");
   return 0;
}

const char *aimee_pg_column_text(aimee_pg_stmt_t *stmt, int col)
{
   (void)stmt;
   (void)col;
   return g_bound_dim; /* echo the just-written value back via RETURNING */
}

static const char *apply_with_dim(int dim)
{
   char err[128] = "";
   int rc = db_apply_schema_postgres(&g_fake_conn, dim, err, sizeof(err));
   assert(rc == 0);
   assert(g_captured_sql != NULL);
   return g_captured_sql;
}

/* An unusable width must be REFUSED. Returns the error text the caller would see. */
static const char *apply_with_dim_expect_refusal(int dim)
{
   static char err[512];
   err[0] = '\0';
   free(g_captured_sql);
   g_captured_sql = NULL;
   int rc = db_apply_schema_postgres(&g_fake_conn, dim, err, sizeof(err));
   assert(rc == -1);
   /* Nothing may be handed to Postgres: no DDL, valid or otherwise. */
   assert(g_captured_sql == NULL);
   assert(err[0] != '\0');
   return err;
}

/* Every placeholder must be replaced — no token may survive into the DDL. */
static void assert_fully_substituted(const char *sql)
{
   assert(strstr(sql, "__EMBED_DIM__") == NULL);
}

static void test_substitutes_explicit_dim(void)
{
   const char *sql = apply_with_dim(2560);
   assert(strstr(sql, "vector(2560)") != NULL);
   assert(strstr(sql, "vector(1024)") == NULL);
   assert_fully_substituted(sql);
}

static void test_default_dim(void)
{
   const char *sql = apply_with_dim(1024);
   assert(strstr(sql, "vector(1024)") != NULL);
   assert(strstr(sql, "vector(2560)") == NULL);
   assert_fully_substituted(sql);
}

/* dim <= 0 means the deployment's width never reached this layer. That is an error,
 * NOT a fallback: this layer declares no width (config does — see
 * config_embedder_dims_effective), and inventing one here would size a corpus for
 * an embedder that is not the one running. That exact substitution is how a kb
 * ended up with 1024-wide columns while the bundled model returned 384. */
static void test_unset_is_refused(void)
{
   const char *err = apply_with_dim_expect_refusal(0);
   assert(strstr(err, "unusable") != NULL);

   err = apply_with_dim_expect_refusal(-5);
   assert(strstr(err, "unusable") != NULL);
}

/* A dimension beyond what the embedding buffers can hold (EMBED_MAX_DIM) is
 * refused for the same reason — a misconfiguration must not silently become
 * columns of some other width. */
static void test_out_of_range_is_refused(void)
{
   const char *err = apply_with_dim_expect_refusal(EMBED_MAX_DIM + 1);
   assert(strstr(err, "unusable") != NULL);
}

/* The placeholder appears on multiple columns; substitution must replace all of
 * them, not just the first. */
static void test_all_columns_substituted(void)
{
   const char *sql = apply_with_dim(2560);
   /* memory_embeddings, kb_embeddings, and the curator vector tables all carry a
    * vector embedding column; expect several substituted occurrences. */
   int count = 0;
   for (const char *p = sql; (p = strstr(p, "vector(2560)")) != NULL; p += 13)
      count++;
   assert(count >= 5);
}

/* Optional runtime roles must never make owner-schema upgrades fail on the
 * single-user/dev tier. Pin the guard around the obsolete function's REVOKE;
 * PostgreSQL errors on REVOKE ... FROM a role that does not exist. */
static void test_optional_runtime_role_is_guarded(void)
{
   const char *sql = apply_with_dim(1024);
   const char *block = strstr(sql, "DO $drop_obsolete_bedrock_upsert$");
   assert(block != NULL);
   const char *guard = strstr(
       block,
       "IF EXISTS (SELECT 1 FROM pg_catalog.pg_roles WHERE rolname = 'aimee_kb_runtime') THEN");
   const char *revoke =
       strstr(block, "REVOKE ALL ON FUNCTION "
                     "public.org_catalog_bedrock_upsert(text,text,text,text,text,text,text,text[],"
                     "text[],text,boolean) FROM aimee_kb_runtime");
   assert(guard != NULL && revoke != NULL && guard < revoke);
}

int main(void)
{
   test_substitutes_explicit_dim();
   test_default_dim();
   test_unset_is_refused();
   test_out_of_range_is_refused();
   test_all_columns_substituted();
   test_optional_runtime_role_is_guarded();
   free(g_captured_sql);
   printf("schema_subst: all tests passed\n");
   return 0;
}
