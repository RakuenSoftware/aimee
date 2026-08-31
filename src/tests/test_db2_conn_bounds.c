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

#include "modules/db2/c/db2_pool.h" /* DB2_POOL_HOLD_CEILING_MS */

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
   /* And EXACTLY "0" — a leading zero is not the documented opt-out, so "00"
    * falls back rather than silently disabling the bound. */
   setenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS", "00", 1);
   must(db2_pg_statement_timeout_ms() == DB2_POOL_HOLD_CEILING_MS,
        "\"00\" is not the opt-out; it falls back");
   unsetenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS");
   printf("  PASS: an explicit override, including 0, is honoured\n");
}

/* A malformed value must fall back to the bound, never to "unlimited". Silently
 * removing the timeout on a typo is precisely the regression this guards. */
static void test_a_bad_value_falls_back_to_the_bound(void)
{
   /* The signed and whitespace-prefixed zeroes matter most. strtol accepts each
    * and returns 0 — the sentinel that DISABLES the bound — so without a
    * canonical-digits check these malformed spellings would silently opt out of
    * the safety property rather than fall back to it. It was inconsistent too:
    * " 0" disabled the bound while "0 " fell back. */
   const char *bad[] = {"",    "abc", "-1",  "12x", "  ",  "1e3",  "-0",  "+0",      " 0",     "0 ",
                        "\t0", "0\n", "0x0", "00",  "000", "0060", "007", "+300000", " 300000"};
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
   /* The tuning keys are still emitted and simply unused. One rule with no
    * exception beats a special case nobody remembers. */
   must(strstr(out, "keepalives_idle=30") != NULL, "unset tuning keys are still filled in");
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

   /* keepalives=1 without tuning must still get the tuning. Withholding it left
    * libpq's 7200s default idle where 30s was intended, so a setting whose point
    * is detecting a vanished peer would have taken two hours to notice. */
   db2_pg_conninfo_with_bounds("host=db keepalives=1", out, sizeof out);
   must(strstr(out, "keepalives_idle=30") != NULL, "an enabled keepalives still gets its idle");
   must(strstr(out, "keepalives_interval=10") != NULL, "and its interval");
   must(strstr(out, "keepalives_count=3") != NULL, "and its count");
   printf("  PASS: option detection matches whole keys, not substrings\n");
}

/* Option-shaped text inside a QUOTED value is not an option. A tenant name is
 * attacker-influenced in a multi-tenant deployment, and the failure is silent and
 * unsafe — the option looks set, so the bound is never added. */
static void test_quoted_values_cannot_suppress_the_bounds(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("host=db dbname='tenant keepalives=off'", out, sizeof out);
   must(strstr(out, "keepalives=1") != NULL, "text inside a quoted value does not suppress");
   must(strstr(out, "dbname='tenant keepalives=off'") != NULL, "the quoted value is preserved");

   db2_pg_conninfo_with_bounds("host=db password='p connect_timeout=1'", out, sizeof out);
   must(strstr(out, "connect_timeout=10") != NULL, "same for connect_timeout in a password");

   /* An escaped quote must not end the value early and expose the rest. */
   db2_pg_conninfo_with_bounds("host=db dbname='a\\' keepalives=off'", out, sizeof out);
   must(strstr(out, "keepalives=1") != NULL, "an escaped quote does not end the value early");

   /* A URI's password and path are not the query string. */
   db2_pg_conninfo_with_bounds("postgresql://u:keepalives=off@h/db", out, sizeof out);
   must(strstr(out, "keepalives=1") != NULL, "URI userinfo is not read as options");
   printf("  PASS: option-shaped text in a quoted value or URI authority is not an option\n");
}

/* Assert against libpq's OWN parser, not against the shape of our output.
 *
 * Every other case here is a string assertion, and string assertions are what let
 * the URI bug through: appending keywords to a URI produces output containing
 * "connect_timeout=10", so "the bounds appear" was true of a conninfo that in fact
 * put them inside dbname. Only the real parser can tell those apart.
 *
 * These assertions are MANDATORY, not best-effort. An earlier revision compiled
 * them out when libpq was missing and printed SKIP, so a fully green run did not
 * prove the parser check had executed — the same "green does not mean what it
 * says" trap this file keeps falling into. The target links libpq unconditionally
 * (see tests/Rules.mk), so a missing header is a build misconfiguration and must
 * break the build rather than quietly reduce coverage. */
