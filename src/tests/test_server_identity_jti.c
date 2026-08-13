/* test_server_identity_jti.c — the identity-token replay store (proposal §9).
 *
 * The property under test is narrow and absolute: a jti can be consumed at most
 * once, and that must survive a process restart, because a replay window that
 * opens on restart is a replay window. */
#define SERVER_IDENTITY_JTI_TEST_API 1
#include "db1.h"
#include "db1_internal.h"
#include "server_identity_jti.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static server_identity_jti_t token(const char *jti, int64_t issued, int64_t expires,
                                   const char *tier)
{
   server_identity_jti_t t;
   memset(&t, 0, sizeof(t));
   t.jti = jti;
   t.issuer = "kb";
   t.kid = "kid-a";
   t.audience = "server-1";
   t.subject = "oidc:idp.test:user-1";
   t.team_id = 7;
   t.tier = tier;
   t.issued_at = issued;
   t.expires_at = expires;
   return t;
}

static int64_t scalar(const char *sql)
{
   sqlite3_stmt *stmt = NULL;
   assert(sqlite3_prepare_v2(db1_conn(), sql, -1, &stmt, NULL) == SQLITE_OK);
   assert(sqlite3_step(stmt) == SQLITE_ROW);
   int64_t v = sqlite3_column_int64(stmt, 0);
   sqlite3_finalize(stmt);
   return v;
}

int main(void)
{
   char path[256];
   snprintf(path, sizeof path, "%s/aimee-identity-jti-XXXXXX", platform_tmpdir());
   int fd = mkstemp(path);
   assert(fd >= 0);
   close(fd);
   assert(db1_init(path) == 0);

   /* Consume once, then never again. */
   server_identity_jti_t first = token("id-jti-00000001", 100, 190, "data");
   assert(server_identity_jti_consume(&first, 101) == SERVER_IDENTITY_JTI_OK);
   assert(server_identity_jti_consume(&first, 101) == SERVER_IDENTITY_JTI_REPLAY);

   /* The record is durable: a restart must not reopen the window. */
   db1_shutdown();
   assert(db1_init(path) == 0);
   assert(server_identity_jti_consume(&first, 102) == SERVER_IDENTITY_JTI_REPLAY);

   /* Saturation denies rather than evicting a live entry. */
   server_identity_jti_t second = token("id-jti-00000002", 100, 190, "full");
   server_identity_jti_t third = token("id-jti-00000003", 100, 190, "off");
   assert(server_identity_jti_consume_for_test(&second, 102, 2) == SERVER_IDENTITY_JTI_OK);
   assert(server_identity_jti_consume_for_test(&third, 102, 2) == SERVER_IDENTITY_JTI_SATURATED);
   /* ...and the saturated token was NOT recorded, so it can be retried once the
    * store drains rather than being permanently poisoned. */
   assert(scalar("SELECT count(*) FROM server_identity_jti WHERE jti='id-jti-00000003'") == 0);

   /* An 8-character jti is accepted: the management store's floor is 16, but the
    * server's identity verifier accepts 8, and a store that rejected what the
    * verifier accepts would deny valid tokens. */
   server_identity_jti_t shortest = token("id-00001", 100, 190, "data");
   assert(server_identity_jti_consume(&shortest, 101) == SERVER_IDENTITY_JTI_OK);

   /* Malformed records are refused before touching the store. */
   server_identity_jti_t bad = token("id-jti-00000004", 100, 190, "superuser");
   assert(server_identity_jti_consume(&bad, 101) == SERVER_IDENTITY_JTI_INVALID);
   bad = token("short", 100, 190, "data"); /* below the 8-character floor */
   assert(server_identity_jti_consume(&bad, 101) == SERVER_IDENTITY_JTI_INVALID);
   bad = token("id-jti-00000005", 100, 190, "data");
   bad.team_id = 0;
   assert(server_identity_jti_consume(&bad, 101) == SERVER_IDENTITY_JTI_INVALID);
   bad = token("id-jti-00000006", 100, 190, "data");
   assert(server_identity_jti_consume(&bad, 191) == SERVER_IDENTITY_JTI_INVALID); /* past exp */
   assert(server_identity_jti_consume(&bad, 99) == SERVER_IDENTITY_JTI_INVALID);  /* before iat */
   assert(scalar("SELECT count(*) FROM server_identity_jti WHERE jti LIKE 'id-jti-00000%'") == 2);

   /* Expired rows are collected; a row expiring exactly now is not yet stale. */
   server_identity_jti_t old = token("id-jti-00000007", 100, 150, "data");
   assert(server_identity_jti_consume(&old, 101) == SERVER_IDENTITY_JTI_OK);
   server_identity_jti_t later = token("id-jti-00000008", 200, 400, "data");
   assert(server_identity_jti_consume(&later, 201) == SERVER_IDENTITY_JTI_OK);
   assert(scalar("SELECT count(*) FROM server_identity_jti WHERE jti='id-jti-00000007'") == 0);

   /* The management store is a genuinely separate table — consuming here must
    * not affect it, or the two token types could evict each other. */
   assert(scalar("SELECT count(*) FROM server_management_jti") == 0);

   db1_shutdown();
   unlink(path);
   printf("  PASS: server_identity_jti consumes once, survives restart, denies when saturated\n");
   return 0;
}
