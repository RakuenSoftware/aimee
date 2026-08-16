/* test_db2_conn_open.c -- aimee_pg_open's contract with the connection it hands back.
 *
 * The sibling suite (test_db2_conn_bounds.c) proves the conninfo STRING is built
 * correctly. It cannot prove anything about aimee_pg_open itself, because that
 * function needs libpq to do something: a regression that dropped the
 * `SET statement_timeout` entirely, or returned an unbounded connection when the
 * SET failed, would pass every assertion over there. That gap was reported by a
 * review of this change and left open across two commits; this closes it.
 *
 * The seam is link-time interposition, not a live backend. The seven libpq
 * entry points aimee_pg_open calls are defined here, and a definition in an
 * object file takes precedence over the same symbol in a shared library, so the
 * real libpq is still linked for everything else but these calls land on the
 * fakes. That buys the one thing a live Postgres could not: the ability to make
 * `SET statement_timeout` FAIL on demand, which is the branch whose whole purpose
 * is to refuse a connection that would otherwise look perfectly healthy.
 */
#include "db_postgres.h"

#include "modules/db2/c/db2_pool.h" /* DB2_POOL_HOLD_CEILING_MS */

#include <libpq-fe.h>
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

/* --- the fake libpq ------------------------------------------------ */

static struct
{
   char last_conninfo[2048];
   char last_sql[256];
   int exec_calls;
   int finish_calls;
   int connect_calls;
   ConnStatusType status;      /* what PQstatus reports */
   ExecStatusType exec_status; /* what PQresultStatus reports */
   int exec_returns_null;      /* PQexec itself fails */
} fake;

static void fake_reset(void)
{
   memset(&fake, 0, sizeof fake);
   fake.status = CONNECTION_OK;
   fake.exec_status = PGRES_COMMAND_OK;
}

/* Distinct non-NULL sentinels; aimee_pg_open only ever passes them back to us. */
static int conn_token;
static int result_token;

PGconn *PQconnectdb(const char *conninfo)
{
   fake.connect_calls++;
   snprintf(fake.last_conninfo, sizeof fake.last_conninfo, "%s", conninfo ? conninfo : "");
   return (PGconn *)&conn_token;
}

ConnStatusType PQstatus(const PGconn *conn)
{
   (void)conn;
   return fake.status;
}

char *PQerrorMessage(const PGconn *conn)
{
   (void)conn;
   return (char *)"fake libpq: permission denied to set statement_timeout";
}

void PQfinish(PGconn *conn)
{
   (void)conn;
   fake.finish_calls++;
}

PGresult *PQexec(PGconn *conn, const char *query)
{
   (void)conn;
   fake.exec_calls++;
   snprintf(fake.last_sql, sizeof fake.last_sql, "%s", query ? query : "");
   return fake.exec_returns_null ? NULL : (PGresult *)&result_token;
}

ExecStatusType PQresultStatus(const PGresult *res)
{
   (void)res;
   return fake.exec_status;
}

void PQclear(PGresult *res)
{
   (void)res;
}

/* --- the contract -------------------------------------------------- */

/* The bound is applied to the connection, not merely computed. The SQL is
 * asserted against the pool's ceiling rather than a literal, so moving the
 * ceiling moves this too. */
static void test_the_statement_bound_is_applied_to_the_connection(void)
{
   fake_reset();
   unsetenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS");
   char err[256] = {0};
   void *c = aimee_pg_open("host=db dbname=aimee", err, sizeof err);
   must(c != NULL, "a healthy connection is returned");
   must(fake.exec_calls == 1, "exactly one SET is issued");

   /* BOTH bounds ride the connection. statement_timeout alone leaves the case
    * that actually wedged a kb — a transaction opened and then stalled before
    * its next statement, invisible to a statement bound, holding its pool member
    * for hours. Asserted against the pool ceiling rather than literals so moving
    * the ceiling moves this too. */
   char want[160];
   snprintf(want, sizeof want,
            "SET statement_timeout = %d; SET idle_in_transaction_session_timeout = %d",
            DB2_POOL_HOLD_CEILING_MS, DB2_POOL_HOLD_CEILING_MS);
   must(strcmp(fake.last_sql, want) == 0, "the SET carries both bounds at the pool's ceiling");
   /* And the connection it opened was the bounded string, not the caller's. */
   must(strstr(fake.last_conninfo, "connect_timeout=10") != NULL, "the conninfo was bounded");
   must(fake.finish_calls == 0, "a healthy connection is not closed");
   printf("  PASS: the statement bound is applied to the connection, at the pool ceiling\n");
}

