/* test_db2_conn_bounds.c -- the bounds every pooled Postgres connection carries.
 *
 * These exist because aimee_pg_open set no bounds at all. Without a
 * statement_timeout, "hung forever" and "slow" are the same thing to the pool,
 * and the distinction matters: the reaper CANNOT reclaim an over-ceiling lease
 * (libpq forbids concurrent use of a connection a live thread still holds), so
 * bounding the statement is the only lever at this layer.
 *
 * These tests are NOT a regression test for a diagnosed outage. They were written
 * while investigating a wedged knowledge service, and that investigation did not
 * establish this as the cause — see the commit message. Treat them as a statement
 * about the bounds themselves.
 *
 * The pool already declares 300s as the longest a lease may reasonably be held.
 * The point of these tests is that the figure is now ENFORCED, and that no
 * ordinary mistake — a typo in an env var, a caller passing its own conninfo —
 * can quietly restore the unbounded behaviour.
 */
#include "db_postgres.h"

#include "db2_pool.h" /* DB2_POOL_HOLD_CEILING_MS */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void must(int cond, const char *what)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL: %s\n", what);
      exit(1);
   }
}

/* The default matches the pool's own stuck-lease ceiling: a statement must not
 * outlive the duration that defines "stuck". */
static void test_default_matches_the_pool_ceiling(void)
{
   unsetenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS");
   /* Assert the LINKAGE, not the number. These were two independent 300000
    * literals that happened to agree; if the pool's ceiling moves and the
    * statement bound does not, the pool goes back to reporting stuck leases it
    * cannot stop — which is the state this whole change exists to remove. */
   must(db2_pg_statement_timeout_ms() == DB2_POOL_HOLD_CEILING_MS,
        "the default statement bound IS the pool's hold ceiling");
   printf("  PASS: default bounds a statement at the pool's stuck-lease ceiling\n");
}

static void test_env_override_is_honoured(void)
{
   setenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS", "60000", 1);
   must(db2_pg_statement_timeout_ms() == 60000, "override honoured");
   /* 0 is a deliberate opt-out, not a typo: it must be respected so a deployment
    * with genuinely unbounded work can choose it explicitly. */
   setenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS", "0", 1);
   must(db2_pg_statement_timeout_ms() == 0, "explicit 0 disables the bound");
   unsetenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS");
   printf("  PASS: an explicit override, including 0, is honoured\n");
}

/* A malformed value must fall back to the bound, never to "unlimited". Silently
 * removing the timeout on a typo is precisely the regression this guards. */
static void test_a_bad_value_falls_back_to_the_bound(void)
{
   const char *bad[] = {"", "abc", "-1", "12x", "  ", "1e3"};
   for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
   {
      setenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS", bad[i], 1);
      must(db2_pg_statement_timeout_ms() == DB2_POOL_HOLD_CEILING_MS,
           "malformed value falls back to the ceiling");
   }
   unsetenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS");
   printf("  PASS: a malformed override falls back to the bound, never to unlimited\n");
}

/* Every connection gets a connect timeout and keepalives. Keepalives matter
 * independently of statement_timeout: a peer that disappears without a FIN
 * leaves a read blocked forever, and the server never sees the query to time
 * it out. */
static void test_bounds_are_added_to_a_plain_conninfo(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("host=db user=aimee", out, sizeof out);
   must(strstr(out, "host=db user=aimee") != NULL, "the caller's conninfo is preserved");
   must(strstr(out, "connect_timeout=") != NULL, "a connect timeout is added");
   must(strstr(out, "keepalives=1") != NULL, "keepalives are enabled");
   must(strstr(out, "keepalives_idle=") != NULL, "a keepalive idle is set");
   must(strstr(out, "keepalives_count=") != NULL, "a keepalive count is set");
   printf("  PASS: a plain conninfo gains a connect timeout and keepalives\n");
}

static void test_a_null_conninfo_is_still_bounded(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds(NULL, out, sizeof out);
   must(strstr(out, "connect_timeout=") != NULL, "NULL conninfo still gets a connect timeout");
   must(strstr(out, "keepalives=1") != NULL, "NULL conninfo still gets keepalives");
   printf("  PASS: a NULL conninfo is bounded too\n");
}

