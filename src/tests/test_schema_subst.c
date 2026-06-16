/* test_schema_subst.c: the DB2 schema is shipped with a __EMBED_DIM__
 * placeholder in its halfvec embedding columns so a deployment can run a single
 * embedder at its own dimension (1024 for pplx-0.6b, 2560 for pplx-4b).
 * db_apply_schema_postgres() substitutes the configured dimension before
 * handing the DDL to Postgres. These tests capture the SQL the apply path would
 * execute and assert the substitution is complete and correct. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db2/db_schema.h"
#include "db2/db_postgres.h"

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

/* Every placeholder must be replaced — no token may survive into the DDL. */
static void assert_fully_substituted(const char *sql)
{
   assert(strstr(sql, "__EMBED_DIM__") == NULL);
}

static void test_substitutes_explicit_dim(void)
{
   const char *sql = apply_with_dim(2560);
   assert(strstr(sql, "halfvec(2560)") != NULL);
   assert(strstr(sql, "halfvec(1024)") == NULL);
   assert_fully_substituted(sql);
}

static void test_default_dim(void)
{
   const char *sql = apply_with_dim(1024);
   assert(strstr(sql, "halfvec(1024)") != NULL);
   assert(strstr(sql, "halfvec(2560)") == NULL);
   assert_fully_substituted(sql);
}

/* dim <= 0 means "unset" — fall back to the 1024 default rather than emit
 * invalid DDL like halfvec(0). */
static void test_unset_falls_back_to_default(void)
{
   const char *sql = apply_with_dim(0);
   assert(strstr(sql, "halfvec(1024)") != NULL);
   assert_fully_substituted(sql);

   sql = apply_with_dim(-5);
   assert(strstr(sql, "halfvec(1024)") != NULL);
   assert_fully_substituted(sql);
}

/* A dimension beyond what the embedding buffers can hold (EMBED_MAX_DIM) is
 * clamped back to the default — a deployment misconfiguration must not produce
 * columns the code can never fill. */
static void test_out_of_range_falls_back_to_default(void)
{
   const char *sql = apply_with_dim(1000000);
   assert(strstr(sql, "halfvec(1024)") != NULL);
   assert(strstr(sql, "halfvec(1000000)") == NULL);
   assert_fully_substituted(sql);
}

/* The placeholder appears on multiple columns; substitution must replace all of
 * them, not just the first. */
static void test_all_columns_substituted(void)
{
   const char *sql = apply_with_dim(2560);
   /* memory_embeddings, kb_embeddings, and the curator vector tables all carry a
    * halfvec embedding column; expect several substituted occurrences. */
   int count = 0;
   for (const char *p = sql; (p = strstr(p, "halfvec(2560)")) != NULL; p += 13)
      count++;
   assert(count >= 5);
}

int main(void)
{
   test_substitutes_explicit_dim();
   test_default_dim();
   test_unset_falls_back_to_default();
   test_out_of_range_falls_back_to_default();
   test_all_columns_substituted();
   free(g_captured_sql);
   printf("schema_subst: all tests passed\n");
   return 0;
}
