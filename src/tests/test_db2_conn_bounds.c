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

/* No room must FAIL, not silently hand back an unbounded conninfo. An earlier
 * revision returned the base unchanged, which is well-formed and therefore
 * connects — losing the bounds on precisely the long TLS conninfo most likely to
 * need them, with nothing anywhere to say so. */
static void test_no_room_fails_rather_than_dropping_the_bounds(void)
{
   /* Big enough for the conninfo itself, far too small for the bounds. */
   char out[40];
   must(db2_pg_conninfo_with_bounds("host=a-fairly-long-hostname-here", out, sizeof out) == -1,
        "no room is reported as a failure");
   must(out[0] == '\0', "no unbounded conninfo is left behind for a caller to use");
   printf("  PASS: too little room fails instead of silently dropping the bounds\n");
}

/* An out-of-range override must fall back to the bound. The truncating cast makes
 * this the sharpest case of all: 4294967296 narrows to int 0, which is the
 * sentinel for "no statement timeout", so an unchecked overflow removes the bound
 * entirely rather than merely setting an odd one. */
static void test_an_out_of_range_override_falls_back(void)
{
   const char *over[] = {"4294967296", "5000000000", "99999999999999999999", "-9999999999"};
   for (size_t i = 0; i < sizeof over / sizeof over[0]; i++)
   {
      setenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS", over[i], 1);
      must(db2_pg_statement_timeout_ms() == 300000,
           "an out-of-range value falls back to the bound");
   }
   unsetenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS");
   printf("  PASS: an out-of-range override falls back, never to 0 or a negative\n");
}

/* Whole-key matching, not substring. Both of these were live defects: a value
 * containing the word suppressed the option, and one keepalive key suppressed the
 * whole group. */
static void test_option_detection_matches_whole_keys(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("host=db dbname=keepalives_test", out, sizeof out);
   must(strstr(out, "keepalives=1") != NULL, "a value containing the word does not suppress it");

   db2_pg_conninfo_with_bounds("host=db dbname=connect_timeout_db", out, sizeof out);
   must(strstr(out, "connect_timeout=10") != NULL, "same for connect_timeout inside a value");

   /* A caller who tuned one keepalive key wants that idle — not the loss of
    * keepalives altogether. Their key survives, and only it; the rest are filled
    * in, because the guarantee is that every connection carries the group. */
   db2_pg_conninfo_with_bounds("host=db keepalives_idle=30", out, sizeof out);
   must(strstr(out, "keepalives=1") != NULL, "keepalives are still enabled");
   const char *idle = strstr(out, "keepalives_idle=");
   must(idle != NULL && strncmp(idle, "keepalives_idle=30", 18) == 0, "the caller's idle survives");
   must(strstr(idle + 1, "keepalives_idle=") == NULL, "keepalives_idle is not specified twice");
   must(strstr(out, "keepalives_interval=10") != NULL, "the unset tuning keys are filled in");
   must(strstr(out, "keepalives_count=3") != NULL, "the unset tuning keys are filled in");
   must(strstr(out, "connect_timeout=10") != NULL, "the unrelated bound is still added");

   /* An explicit keepalives= is the caller owning the group, including off. */
   db2_pg_conninfo_with_bounds("host=db keepalives=0", out, sizeof out);
   must(strstr(out, "keepalives=0") != NULL, "an explicit disable survives");
   must(strstr(out, "keepalives_idle") == NULL, "no tuning keys are layered onto a disable");
   printf("  PASS: option detection matches whole keys, not substrings\n");
}

/* Assert against libpq's OWN parser, not against the shape of our output.
 *
 * Every other case here is a string assertion, and string assertions are what let
 * the URI bug through: appending keywords to a URI produces output containing
 * "connect_timeout=10", so "the bounds appear" was true of a conninfo that in fact
 * put them inside dbname. Only the real parser can tell those apart. Gated on
 * libpq being present so the minimal-link build still runs everything above. */
#if defined(__has_include)
#if __has_include(<libpq-fe.h>) && !defined(AIMEE_DISABLE_POSTGRES)
#define CONN_BOUNDS_HAVE_LIBPQ 1
#endif
#endif

#ifdef CONN_BOUNDS_HAVE_LIBPQ
#include <libpq-fe.h>

static const char *opt_value(PQconninfoOption *opts, const char *key)
{
   for (PQconninfoOption *o = opts; o->keyword; o++)
      if (strcmp(o->keyword, key) == 0)
         return o->val ? o->val : "";
   return "";
}

static void parses_as(const char *conninfo, const char *want_dbname)
{
   char out[2048];
   must(db2_pg_conninfo_with_bounds(conninfo, out, sizeof out) == 0, "bounds fit");
   char *err = NULL;
   PQconninfoOption *opts = PQconninfoParse(out, &err);
   must(opts != NULL, "libpq parses the generated conninfo");
   /* The bug's signature was the bounds landing INSIDE dbname. */
   must(strcmp(opt_value(opts, "dbname"), want_dbname) == 0, "dbname is exactly the database");
   must(strcmp(opt_value(opts, "connect_timeout"), "10") == 0, "connect_timeout is a real option");
   must(strcmp(opt_value(opts, "keepalives"), "1") == 0, "keepalives is a real option");
   PQconninfoFree(opts);
   if (err)
      PQfreemem(err);
}

static void test_every_form_parses_as_libpq_intends(void)
{
   parses_as("postgresql://aimee:aimee@postgres:5432/aimee_shared", "aimee_shared");
   parses_as("postgres://h:5432/aimee_shared", "aimee_shared");
   parses_as("postgresql://h/aimee_shared?sslmode=require", "aimee_shared");
   parses_as("host=db dbname=aimee_shared user=aimee", "aimee_shared");
   printf("  PASS: libpq parses every form with the bounds as real options\n");
}
#else
static void test_every_form_parses_as_libpq_intends(void)
{
   printf("  SKIP: libpq unavailable in this build; parser assertions not run\n");
}
#endif

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
   test_no_room_fails_rather_than_dropping_the_bounds();
   test_an_out_of_range_override_falls_back();
   test_option_detection_matches_whole_keys();
   test_every_form_parses_as_libpq_intends();
   printf("All tests passed.\n");
   return 0;
}