/* An explicit caller setting wins — the bounds must not be appended twice, which
 * would leave the effective value ambiguous. */
static void test_caller_supplied_settings_are_not_overridden(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("host=db connect_timeout=3", out, sizeof out);
   must(strstr(out, "connect_timeout=3") != NULL, "the caller's connect_timeout survives");
   const char *first = strstr(out, "connect_timeout");
   must(strstr(first + 1, "connect_timeout") == NULL, "connect_timeout is not duplicated");

   db2_pg_conninfo_with_bounds("host=db keepalives=0", out, sizeof out);
   must(strstr(out, "keepalives=0") != NULL, "the caller's keepalives choice survives");
   must(strstr(out, "keepalives=1") == NULL, "keepalives are not forced on top");
   printf("  PASS: an explicit caller setting is not overridden or duplicated\n");
}

/* The form that actually ships: AIMEE_DB2_URL is a URI in every compose file.
 *
 * The first version of this change appended " connect_timeout=10 ..." to it
 * unconditionally and took the KB container down in e2e. The failure mode is
 * worse than a rejected string — libpq ACCEPTS it, and folds the appended text
 * into the database name:
 *
 *   dbname = "aimee_shared connect_timeout=10 keepalives=1"
 *
 * so the connection fails on a database that does not exist, AND the bounds are
 * silently never applied. A test asserting only "the bounds appear in the output"
 * passes on that string, which is exactly what happened; these cases assert the
 * SHAPE the URI form requires. */
static void test_a_uri_gets_query_parameters_not_keywords(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("postgresql://aimee:aimee@postgres:5432/aimee_shared", out,
                               sizeof out);
   must(strncmp(out, "postgresql://aimee:aimee@postgres:5432/aimee_shared", 50) == 0,
        "the URI is preserved");
   must(strstr(out, "?connect_timeout=") != NULL, "the first parameter opens the query string");
   must(strstr(out, "&keepalives=1") != NULL, "later parameters extend it with &");
   must(strstr(out, " ") == NULL, "a URI never gains a space-separated keyword");
   printf("  PASS: a URI gains query parameters, never keyword/value pairs\n");
}

/* A URI that already carries a parameter must extend the query string, not open
 * a second one — two '?' is as invalid as a space. */
static void test_a_uri_with_existing_query_is_extended(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("postgresql://h/db?sslmode=require", out, sizeof out);
   must(strstr(out, "sslmode=require") != NULL, "the caller's parameter survives");
   must(strchr(out, '?') == strrchr(out, '?'), "only one '?' in the URI");
   must(strstr(out, "&connect_timeout=") != NULL, "bounds are appended with &");
   printf("  PASS: a URI with an existing query string is extended, not restarted\n");
}

/* postgres:// is the same thing under an older name. */
static void test_the_short_uri_scheme_is_recognised(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("postgres://h/db", out, sizeof out);
   must(strstr(out, "?connect_timeout=") != NULL, "postgres:// is treated as a URI");
   printf("  PASS: the postgres:// scheme is recognised as a URI too\n");
}

/* Truncation would produce a conninfo that is not merely unbounded but malformed,
 * so a base that leaves no room is returned unchanged. */
static void test_no_room_returns_the_conninfo_unchanged(void)
{
   /* Big enough for the conninfo itself, far too small for the bounds. */
   char out[40];
   db2_pg_conninfo_with_bounds("host=a-fairly-long-hostname-here", out, sizeof out);
   must(strcmp(out, "host=a-fairly-long-hostname-here") == 0, "returned unchanged, not truncated");
   printf("  PASS: too little room yields the conninfo unchanged, never a truncated one\n");
}

int main(void)
{
   printf("test_db2_conn_bounds:\n");
   test_default_matches_the_pool_ceiling();
   test_env_override_is_honoured();
   test_a_bad_value_falls_back_to_the_bound();
   test_bounds_are_added_to_a_plain_conninfo();
   test_a_null_conninfo_is_still_bounded();
   test_caller_supplied_settings_are_not_overridden();
   test_a_uri_gets_query_parameters_not_keywords();
   test_a_uri_with_existing_query_is_extended();
   test_the_short_uri_scheme_is_recognised();
   test_no_room_returns_the_conninfo_unchanged();
   printf("All tests passed.\n");
   return 0;
}
