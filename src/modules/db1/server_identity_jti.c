/* server_identity_jti.c — replay store for the data-plane identity token.
 * See server_identity_jti.h and the table comment in db1/schema.sql. Mirrors
 * server_management_jti.c: same transaction discipline, same bounded-growth
 * pruning, same "durable before OK" guarantee. */

#include "server_identity_jti.h"
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
   for (size_t i = 0; i < n; i++)
   {
      char c = s[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-'))
         return 0;
   }
   return 1;
}

static int valid_tier(const char *s)
{
   return s && (!strcmp(s, "off") || !strcmp(s, "data") || !strcmp(s, "full"));
}

static int valid_record(const server_identity_jti_t *t, int64_t consumed_at)
{
   /* The jti floor is 8, matching the server verifier's accepted range, and the
    * tier must be one of the three defined levels — an unrecognized tier is a
    * corrupt record, not a token to consume. */
   return t && ascii_token(t->jti, 8, 128) && control_free(t->issuer, 1, 255) &&
          ascii_token(t->kid, 1, 64) && ascii_token(t->audience, 1, 127) &&
          control_free(t->subject, 1, 576) && t->team_id > 0 && valid_tier(t->tier) &&
          t->issued_at >= 0 && t->issued_at < t->expires_at && consumed_at >= t->issued_at &&
          consumed_at < t->expires_at;
}

static int bind_record(sqlite3_stmt *q, const server_identity_jti_t *t, int64_t consumed_at)
{
   int rc = SQLITE_OK;
#define BIND_TEXT(N, V)                                                                            \
   if (rc == SQLITE_OK)                                                                            \
   rc = sqlite3_bind_text(q, (N), (V), -1, SQLITE_STATIC)
#define BIND_I64(N, V)                                                                             \
   if (rc == SQLITE_OK)                                                                            \
   rc = sqlite3_bind_int64(q, (N), (V))
   BIND_TEXT(1, t->jti);
   BIND_TEXT(2, t->issuer);
   BIND_TEXT(3, t->kid);
   BIND_TEXT(4, t->audience);
   BIND_TEXT(5, t->subject);
   BIND_I64(6, t->team_id);
   BIND_TEXT(7, t->tier);
   BIND_I64(8, t->issued_at);
   BIND_I64(9, t->expires_at);
   BIND_I64(10, consumed_at);
#undef BIND_TEXT
#undef BIND_I64
   return rc;
}

static server_identity_jti_result_t consume(const server_identity_jti_t *t, int64_t consumed_at,
                                            size_t live_limit)
{
   if (!valid_record(t, consumed_at) || live_limit == 0 || live_limit > INT_MAX)
      return SERVER_IDENTITY_JTI_INVALID;

   sqlite3 *db = db1_conn();
   if (!db || db1_txn_begin_nowait(db, "BEGIN IMMEDIATE") != 0)
      return SERVER_IDENTITY_JTI_STORAGE;

   server_identity_jti_result_t result = SERVER_IDENTITY_JTI_STORAGE;
   sqlite3_stmt *q = NULL;
   static const char gc_sql[] =
       "DELETE FROM server_identity_jti WHERE jti IN "
       "(SELECT jti FROM server_identity_jti WHERE expires_at<?1 "
       "ORDER BY expires_at,jti LIMIT " STRINGIFY(SERVER_IDENTITY_JTI_LIVE_LIMIT) ")";
   if (sqlite3_prepare_v2(db, gc_sql, -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_int64(q, 1, consumed_at) != SQLITE_OK || sqlite3_step(q) != SQLITE_DONE)
      goto rollback;
   sqlite3_finalize(q);
   q = NULL;

   if (sqlite3_prepare_v2(db, "SELECT count(*) FROM server_identity_jti", -1, &q, NULL) !=
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
      /* Saturation denies rather than evicting a live entry: dropping an
       * unexpired jti to make room would hand back a replay window. */
      result = SERVER_IDENTITY_JTI_SATURATED;
      goto commit;
   }

   static const char insert_sql[] =
       "INSERT INTO server_identity_jti(jti,issuer,kid,audience,subject,team_id,tier,"
       "issued_at,expires_at,consumed_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)";
   if (sqlite3_prepare_v2(db, insert_sql, -1, &q, NULL) != SQLITE_OK ||
       bind_record(q, t, consumed_at) != SQLITE_OK)
      goto rollback;
   int step = sqlite3_step(q);
   if (step != SQLITE_DONE)
   {
      int xrc = sqlite3_extended_errcode(db);
      if (xrc == SQLITE_CONSTRAINT_PRIMARYKEY || xrc == SQLITE_CONSTRAINT_UNIQUE)
         result = SERVER_IDENTITY_JTI_REPLAY;
      goto rollback_result;
   }
   result = SERVER_IDENTITY_JTI_OK;

commit:
   sqlite3_finalize(q);
   return db1_txn_commit_or_rollback(db) == 0 ? result : SERVER_IDENTITY_JTI_STORAGE;

rollback_result:
   sqlite3_finalize(q);
   q = NULL;
   db1_txn_end(db, "ROLLBACK");
   return result;

rollback:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return SERVER_IDENTITY_JTI_STORAGE;
}

server_identity_jti_result_t server_identity_jti_consume(const server_identity_jti_t *token,
                                                         int64_t consumed_at)
{
   return consume(token, consumed_at, SERVER_IDENTITY_JTI_LIVE_LIMIT);
}

/* Defined unconditionally, like the management store's equivalent: only the
 * HEADER declaration is guarded, which keeps it out of the production API
 * surface without making the object depend on a test-only define. */
server_identity_jti_result_t
server_identity_jti_consume_for_test(const server_identity_jti_t *token, int64_t consumed_at,
                                     size_t live_limit)
{
   return consume(token, consumed_at, live_limit);
}
