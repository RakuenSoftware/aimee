/* db1/mgmt_nonce.c: the management challenge rows and the revocation HWM.
 *
 * Lifted out of server/server_mgmt_status.c. What stayed there is the peer
 * certificate, the signature check, and the decision that a request is valid;
 * what came here is every statement that touched the database.
 *
 * The nonce is 32 bytes and arrives as hex (see the header). This file is the
 * only place that converts, and it refuses anything that is not exactly 64
 * lowercase hex characters rather than binding a short value -- a truncated
 * nonce would look up a different challenge.
 */
#include "mgmt_nonce.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <string.h>

static int nonce_bytes(const char *hex, unsigned char out[32])
{
   if (!hex || strlen(hex) != 64)
      return -1;
   for (int i = 0; i < 32; i++)
   {
      int hi = hex[i * 2], lo = hex[i * 2 + 1];
      hi = (hi >= '0' && hi <= '9') ? hi - '0' : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
      lo = (lo >= '0' && lo <= '9') ? lo - '0' : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
      if (hi < 0 || lo < 0)
         return -1;
      out[i] = (unsigned char)((hi << 4) | lo);
   }
   return 0;
}

static int text_eq(sqlite3_stmt *s, int col, const char *expected)
{
   const unsigned char *v = sqlite3_column_text(s, col);
   return v && expected && strcmp((const char *)v, expected) == 0;
}

int db1_mgmt_nonce_clear(void)
{
   sqlite3 *db = db1_conn();
   return db && sqlite3_exec(db, "DELETE FROM server_mgmt_nonce", NULL, NULL, NULL) == SQLITE_OK
              ? 0
              : -1;
}

int db1_mgmt_nonce_issue(const db1_mgmt_nonce_issue_t *in)
{
   unsigned char nonce[32];
   if (!in || nonce_bytes(in->nonce, nonce) != 0)
      return DB1_MGMT_NONCE_INVALID;
   sqlite3 *db = db1_conn();
   if (!db)
      return DB1_MGMT_NONCE_STORAGE;
   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return DB1_MGMT_NONCE_STORAGE;

   int result = DB1_MGMT_NONCE_STORAGE;
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(db, "DELETE FROM server_mgmt_nonce WHERE expires_at<?1", -1, &q, NULL) !=
       SQLITE_OK)
      goto done;
   sqlite3_bind_int64(q, 1, (sqlite3_int64)in->now);
   if (sqlite3_step(q) != SQLITE_DONE)
      goto done;
   sqlite3_finalize(q);
   q = NULL;
   if (sqlite3_prepare_v2(db, "SELECT count(*) FROM server_mgmt_nonce", -1, &q, NULL) !=
           SQLITE_OK ||
       sqlite3_step(q) != SQLITE_ROW)
      goto done;
   int count = sqlite3_column_int(q, 0);
   sqlite3_finalize(q);
   q = NULL;
   if (count >= DB1_MGMT_NONCE_CAP)
   {
      result = DB1_MGMT_NONCE_SATURATED;
      goto commit;
   }
   static const char sql[] =
       "INSERT INTO server_mgmt_nonce(nonce,peer_issuer,peer_serial_norm,peer_fingerprint,"
       "channel_binding,target_server_id,purpose,expires_at) VALUES(?1,?2,?3,?4,?5,?6,"
       "?7,?8)";
   if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK)
      goto done;
   sqlite3_bind_blob(q, 1, nonce, 32, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 2, in->peer_issuer, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 3, in->peer_serial_norm, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 4, in->peer_fingerprint, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 5, in->channel_binding, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 6, in->target_server_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 7, in->purpose, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(q, 8, (sqlite3_int64)(in->now + DB1_MGMT_NONCE_TTL));
   if (sqlite3_step(q) != SQLITE_DONE)
      goto done;
   result = DB1_MGMT_NONCE_OK;
commit:
   sqlite3_finalize(q);
   return db1_txn_end(db, "COMMIT") == 0 ? result : DB1_MGMT_NONCE_STORAGE;
done:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return DB1_MGMT_NONCE_STORAGE;
}

