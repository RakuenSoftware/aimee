#include "server_mgmt_status.h"
#include "db1_internal.h"

#include <openssl/rand.h>
#include <sqlite3.h>
#include <string.h>

#define NONCE_CAP 128
#define NONCE_TTL 15

static int text_eq(sqlite3_stmt *s, int col, const char *expected)
{
   const unsigned char *v = sqlite3_column_text(s, col);
   return v && expected && strcmp((const char *)v, expected) == 0;
}

int server_mgmt_status_init(void)
{
   sqlite3 *db = db1_conn();
   return db && sqlite3_exec(db, "DELETE FROM server_mgmt_nonce", NULL, NULL, NULL) == SQLITE_OK
              ? 0
              : -1;
}

int server_mgmt_nonce_issue(const server_tls_peer_cert_t *p, const char *target, uint64_t now,
                            unsigned char nonce[KB_MGMT_STATUS_NONCE_LEN], uint64_t *expires)
{
   sqlite3 *db = db1_conn();
   if (!db || !p || !p->issuer[0] || !p->serial_norm[0] || strlen(p->fingerprint) != 64 ||
       strlen(p->channel_binding) != 64 || !target || !target[0] || strlen(target) > 127 ||
       !nonce || !expires || now > INT64_MAX - NONCE_TTL || RAND_bytes(nonce, 32) != 1)
      return SERVER_MGMT_NONCE_INVALID;
   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return SERVER_MGMT_NONCE_STORAGE;
   int result = SERVER_MGMT_NONCE_STORAGE;
   sqlite3_stmt *q = NULL;
   if (sqlite3_prepare_v2(db, "DELETE FROM server_mgmt_nonce WHERE expires_at<?1", -1, &q, NULL) !=
       SQLITE_OK)
      goto done;
   sqlite3_bind_int64(q, 1, (sqlite3_int64)now);
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
   if (count >= NONCE_CAP)
   {
      result = SERVER_MGMT_NONCE_SATURATED;
      goto commit;
   }
   static const char sql[] =
       "INSERT INTO server_mgmt_nonce(nonce,peer_issuer,peer_serial_norm,peer_fingerprint,"
       "channel_binding,target_server_id,purpose,expires_at) VALUES(?1,?2,?3,?4,?5,?6,"
       "'management.health.v1',?7)";
   if (sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK)
      goto done;
   sqlite3_bind_blob(q, 1, nonce, 32, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 2, p->issuer, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 3, p->serial_norm, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 4, p->fingerprint, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 5, p->channel_binding, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(q, 6, target, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(q, 7, (sqlite3_int64)(now + NONCE_TTL));
   if (sqlite3_step(q) != SQLITE_DONE)
      goto done;
   *expires = now + NONCE_TTL;
   result = SERVER_MGMT_NONCE_OK;
commit:
   sqlite3_finalize(q);
   return db1_txn_end(db, "COMMIT") == 0 ? result : SERVER_MGMT_NONCE_STORAGE;
done:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return SERVER_MGMT_NONCE_STORAGE;
}

server_mgmt_nonce_result_t server_mgmt_nonce_consume(const kb_mgmt_status_t *st,
                                                     const server_tls_peer_cert_t *p,
                                                     const char *target, uint64_t now, int valid)
{
   sqlite3 *db = db1_conn();
   if (!db || !st || !p || !target || now > INT64_MAX || st->revocation_generation > INT64_MAX)
      return SERVER_MGMT_NONCE_INVALID;
   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return SERVER_MGMT_NONCE_STORAGE;
   server_mgmt_nonce_result_t result = SERVER_MGMT_NONCE_STORAGE;
   sqlite3_stmt *q = NULL;
   static const char lookup[] =
       "SELECT peer_issuer,peer_serial_norm,peer_fingerprint,channel_binding,target_server_id,"
       "purpose,expires_at FROM server_mgmt_nonce WHERE nonce=?1";
   if (sqlite3_prepare_v2(db, lookup, -1, &q, NULL) != SQLITE_OK)
      goto rollback;
   sqlite3_bind_blob(q, 1, st->nonce, 32, SQLITE_TRANSIENT);
   if (sqlite3_step(q) != SQLITE_ROW)
   {
      result = SERVER_MGMT_NONCE_NOT_FOUND;
      goto commit;
   }
   int64_t exp = sqlite3_column_int64(q, 6);
   int bound = text_eq(q, 0, p->issuer) && text_eq(q, 1, p->serial_norm) &&
               text_eq(q, 2, p->fingerprint) && text_eq(q, 3, p->channel_binding) &&
               text_eq(q, 4, target) && text_eq(q, 5, "management.health.v1");
   sqlite3_finalize(q);
   q = NULL;
   if (sqlite3_prepare_v2(db, "DELETE FROM server_mgmt_nonce WHERE nonce=?1", -1, &q, NULL) !=
       SQLITE_OK)
      goto rollback;
   sqlite3_bind_blob(q, 1, st->nonce, 32, SQLITE_TRANSIENT);
   if (sqlite3_step(q) != SQLITE_DONE || sqlite3_changes(db) != 1)
      goto rollback;
   sqlite3_finalize(q);
   q = NULL;
   if (!bound)
      result = SERVER_MGMT_NONCE_MISMATCH;
   else if (exp < 0 || now > (uint64_t)exp)
      result = SERVER_MGMT_NONCE_EXPIRED;
   else if (!valid)
      result = SERVER_MGMT_NONCE_INVALID;
   else
   {
      if (sqlite3_prepare_v2(db, "SELECT generation FROM server_mgmt_status_hwm WHERE singleton=1",
                             -1, &q, NULL) != SQLITE_OK ||
          sqlite3_step(q) != SQLITE_ROW)
         goto rollback;
      uint64_t hwm = (uint64_t)sqlite3_column_int64(q, 0);
      sqlite3_finalize(q);
      q = NULL;
      if (st->revocation_generation < hwm)
         result = SERVER_MGMT_NONCE_ROLLBACK;
      else if (sqlite3_prepare_v2(db,
                                  "UPDATE server_mgmt_status_hwm SET generation=max(generation,?1) "
                                  "WHERE singleton=1",
                                  -1, &q, NULL) != SQLITE_OK)
         goto rollback;
      else
      {
         sqlite3_bind_int64(q, 1, (sqlite3_int64)st->revocation_generation);
         if (sqlite3_step(q) != SQLITE_DONE || sqlite3_changes(db) != 1)
            goto rollback;
         result = SERVER_MGMT_NONCE_OK;
      }
   }
commit:
   sqlite3_finalize(q);
   return db1_txn_end(db, "COMMIT") == 0 ? result : SERVER_MGMT_NONCE_STORAGE;
rollback:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return SERVER_MGMT_NONCE_STORAGE;
}

int server_mgmt_status_hwm(uint64_t *generation)
{
   sqlite3 *db = db1_conn();
   sqlite3_stmt *q = NULL;
   if (!db || !generation ||
       sqlite3_prepare_v2(db, "SELECT generation FROM server_mgmt_status_hwm WHERE singleton=1", -1,
                          &q, NULL) != SQLITE_OK)
      return -1;
   int rc = sqlite3_step(q) == SQLITE_ROW ? 0 : -1;
   if (rc == 0)
      *generation = (uint64_t)sqlite3_column_int64(q, 0);
   sqlite3_finalize(q);
   return rc;
}
