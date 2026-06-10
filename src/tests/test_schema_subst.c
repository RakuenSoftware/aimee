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