int db1_mgmt_nonce_consume(const db1_mgmt_nonce_consume_t *in)
{
   unsigned char nonce[32];
   if (!in || nonce_bytes(in->nonce, nonce) != 0)
      return DB1_MGMT_NONCE_INVALID;
   sqlite3 *db = db1_conn();
   if (!db)
      return DB1_MGMT_NONCE_STORAGE;
   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return DB1_MGMT_NONCE_STORAGE;

   int result = DB1_MGMT_NONCE_STORAGE;
   sqlite3_stmt *q = NULL;
   static const char lookup[] =
       "SELECT peer_issuer,peer_serial_norm,peer_fingerprint,channel_binding,target_server_id,"
       "purpose,expires_at FROM server_mgmt_nonce WHERE nonce=?1";
   if (sqlite3_prepare_v2(db, lookup, -1, &q, NULL) != SQLITE_OK)
      goto rollback;
   sqlite3_bind_blob(q, 1, nonce, 32, SQLITE_TRANSIENT);
   if (sqlite3_step(q) != SQLITE_ROW)
   {
      result = DB1_MGMT_NONCE_NOT_FOUND;
      goto commit;
   }
   int64_t exp = sqlite3_column_int64(q, 6);
   int bound = text_eq(q, 0, in->peer_issuer) && text_eq(q, 1, in->peer_serial_norm) &&
               text_eq(q, 2, in->peer_fingerprint) && text_eq(q, 3, in->channel_binding) &&
               text_eq(q, 4, in->target_server_id) && text_eq(q, 5, in->purpose);
   sqlite3_finalize(q);
   q = NULL;
   /* Deleted whatever the verdict is: a challenge that was looked at is spent,
      or a wrong answer would be a free retry. */
   if (sqlite3_prepare_v2(db, "DELETE FROM server_mgmt_nonce WHERE nonce=?1", -1, &q, NULL) !=
       SQLITE_OK)
      goto rollback;
   sqlite3_bind_blob(q, 1, nonce, 32, SQLITE_TRANSIENT);
   if (sqlite3_step(q) != SQLITE_DONE || sqlite3_changes(db) != 1)
      goto rollback;
   sqlite3_finalize(q);
   q = NULL;
   if (!bound)
      result = DB1_MGMT_NONCE_MISMATCH;
   else if (exp < 0 || in->now > exp)
      result = DB1_MGMT_NONCE_EXPIRED;
   else if (!in->valid)
      result = DB1_MGMT_NONCE_INVALID;
   else
   {
      if (sqlite3_prepare_v2(db, "SELECT generation FROM server_mgmt_status_hwm WHERE singleton=1",
                             -1, &q, NULL) != SQLITE_OK ||
          sqlite3_step(q) != SQLITE_ROW)
         goto rollback;
      int64_t hwm = sqlite3_column_int64(q, 0);
      sqlite3_finalize(q);
      q = NULL;
      if (in->revocation_generation < hwm)
         result = DB1_MGMT_NONCE_ROLLBACK;
      else if (sqlite3_prepare_v2(db,
                                  "UPDATE server_mgmt_status_hwm SET generation=max(generation,?1) "
                                  "WHERE singleton=1",
                                  -1, &q, NULL) != SQLITE_OK)
         goto rollback;
      else
      {
         sqlite3_bind_int64(q, 1, (sqlite3_int64)in->revocation_generation);
         if (sqlite3_step(q) != SQLITE_DONE || sqlite3_changes(db) != 1)
            goto rollback;
         result = DB1_MGMT_NONCE_OK;
      }
   }
commit:
   sqlite3_finalize(q);
   return db1_txn_end(db, "COMMIT") == 0 ? result : DB1_MGMT_NONCE_STORAGE;
rollback:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return DB1_MGMT_NONCE_STORAGE;
}

int db1_mgmt_status_hwm_read(int64_t *generation)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *q = NULL;
   if (!db || !generation ||
       sqlite3_prepare_v2(db, "SELECT generation FROM server_mgmt_status_hwm WHERE singleton=1", -1,
                          &q, NULL) != SQLITE_OK)
      return -1;
   int rc = sqlite3_step(q) == SQLITE_ROW ? 0 : -1;
   if (rc == 0)
      *generation = sqlite3_column_int64(q, 0);
   sqlite3_finalize(q);
   return rc;
}

int db1_mgmt_status_hwm_set(int64_t generation)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *q = NULL;
   if (!db || generation < 0 ||
       sqlite3_prepare_v2(db,
                          "UPDATE server_mgmt_status_hwm SET generation=?1 "
                          "WHERE singleton=1 AND generation<=?1",
                          -1, &q, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(q, 1, (sqlite3_int64)generation);
   int rc = sqlite3_step(q) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
   sqlite3_finalize(q);
   return rc;
}