/* An explicit 0 is the documented opt-out, and the two bounds opt out
 * INDEPENDENTLY: disabling the statement bound is not a request to also run
 * unbounded inside an idle transaction. Only opting out of both issues no SET. */
static void test_an_explicit_zero_issues_no_set(void)
{
   fake_reset();
   setenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS", "0", 1);
   setenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS", "0", 1);
   char err[256] = {0};
   void *c = aimee_pg_open("host=db", err, sizeof err);
   must(c != NULL, "the connection is still returned");
   must(fake.exec_calls == 0, "no SET is issued when both bounds are opted out");

   /* Statement bound off, idle bound left alone: the idle SET still goes. */
   fake_reset();
   unsetenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS");
   c = aimee_pg_open("host=db", err, sizeof err);
   must(c != NULL, "the connection is still returned");
   must(fake.exec_calls == 1, "the idle bound is still applied on its own");
   must(strstr(fake.last_sql, "idle_in_transaction_session_timeout") != NULL,
        "the surviving SET is the idle bound");
   must(strstr(fake.last_sql, "statement_timeout") == NULL,
        "the opted-out statement bound is not reinstated");
   unsetenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS");
   printf("  PASS: the two bounds opt out independently\n");
}

/* THE POINT OF THIS FILE. If the bound cannot be set, the connection must be
 * refused and closed — never handed back unbounded, which is exactly the state
 * the pool can observe but not reclaim. */
static void test_a_failed_set_refuses_and_closes_the_connection(void)
{
   /* The server rejects the statement. */
   fake_reset();
   fake.exec_status = PGRES_FATAL_ERROR;
   char err[256] = {0};
   must(aimee_pg_open("host=db", err, sizeof err) == NULL, "a rejected SET refuses the connection");
   must(fake.finish_calls == 1, "the refused connection is closed, not leaked");
   must(strstr(err, "statement_timeout") != NULL, "the error names the bound that failed");
   must(strstr(err, "permission denied") != NULL, "libpq's own reason is preserved");

   /* PQexec itself fails (out of memory, connection lost mid-SET). */
   fake_reset();
   fake.exec_returns_null = 1;
   err[0] = '\0';
   must(aimee_pg_open("host=db", err, sizeof err) == NULL, "a NULL PGresult also refuses");
   must(fake.finish_calls == 1, "and still closes the connection");
   printf("  PASS: a failed SET refuses the connection and closes it, keeping libpq's reason\n");
}

/* A connection that never came up is refused and closed before any SET. */
static void test_a_bad_connection_is_refused_before_the_set(void)
{
   fake_reset();
   fake.status = CONNECTION_BAD;
   char err[256] = {0};
   must(aimee_pg_open("host=db", err, sizeof err) == NULL, "a bad connection is refused");
   must(fake.exec_calls == 0, "no SET is attempted on a connection that is not up");
   must(fake.finish_calls == 1, "the bad connection is closed");
   printf("  PASS: a connection that never came up is refused before the SET\n");
}

/* If the bounds do not fit, aimee_pg_open must not connect at all — the previous
 * behaviour was to connect with an unbounded conninfo. */
static void test_an_unboundable_conninfo_never_connects(void)
{
   fake_reset();
   char big[2100];
   memset(big, 'x', sizeof big - 1);
   big[sizeof big - 1] = '\0';
   memcpy(big, "host=", 5);
   char err[256] = {0};
   must(aimee_pg_open(big, err, sizeof err) == NULL, "an unboundable conninfo is refused");
   must(fake.connect_calls == 0, "no connection is opened without the bounds");
   must(err[0] != '\0', "the caller is told why");
   printf("  PASS: a conninfo that cannot carry the bounds never opens a connection\n");
}

int main(void)
{
   printf("test_db2_conn_open:\n");
   test_the_statement_bound_is_applied_to_the_connection();
   test_an_explicit_zero_issues_no_set();
   test_a_failed_set_refuses_and_closes_the_connection();
   test_a_bad_connection_is_refused_before_the_set();
   test_an_unboundable_conninfo_never_connects();
   printf("All tests passed.\n");
   return 0;
}
