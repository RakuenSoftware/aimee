#define SERVER_MANAGEMENT_JTI_TEST_API 1
#include "server_management_jti.h"
#include "db1_internal.h"

#include <limits.h>
#include <sqlite3.h>
#include <string.h>

#define STRINGIFY_INNER(V) #V
#define STRINGIFY(V)       STRINGIFY_INNER(V)

static int control_free(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if (n < min || n > max)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return 0;
   }
   return 1;
}

static int ascii_token(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if (n < min || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == '_' || s[i] == '-'))
         return 0;
   return 1;
}

static int lowercase_hex(const char *s, size_t min, size_t max)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if (n < min || n > max)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int valid_record(const server_management_jti_t *t, int64_t consumed_at)
{
   return t && ascii_token(t->jti, 16, 128) && control_free(t->issuer, 1, 255) &&
          ascii_token(t->kid, 1, 64) && ascii_token(t->audience, 1, 127) &&
          control_free(t->subject, 1, 576) && t->team_id > 0 && ascii_token(t->capability, 1, 64) &&
          control_free(t->peer_issuer, 1, 511) && lowercase_hex(t->peer_serial, 1, 79) &&
          lowercase_hex(t->peer_fingerprint, 64, 64) && lowercase_hex(t->request_sha256, 64, 64) &&
          ascii_token(t->correlation_id, 1, 128) && t->issued_at >= 0 &&
          t->issued_at < t->expires_at && consumed_at >= t->issued_at &&
          consumed_at < t->expires_at;
}

static int bind_record(sqlite3_stmt *q, const server_management_jti_t *t, int64_t consumed_at)
{
   int rc = SQLITE_OK;
#define BIND_TEXT(N, V)                                                                            \
   do                                                                                              \
   {                                                                                               \
      if (rc == SQLITE_OK)                                                                         \
         rc = sqlite3_bind_text(q, (N), (V), -1, SQLITE_TRANSIENT);                                \
   } while (0)
   BIND_TEXT(1, t->jti);
   BIND_TEXT(2, t->issuer);
   BIND_TEXT(3, t->kid);
   BIND_TEXT(4, t->audience);
   BIND_TEXT(5, t->subject);
   if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(q, 6, (sqlite3_int64)t->team_id);
   BIND_TEXT(7, t->capability);
   BIND_TEXT(8, t->peer_issuer);
   BIND_TEXT(9, t->peer_serial);
   BIND_TEXT(10, t->peer_fingerprint);
   BIND_TEXT(11, t->request_sha256);
   BIND_TEXT(12, t->correlation_id);
   if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(q, 13, (sqlite3_int64)t->issued_at);
   if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(q, 14, (sqlite3_int64)t->expires_at);
   if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(q, 15, (sqlite3_int64)consumed_at);
#undef BIND_TEXT
   return rc;
}

static server_management_jti_result_t consume(const server_management_jti_t *t, int64_t consumed_at,
                                              size_t live_limit)
{
   if (!valid_record(t, consumed_at) || live_limit == 0 || live_limit > INT_MAX)
      return SERVER_MANAGEMENT_JTI_INVALID;

   sqlite3 *db = db1_conn();
   if (!db || db1_txn_begin_nowait(db, "BEGIN IMMEDIATE") != 0)
      return SERVER_MANAGEMENT_JTI_STORAGE;

   server_management_jti_result_t result = SERVER_MANAGEMENT_JTI_STORAGE;
   sqlite3_stmt *q = NULL;
   static const char gc_sql[] =
       "DELETE FROM server_management_jti WHERE jti IN "
       "(SELECT jti FROM server_management_jti WHERE expires_at<?1 "
       "ORDER BY expires_at,jti LIMIT " STRINGIFY(SERVER_MANAGEMENT_JTI_LIVE_LIMIT) ")";
   if (sqlite3_prepare_v2(db, gc_sql, -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_int64(q, 1, consumed_at) != SQLITE_OK || sqlite3_step(q) != SQLITE_DONE)
      goto rollback;
   sqlite3_finalize(q);
   q = NULL;

   if (sqlite3_prepare_v2(db, "SELECT count(*) FROM server_management_jti", -1, &q, NULL) !=
           SQLITE_OK ||
       sqlite3_step(q) != SQLITE_ROW)
      goto rollback;
   sqlite3_int64 count = sqlite3_column_int64(q, 0);
   sqlite3_finalize(q);
   q = NULL;
   if (count < 0)
      goto rollback;
   if ((uint64_t)count >= (uint64_t)live_limit)
   {
      result = SERVER_MANAGEMENT_JTI_SATURATED;
      goto commit;
   }

   static const char insert_sql[] =
       "INSERT INTO server_management_jti(jti,issuer,kid,audience,subject,team_id,capability,"
       "peer_issuer,peer_serial,peer_fingerprint,request_sha256,correlation_id,issued_at,"
       "expires_at,consumed_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)";
   if (sqlite3_prepare_v2(db, insert_sql, -1, &q, NULL) != SQLITE_OK ||
       bind_record(q, t, consumed_at) != SQLITE_OK)
      goto rollback;
   int step = sqlite3_step(q);
   if (step != SQLITE_DONE)
   {
      int xrc = sqlite3_extended_errcode(db);
      if (xrc == SQLITE_CONSTRAINT_PRIMARYKEY || xrc == SQLITE_CONSTRAINT_UNIQUE)
         result = SERVER_MANAGEMENT_JTI_REPLAY;
      goto rollback_result;
   }
   result = SERVER_MANAGEMENT_JTI_OK;

commit:
   sqlite3_finalize(q);
   return db1_txn_commit_or_rollback(db) == 0 ? result : SERVER_MANAGEMENT_JTI_STORAGE;

rollback_result:
   sqlite3_finalize(q);
   q = NULL;
   db1_txn_end(db, "ROLLBACK");
   return result;

rollback:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return SERVER_MANAGEMENT_JTI_STORAGE;
}

server_management_jti_result_t db1_management_jti_consume(const server_management_jti_t *token,
                                                          int64_t consumed_at)
{
   return consume(token, consumed_at, SERVER_MANAGEMENT_JTI_LIVE_LIMIT);
}

server_management_jti_result_t
server_management_jti_consume_for_test(const server_management_jti_t *token, int64_t consumed_at,
                                       size_t live_limit)
{
   return consume(token, consumed_at, live_limit);
}

server_management_jti_result_t
db1_management_jti_consume_row(const db1_management_jti_consume_t *in)
{
   if (!in)
      return SERVER_MANAGEMENT_JTI_INVALID;
   return db1_management_jti_consume(&in->token, in->consumed_at);
}
