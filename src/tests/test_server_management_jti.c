#define SERVER_MANAGEMENT_JTI_TEST_API 1
#include "db1.h"
#include "db1_internal.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static int reject_commit(void *arg)
{
   int *calls = arg;
   (*calls)++;
   return 1;
}

static int64_t monotonic_ms(void)
{
   struct timespec ts = {0};
   assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
   return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t scalar(const char *sql)
{
   sqlite3_stmt *stmt = NULL;
   assert(sqlite3_prepare_v2(db1_conn(), sql, -1, &stmt, NULL) == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int64_t value = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return value;
}

static server_management_jti_t token(const char *jti, int64_t issued_at, int64_t expires_at)
{
   server_management_jti_t t = {
       .jti = jti,
       .issuer = "https://kb.example.test/management",
       .kid = "management-key-1",
       .audience = "server-1",
       .subject = "oidc:https%3A%25%25issuer.example:subject",
       .team_id = 7,
       .capability = "remote_writes",
       .peer_issuer = "/CN=test-management-ca",
       .peer_serial = "01ab",
       .peer_fingerprint = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
       .request_sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
       .correlation_id = "correlation-1",
       .issued_at = issued_at,
       .expires_at = expires_at,
   };
   return t;
}

int main(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-management-jti-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(path) == 0);

   server_management_jti_t first = token("0123456789abcdef", 100, 190);
   assert(server_management_jti_consume(&first, 101) == SERVER_MANAGEMENT_JTI_OK);
   assert(server_management_jti_consume(&first, 101) == SERVER_MANAGEMENT_JTI_REPLAY);

   db1_shutdown();
   assert(db1_init(path) == 0);
   assert(server_management_jti_consume(&first, 102) == SERVER_MANAGEMENT_JTI_REPLAY);

   server_management_jti_t second = token("1123456789abcdef", 100, 190);
   server_management_jti_t third = token("2123456789abcdef", 100, 190);
   assert(server_management_jti_consume_for_test(&second, 102, 2) == SERVER_MANAGEMENT_JTI_OK);
   assert(server_management_jti_consume_for_test(&third, 102, 2) ==
          SERVER_MANAGEMENT_JTI_SATURATED);

   /* Strictly older rows are collected, while expires_at == now is not. */
   server_management_jti_t fresh = token("3123456789abcdef", 190, 280);
   assert(server_management_jti_consume_for_test(&fresh, 190, 1) ==
          SERVER_MANAGEMENT_JTI_SATURATED);
   assert(server_management_jti_consume_for_test(&fresh, 191, 1) == SERVER_MANAGEMENT_JTI_OK);

   server_management_jti_t bad = fresh;
   bad.peer_fingerprint = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
   assert(server_management_jti_consume(&bad, 192) == SERVER_MANAGEMENT_JTI_INVALID);
   assert(server_management_jti_consume(&fresh, 280) == SERVER_MANAGEMENT_JTI_INVALID);
   bad = fresh;
   bad.jti = "too-short";
   assert(server_management_jti_consume(&bad, 192) == SERVER_MANAGEMENT_JTI_INVALID);
   bad = fresh;
   bad.correlation_id = "control\ncharacter";
   assert(server_management_jti_consume(&bad, 192) == SERVER_MANAGEMENT_JTI_INVALID);
   bad = fresh;
   bad.capability = "not/a/token";
   assert(server_management_jti_consume(&bad, 192) == SERVER_MANAGEMENT_JTI_INVALID);

   /* P1 identity keys preserve non-ASCII UTF-8 bytes; DB1 must not narrow that canonical form. */
   server_management_jti_t unicode = token("unicode.identity1", 200, 290);
   unicode.subject = "oidc:https%3A//idp.example:jos\303\251";
   assert(server_management_jti_consume(&unicode, 201) == SERVER_MANAGEMENT_JTI_OK);

   /* A failed/ambiguous COMMIT is unavailable. Its row is rolled back and the same shared
    * connection remains usable for a later unambiguous consume. */
   int commit_calls = 0;
   sqlite3_commit_hook(db1_conn(), reject_commit, &commit_calls);
   server_management_jti_t commit_fail = token("4123456789abcdef", 200, 290);
   assert(server_management_jti_consume(&commit_fail, 201) == SERVER_MANAGEMENT_JTI_STORAGE);
   assert(commit_calls == 1);
   sqlite3_commit_hook(db1_conn(), NULL, NULL);
   assert(server_management_jti_consume(&commit_fail, 201) == SERVER_MANAGEMENT_JTI_OK);

   /* An out-of-process writer lock is unavailable/deny, never authorization success. */
   sqlite3 *other = NULL;
   assert(sqlite3_open(path, &other) == SQLITE_OK);
   assert(sqlite3_exec(other, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
   server_management_jti_t locked = token("5123456789abcdef", 200, 290);
   int64_t started = monotonic_ms();
   assert(server_management_jti_consume(&locked, 201) == SERVER_MANAGEMENT_JTI_STORAGE);
   int64_t elapsed = monotonic_ms() - started;
   assert(elapsed >= 0 && elapsed < 250);
   assert(sqlite3_exec(other, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
   sqlite3_close(other);

   /* Missing durable schema fails closed without poisoning DB1. Reinitialization reapplies the
    * schema and proves subsequent operations remain usable. */
   assert(sqlite3_exec(db1_conn(), "DROP TABLE server_management_jti", NULL, NULL, NULL) ==
          SQLITE_OK);
   server_management_jti_t missing_schema = token("6123456789abcdef", 200, 290);
   assert(server_management_jti_consume(&missing_schema, 201) == SERVER_MANAGEMENT_JTI_STORAGE);
   assert(sqlite3_exec(db1_conn(), "SELECT 1", NULL, NULL, NULL) == SQLITE_OK);
   db1_shutdown();
   assert(db1_init(path) == 0);
   assert(server_management_jti_consume(&missing_schema, 201) == SERVER_MANAGEMENT_JTI_OK);

   /* Even a table inflated outside the typed API performs at most one production-cap batch of
    * expiry GC per authorization transaction. */
   static const char inflate[] =
       "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<4097) "
       "INSERT INTO server_management_jti "
       "SELECT printf('expired%010d',x),'issuer','kid','server','owner',1,'cap','peer','01',"
       "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
       "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
       "printf('corr%010d',x),1,2,1 FROM n";
   assert(sqlite3_exec(db1_conn(), inflate, NULL, NULL, NULL) == SQLITE_OK);
   server_management_jti_t after_burst = token("7123456789abcdef", 5000, 5090);
   assert(server_management_jti_consume(&after_burst, 5001) == SERVER_MANAGEMENT_JTI_OK);
   assert(scalar("SELECT count(*) FROM server_management_jti WHERE expires_at<5001") >= 1);

   db1_shutdown();
   unlink(path);
   puts("server_management_jti: ok");
   return 0;
}