#if defined(__has_include) && !__has_include(<libpq-fe.h>)
#error "test_db2_conn_bounds requires libpq: the PQconninfoParse assertions are not optional"
#endif
#include <libpq-fe.h>

static const char *opt_value(PQconninfoOption *opts, const char *key)
{
   for (PQconninfoOption *o = opts; o->keyword; o++)
      if (strcmp(o->keyword, key) == 0)
         return o->val ? o->val : "";
   return "";
}

/* Every field the commit message claims is checked IS checked. An earlier version
 * asserted only dbname while the message said host, port, dbname and user were
 * all confirmed — a smaller version of the same overclaim this change keeps
 * having to correct. Pass "" for a field the input does not set. */
static void parses_as(const char *conninfo, const char *want_dbname, const char *want_host,
                      const char *want_port, const char *want_user)
{
   char out[2048];
   must(db2_pg_conninfo_with_bounds(conninfo, out, sizeof out) == 0, "bounds fit");
   char *err = NULL;
   PQconninfoOption *opts = PQconninfoParse(out, &err);
   must(opts != NULL, "libpq parses the generated conninfo");
   /* The bug's signature was the bounds landing INSIDE dbname. */
   must(strcmp(opt_value(opts, "dbname"), want_dbname) == 0, "dbname is exactly the database");
   must(strcmp(opt_value(opts, "host"), want_host) == 0, "host survives intact");
   must(strcmp(opt_value(opts, "port"), want_port) == 0, "port survives intact");
   must(strcmp(opt_value(opts, "user"), want_user) == 0, "user survives intact");
   must(strcmp(opt_value(opts, "connect_timeout"), "10") == 0, "connect_timeout is a real option");
   must(strcmp(opt_value(opts, "keepalives"), "1") == 0, "keepalives is a real option");
   PQconninfoFree(opts);
   if (err)
      PQfreemem(err);
}

static void test_every_form_parses_as_libpq_intends(void)
{
   parses_as("postgresql://aimee:aimee@postgres:5432/aimee_shared", "aimee_shared", "postgres",
             "5432", "aimee");
   parses_as("postgres://h:5432/aimee_shared", "aimee_shared", "h", "5432", "");
   parses_as("postgresql://h/aimee_shared?sslmode=require", "aimee_shared", "h", "", "");
   parses_as("host=db dbname=aimee_shared user=aimee", "aimee_shared", "db", "", "aimee");
   /* A quoted value holding option-shaped text must not be read as options: the
    * bounds still land, and the database name survives intact. */
   parses_as("host=db dbname='tenant keepalives=off' user=aimee", "tenant keepalives=off", "db", "",
             "aimee");
   printf("  PASS: libpq parses every form with the bounds as real options\n");
}

/* libpq separates options on general whitespace, not just spaces and tabs. A
 * scanner that stops at ' ' and '\t' swallows "\nkeepalives=0" into the host
 * VALUE, misses the caller's explicit disable, and appends keepalives=1 — and
 * because the later setting wins, the appended default then OVERRIDES the
 * caller. Silently doing the opposite of what the conninfo asked for is worse
 * than not adding the bound at all, so this is asserted through libpq. */
static void test_newline_separated_options_are_seen(void)
{
   char out[2048];
   db2_pg_conninfo_with_bounds("host=db\nkeepalives=0", out, sizeof out);
   char *err = NULL;
   PQconninfoOption *opts = PQconninfoParse(out, &err);
   must(opts != NULL, "libpq parses the generated conninfo");
   must(strcmp(opt_value(opts, "host"), "db") == 0, "the host is not swallowed");
   must(strcmp(opt_value(opts, "keepalives"), "0") == 0,
        "the caller's explicit disable is not overridden");
   PQconninfoFree(opts);
   if (err)
      PQfreemem(err);

   /* A newline-separated conninfo that sets nothing still gains the bounds. */
   db2_pg_conninfo_with_bounds("host=db\ndbname=aimee_shared", out, sizeof out);
   opts = PQconninfoParse(out, &err);
   must(opts != NULL, "libpq parses it");
   must(strcmp(opt_value(opts, "connect_timeout"), "10") == 0, "bounds are still added");
   must(strcmp(opt_value(opts, "dbname"), "aimee_shared") == 0, "the dbname survives");
   PQconninfoFree(opts);
   if (err)
      PQfreemem(err);
   printf("  PASS: newline-separated options are seen, not swallowed into a value\n");
}

