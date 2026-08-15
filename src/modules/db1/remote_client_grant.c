/* remote_client_grant.c: DB1-owned first-user bearer -> mTLS grant binding. */
#include "remote_client_grant.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <string.h>

static int text_valid(const char *s, size_t min, size_t max)
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

static int lower_hex_exact(const char *s, size_t n)
{
   if (!s || strlen(s) != n)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int serial_valid(const char *s)
{
   if (!s)
      return 0;
   size_t n = strlen(s);
   if (n < 1 || n > DB1_REMOTE_CLIENT_CERT_SERIAL_MAX)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F') ||
            (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static void copy_grant(sqlite3_stmt *q, db1_remote_client_grant_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   db1_copy_col_text(out->principal, sizeof(out->principal), q, 0);
   db1_copy_col_text(out->bearer_sha256, sizeof(out->bearer_sha256), q, 1);
   db1_copy_col_text(out->cert_serial, sizeof(out->cert_serial), q, 2);
   const char *tier = (const char *)sqlite3_column_text(q, 3);
   out->tier = tier && strcmp(tier, "full") == 0 ? 2 : tier && strcmp(tier, "data") == 0 ? 1 : 0;
}

db1_remote_client_claim_result_t db1_remote_client_claim(const char *principal,
                                                         const char *new_bearer_sha256, int64_t now,
                                                         db1_remote_client_grant_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || !text_valid(principal, 9, DB1_REMOTE_CLIENT_PRINCIPAL_MAX) ||
       strncmp(principal, "webuser:", 8) != 0 ||
       !lower_hex_exact(new_bearer_sha256, DB1_REMOTE_CLIENT_BEARER_HASH_LEN) || now < 0)
      return DB1_REMOTE_CLIENT_CLAIM_INVALID;

   sqlite3 *db = db1_conn();
   if (!db || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return DB1_REMOTE_CLIENT_CLAIM_STORAGE;

   sqlite3_stmt *q = NULL;
   db1_remote_client_claim_result_t result = DB1_REMOTE_CLIENT_CLAIM_STORAGE;
   if (sqlite3_prepare_v2(db, "SELECT principal FROM remote_first_user WHERE singleton=1", -1, &q,
                          NULL) != SQLITE_OK)
      goto rollback;
   int step = sqlite3_step(q);
   if (step == SQLITE_ROW)
   {
      const char *owner = (const char *)sqlite3_column_text(q, 0);
      if (!owner || strcmp(owner, principal) != 0)
      {
         sqlite3_finalize(q);
         q = NULL;
         result = DB1_REMOTE_CLIENT_CLAIM_OWNED_BY_OTHER;
         goto commit;
      }
   }
   else if (step == SQLITE_DONE)
   {
      sqlite3_finalize(q);
      q = NULL;
      if (sqlite3_prepare_v2(db,
                             "INSERT INTO remote_first_user(singleton,principal,created_at) "
                             "VALUES(1,?1,?2)",
                             -1, &q, NULL) != SQLITE_OK ||
          sqlite3_bind_text(q, 1, principal, -1, SQLITE_STATIC) != SQLITE_OK ||
          sqlite3_bind_int64(q, 2, now) != SQLITE_OK || sqlite3_step(q) != SQLITE_DONE)
         goto rollback;
   }
   else
      goto rollback;
   sqlite3_finalize(q);
   q = NULL;

   /* Prefer a bound row: once paired, re-running Deploy must not mint a new
    * standing credential. Otherwise return the one pending enrollment so a
    * browser refresh can recover the same quickstart command. */
   if (sqlite3_prepare_v2(db,
                          "SELECT principal,bearer_sha256,COALESCE(cert_serial,''),tier "
                          "FROM remote_client_grants WHERE principal=?1 "
                          "ORDER BY (cert_serial IS NOT NULL) DESC, created_at ASC LIMIT 1",
                          -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_text(q, 1, principal, -1, SQLITE_STATIC) != SQLITE_OK)
      goto rollback;
   step = sqlite3_step(q);
   if (step == SQLITE_ROW)
   {
      copy_grant(q, out);
      result =
          out->cert_serial[0] ? DB1_REMOTE_CLIENT_CLAIM_BOUND : DB1_REMOTE_CLIENT_CLAIM_UNBOUND;
      goto commit;
   }
   if (step != SQLITE_DONE)
      goto rollback;
   sqlite3_finalize(q);
   q = NULL;

   if (sqlite3_prepare_v2(db,
                          "INSERT INTO remote_client_grants"
                          "(bearer_sha256,principal,tier,cert_serial,created_at,bound_at) "
                          "VALUES(?1,?2,'full',NULL,?3,NULL)",
                          -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_text(q, 1, new_bearer_sha256, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_text(q, 2, principal, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_int64(q, 3, now) != SQLITE_OK || sqlite3_step(q) != SQLITE_DONE)
      goto rollback;
   snprintf(out->principal, sizeof(out->principal), "%s", principal);
   snprintf(out->bearer_sha256, sizeof(out->bearer_sha256), "%s", new_bearer_sha256);
   out->tier = 2;
   result = DB1_REMOTE_CLIENT_CLAIM_NEW;

commit:
   sqlite3_finalize(q);
   return db1_txn_commit_or_rollback(db) == 0 ? result : DB1_REMOTE_CLIENT_CLAIM_STORAGE;
rollback:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return DB1_REMOTE_CLIENT_CLAIM_STORAGE;
}

int db1_remote_client_abandon(const char *bearer_sha256)
{
   if (!lower_hex_exact(bearer_sha256, DB1_REMOTE_CLIENT_BEARER_HASH_LEN))
      return -1;
   sqlite3 *db = db1_conn();
   sqlite3_stmt *q = NULL;
   if (!db ||
       sqlite3_prepare_v2(db,
                          "DELETE FROM remote_client_grants WHERE bearer_sha256=?1 "
                          "AND cert_serial IS NULL",
                          -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_text(q, 1, bearer_sha256, -1, SQLITE_STATIC) != SQLITE_OK)
   {
      sqlite3_finalize(q);
      return -1;
   }
   int ok = sqlite3_step(q) == SQLITE_DONE;
   sqlite3_finalize(q);
   return ok ? 0 : -1;
}

int db1_remote_client_bind(const char *bearer_sha256, const char *cert_serial, int64_t now)
{
   if (!lower_hex_exact(bearer_sha256, DB1_REMOTE_CLIENT_BEARER_HASH_LEN) ||
       !serial_valid(cert_serial) || now < 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   sqlite3_stmt *q = NULL;
   int result = -1;
   if (sqlite3_prepare_v2(db,
                          "SELECT COALESCE(cert_serial,'') FROM remote_client_grants "
                          "WHERE bearer_sha256=?1",
                          -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_text(q, 1, bearer_sha256, -1, SQLITE_STATIC) != SQLITE_OK)
      goto rollback;
   int step = sqlite3_step(q);
   if (step == SQLITE_DONE)
   {
      result = 0;
      goto commit;
   }
   if (step != SQLITE_ROW)
      goto rollback;
   const char *bound = (const char *)sqlite3_column_text(q, 0);
   if (bound && bound[0])
   {
      result = strcmp(bound, cert_serial) == 0 ? 1 : -2;
      goto commit;
   }
   sqlite3_finalize(q);
   q = NULL;
   if (sqlite3_prepare_v2(db,
                          "UPDATE remote_client_grants SET cert_serial=?1,bound_at=?2 "
                          "WHERE bearer_sha256=?3 AND cert_serial IS NULL",
                          -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_text(q, 1, cert_serial, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_bind_int64(q, 2, now) != SQLITE_OK ||
       sqlite3_bind_text(q, 3, bearer_sha256, -1, SQLITE_STATIC) != SQLITE_OK ||
       sqlite3_step(q) != SQLITE_DONE || sqlite3_changes(db) != 1)
      goto rollback;
   result = 1;

commit:
   sqlite3_finalize(q);
   return db1_txn_commit_or_rollback(db) == 0 ? result : -1;
rollback:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return -1;
}

int db1_remote_client_tier(const char *cert_serial, char *principal, size_t principal_cap)
{
   if (principal && principal_cap)
      principal[0] = '\0';
   if (!serial_valid(cert_serial) || !principal || principal_cap == 0)
      return -1;
   sqlite3 *db = db1_conn();
   sqlite3_stmt *q = NULL;
   if (!db ||
       sqlite3_prepare_v2(db,
                          "SELECT principal,tier FROM remote_client_grants "
                          "WHERE cert_serial=?1",
                          -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_text(q, 1, cert_serial, -1, SQLITE_STATIC) != SQLITE_OK)
   {
      sqlite3_finalize(q);
      return -1;
   }
   int step = sqlite3_step(q);
   if (step == SQLITE_DONE)
   {
      sqlite3_finalize(q);
      return 0;
   }
   if (step != SQLITE_ROW)
   {
      sqlite3_finalize(q);
      return -1;
   }
   const char *owner = (const char *)sqlite3_column_text(q, 0);
   const char *tier = (const char *)sqlite3_column_text(q, 1);
   if (!owner || !tier || strlen(owner) >= principal_cap)
   {
      sqlite3_finalize(q);
      return -1;
   }
   snprintf(principal, principal_cap, "%s", owner);
   int result = strcmp(tier, "full") == 0 ? 2 : strcmp(tier, "data") == 0 ? 1 : 0;
   sqlite3_finalize(q);
   return result;
}