/* A URI already ending in '?' or '&' must not gain another separator: that makes
 * an empty query parameter, which libpq REJECTS outright ("missing key/value
 * separator"). The whole connection then fails, which is worse than any lost
 * bound — and `postgresql://h/db?` is a conninfo libpq accepts on its own, so a
 * legal input reaches it. Found by differential testing against libpq, not by
 * inspection. */
static void test_a_trailing_query_separator_does_not_break_the_uri(void)
{
   char out[2048];
   char *err = NULL;
   const char *inputs[] = {"postgresql://h/db?", "postgresql://h/db?keepalives=0&"};
   for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++)
   {
      db2_pg_conninfo_with_bounds(inputs[i], out, sizeof out);
      PQconninfoOption *opts = PQconninfoParse(out, &err);
      must(opts != NULL, "libpq still parses a URI with a trailing separator");
      must(strcmp(opt_value(opts, "connect_timeout"), "10") == 0, "the bound is still added");
      PQconninfoFree(opts);
      if (err)
      {
         PQfreemem(err);
         err = NULL;
      }
   }
   printf("  PASS: a URI ending in '?' or '&' gains no empty query parameter\n");
}

/* libpq percent-decodes URI query keys: connect%5Ftimeout=3 IS connect_timeout.
 * A raw byte compare misses it, appends the bound a second time, and the later
 * value wins — overriding the caller. */
static void test_percent_encoded_uri_keys_are_recognised(void)
{
   char out[2048];
   char *err = NULL;
   db2_pg_conninfo_with_bounds("postgresql://h/db?connect%5Ftimeout=3", out, sizeof out);
   PQconninfoOption *opts = PQconninfoParse(out, &err);
   must(opts != NULL, "libpq parses the generated conninfo");
   must(strcmp(opt_value(opts, "connect_timeout"), "3") == 0, "the caller's encoded value wins");
   PQconninfoFree(opts);
   if (err)
      PQfreemem(err);

   db2_pg_conninfo_with_bounds("postgresql://h/db?keepalives%5Fidle=60", out, sizeof out);
   opts = PQconninfoParse(out, &err);
   must(opts != NULL, "libpq parses it");
   must(strcmp(opt_value(opts, "keepalives_idle"), "60") == 0, "an encoded tuning key wins too");
   PQconninfoFree(opts);
   if (err)
      PQfreemem(err);
   printf("  PASS: percent-encoded URI query keys are recognised as the options they are\n");
}

/* statement_timeout bounds a STATEMENT. A unit of work that opens a transaction
 * and then stalls before its next statement is invisible to it and holds its
 * pool member indefinitely — measured at ~4.5 hours against a 5-minute ceiling
 * with statement_timeout correctly set the whole time. The idle bound is what
 * makes Postgres end that backend so the lease comes back without a restart. */
static void test_idle_in_transaction_bound(void)
{
   unsetenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS");
   must(db2_pg_idle_in_transaction_timeout_ms() == DB2_POOL_HOLD_CEILING_MS,
        "idle bound defaults to the pool hold ceiling");

   setenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS", "45000", 1);
   must(db2_pg_idle_in_transaction_timeout_ms() == 45000, "idle override honoured");

   /* Exactly "0" is the documented opt-out; every malformed spelling must fall
    * back to the bound rather than silently removing it. */
   setenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS", "0", 1);
   must(db2_pg_idle_in_transaction_timeout_ms() == 0, "exactly 0 disables");
   const char *bad[] = {"00", "-0", " 0", "+0", "007", "4294967296", "abc", ""};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      setenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS", bad[i], 1);
      must(db2_pg_idle_in_transaction_timeout_ms() == DB2_POOL_HOLD_CEILING_MS,
           "a malformed idle bound falls back rather than unbounding");
   }
   unsetenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS");
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
   test_no_room_fails_rather_than_dropping_the_bounds();
   test_an_out_of_range_override_falls_back();
   test_option_detection_matches_whole_keys();
   test_idle_in_transaction_bound();
   test_quoted_values_cannot_suppress_the_bounds();
   test_every_form_parses_as_libpq_intends();
   test_newline_separated_options_are_seen();
   test_a_trailing_query_separator_does_not_break_the_uri();
   test_percent_encoded_uri_keys_are_recognised();
   printf("All tests passed.\n");
   return 0;
}
