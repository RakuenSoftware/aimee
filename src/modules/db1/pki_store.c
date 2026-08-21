/* db1/pki_store.c: the certificate roster and the mTLS ramp row.
 *
 * Lifted out of server/pki.c, which kept certificate generation, signing and
 * this in one file. Issuing a certificate is cryptography and stayed there;
 * remembering which ones exist, which were presented, and how far the ramp has
 * come is storage and is here.
 *
 * The roster hash is computed HERE rather than by the caller, because it is
 * compared against the stored hash inside the same transaction that may advance
 * the ramp. Computed outside, a certificate added in between would let a server
 * advance on a roster that no longer exists -- which is the whole thing the
 * hash is for. That is why this module links OpenSSL for a digest.
 */
#include "pki_store.h"
#include "db1_internal.h"

#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void db_ensure_tables(void)
{
   sqlite3 *db = db1_conn();
   if (db)
      sqlite3_exec(db,
                   "CREATE TABLE IF NOT EXISTS pki_certs(serial TEXT PRIMARY KEY, cn TEXT NOT NULL,"
                   " issued_at INTEGER, expires_at INTEGER, revoked INTEGER NOT NULL DEFAULT 0)",
                   NULL, NULL, NULL);
}

static int ramp_tables_ensure(sqlite3 *db)
{
   if (!db)
      return -1;
   db_ensure_tables();
   int have_presented = 0;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "PRAGMA table_info(pki_certs)", -1, &st, NULL) != SQLITE_OK)
      return -1;
   while (sqlite3_step(st) == SQLITE_ROW)
   {
      const char *name = (const char *)sqlite3_column_text(st, 1);
      if (name && strcmp(name, "last_presented_at") == 0)
         have_presented = 1;
   }
   sqlite3_finalize(st);
   if (!have_presented &&
       sqlite3_exec(db,
                    "ALTER TABLE pki_certs ADD COLUMN last_presented_at INTEGER NOT NULL DEFAULT 0",
                    NULL, NULL, NULL) != SQLITE_OK)
      return -1;
   return sqlite3_exec(db,
                       "CREATE TABLE IF NOT EXISTS pki_mtls_ramp("
                       "id INTEGER PRIMARY KEY CHECK(id=1),"
                       "ramp_state INTEGER NOT NULL CHECK(ramp_state IN (1,2)),"
                       "roster_hash TEXT NOT NULL,"
                       "last_advance_ts INTEGER NOT NULL DEFAULT 0)",
                       NULL, NULL, NULL) == SQLITE_OK
              ? 0
              : -1;
}

static int ramp_roster_snapshot(sqlite3 *db, long now, char hash_out[65], int *count_out,
                                int *ready_out)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "SELECT serial,cn,issued_at,expires_at,last_presented_at FROM pki_certs "
                          "WHERE revoked=0 AND (expires_at=0 OR expires_at>?) ORDER BY serial,cn",
                          -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(st, 1, now);
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   if (!md || EVP_DigestInit_ex(md, EVP_sha256(), NULL) != 1)
   {
      EVP_MD_CTX_free(md);
      sqlite3_finalize(st);
      return -1;
   }
   int count = 0, ready = 1, rc;
   while ((rc = sqlite3_step(st)) == SQLITE_ROW)
   {
      const char *serial = (const char *)sqlite3_column_text(st, 0);
      const char *cn = (const char *)sqlite3_column_text(st, 1);
      long issued = (long)sqlite3_column_int64(st, 2);
      long expires = (long)sqlite3_column_int64(st, 3);
      long presented = (long)sqlite3_column_int64(st, 4);
      char times[96];
      int n = snprintf(times, sizeof(times), "%ld:%ld", issued, expires);
      unsigned char zero = 0;
      if (!serial || !cn || n < 0 || n >= (int)sizeof(times) ||
          EVP_DigestUpdate(md, serial, strlen(serial)) != 1 ||
          EVP_DigestUpdate(md, &zero, 1) != 1 || EVP_DigestUpdate(md, cn, strlen(cn)) != 1 ||
          EVP_DigestUpdate(md, &zero, 1) != 1 || EVP_DigestUpdate(md, times, (size_t)n) != 1 ||
          EVP_DigestUpdate(md, &zero, 1) != 1)
      {
         rc = SQLITE_ERROR;
         break;
      }
      if (presented <= 0 || presented < issued)
         ready = 0;
      count++;
   }
   unsigned char digest[EVP_MAX_MD_SIZE];
   unsigned int digest_len = 0;
   int ok =
       rc == SQLITE_DONE && EVP_DigestFinal_ex(md, digest, &digest_len) == 1 && digest_len == 32;
   EVP_MD_CTX_free(md);
   sqlite3_finalize(st);
   if (!ok)
      return -1;
   for (unsigned int i = 0; i < digest_len; i++)
      snprintf(hash_out + i * 2, 3, "%02x", digest[i]);
   hash_out[64] = '\0';
   if (count_out)
      *count_out = count;
   if (ready_out)
      *ready_out = count > 0 && ready;
   return 0;
}

static int ramp_refresh_hash(sqlite3 *db, long now)
{
   char hash[65];
   if (ramp_roster_snapshot(db, now, hash, NULL, NULL) != 0)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "UPDATE pki_mtls_ramp SET roster_hash=? WHERE id=1", -1, &st, NULL) !=
       SQLITE_OK)
      return -1;
   sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
   int ok = sqlite3_step(st) == SQLITE_DONE;
   sqlite3_finalize(st);
   return ok ? 0 : -1;
}

static int ramp_readiness(sqlite3 *db, long now, int advance)
{
   char current[65];
   int count = 0, ready = 0;
   if (ramp_roster_snapshot(db, now, current, &count, &ready) != 0)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT ramp_state,roster_hash FROM pki_mtls_ramp WHERE id=1", -1,
                          &st, NULL) != SQLITE_OK)
      return -1;
   int step = sqlite3_step(st), state = 0, matches = 0;
   if (step == SQLITE_ROW)
   {
      state = sqlite3_column_int(st, 0);
      const char *stored = (const char *)sqlite3_column_text(st, 1);
      matches = stored && strcmp(stored, current) == 0;
   }
   sqlite3_finalize(st);
   if (step != SQLITE_ROW || (state != 1 && state != 2))
      return -1;
   if (state == 2)
      return 0;
   if (!ready || count == 0 || !matches)
      return 0;
   if (!advance)
      return 1;
   if (sqlite3_prepare_v2(db,
                          "UPDATE pki_mtls_ramp SET ramp_state=2,last_advance_ts=? "
                          "WHERE id=1 AND ramp_state=1 AND roster_hash=?",
                          -1, &st, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int64(st, 1, now);
   sqlite3_bind_text(st, 2, current, -1, SQLITE_TRANSIENT);
   step = sqlite3_step(st);
   int changed = sqlite3_changes(db);
   sqlite3_finalize(st);
   return step == SQLITE_DONE && changed == 1 ? 1 : (step == SQLITE_DONE ? 0 : -1);
}

int db1_pki_cert_upsert(const char *serial, const char *cn, long issued_at, long expires_at)
{
   sqlite3 *db = db1_conn();
   if (ramp_tables_ensure(db) != 0 || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO pki_certs(serial,cn,issued_at,expires_at,revoked,"
                          "last_presented_at) VALUES(?,?,?,?,0,0)",
                          -1, &st, NULL) != SQLITE_OK)
      goto done;
   sqlite3_bind_text(st, 1, serial, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(st, 2, cn, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 3, issued_at);
   sqlite3_bind_int64(st, 4, expires_at);
   int step = sqlite3_step(st);
   sqlite3_finalize(st);
   st = NULL;
   if (step != SQLITE_DONE || ramp_refresh_hash(db, issued_at) != 0)
      goto done;
   return db1_txn_end(db, "COMMIT");
done:
   db1_txn_end(db, "ROLLBACK");
   return -1;
}

int db1_pki_cert_revoke(const char *serial)
{
   if (!serial || !serial[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (ramp_tables_ensure(db) != 0 || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "UPDATE pki_certs SET revoked=1 WHERE serial=?", -1, &st, NULL) !=
       SQLITE_OK)
      goto revoke_fail;
   sqlite3_bind_text(st, 1, serial, -1, SQLITE_TRANSIENT);
   int step = sqlite3_step(st);
   sqlite3_finalize(st);
   st = NULL;
   if (step != SQLITE_DONE || ramp_refresh_hash(db, (long)time(NULL)) != 0)
      goto revoke_fail;
   /* The in-memory revocation snapshot and the audit line stay with the
      caller: one is a cache this side cannot see, and the other is the
      daemon's audit tap. */
   return db1_txn_end(db, "COMMIT") == 0 ? 0 : -1;
revoke_fail:
   if (st)
      sqlite3_finalize(st);
   db1_txn_end(db, "ROLLBACK");
   return -1;
}

int db1_pki_cert_check(const char *serial, long now)
{
   if (!serial || !serial[0])
      return DB1_PKI_CERT_UNKNOWN; /* no serial to look up -> fail closed at caller */
   db_ensure_tables();
   sqlite3 *db = db1_conn();
   if (!db)
      return DB1_PKI_CERT_ERROR; /* revocation store unavailable -> fail closed */
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT revoked, expires_at FROM pki_certs WHERE serial = ?", -1, &st,
                          NULL) != SQLITE_OK)
      return DB1_PKI_CERT_ERROR;
   if (sqlite3_bind_text(st, 1, serial, -1, SQLITE_TRANSIENT) != SQLITE_OK)
   {
      sqlite3_finalize(st);
      return DB1_PKI_CERT_ERROR;
   }
   int rc = sqlite3_step(st);
   db1_pki_cert_status_t status;
   if (rc == SQLITE_ROW)
   {
      int revoked = sqlite3_column_int(st, 0);
      long expires_at = (long)sqlite3_column_int64(st, 1);
      if (revoked != 0)
         status = DB1_PKI_CERT_REVOKED;
      else if (expires_at > 0 && expires_at <= now)
         status = DB1_PKI_CERT_EXPIRED;
      else
         status = DB1_PKI_CERT_VALID;
   }
   else if (rc == SQLITE_DONE)
      status = DB1_PKI_CERT_UNKNOWN; /* no such serial on this server's roster */
   else
      status = DB1_PKI_CERT_ERROR; /* step failure (lock/IO) -> fail closed */
   sqlite3_finalize(st);
   return status;
}

int db1_pki_note_presentation(const char *serial, long now)
{
   if (!serial || !serial[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (ramp_tables_ensure(db) != 0 || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   sqlite3_stmt *st = NULL;
   int rc = -1, txn_open = 1;
   if (sqlite3_prepare_v2(
           db,
           "UPDATE pki_certs SET last_presented_at=CASE WHEN last_presented_at>? THEN "
           "last_presented_at ELSE ? END WHERE serial=? AND revoked=0 AND "
           "(expires_at=0 OR expires_at>?)",
           -1, &st, NULL) != SQLITE_OK)
      goto done_note;
   sqlite3_bind_int64(st, 1, now);
   sqlite3_bind_int64(st, 2, now);
   sqlite3_bind_text(st, 3, serial, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 4, now);
   int step = sqlite3_step(st);
   int changed = sqlite3_changes(db);
   sqlite3_finalize(st);
   if (step != SQLITE_DONE || changed != 1)
      goto done_note;
   rc = db1_txn_end(db, "COMMIT");
   txn_open = 0;
done_note:
   if (rc != 0 && txn_open)
      db1_txn_end(db, "ROLLBACK");
   return rc;
}

int db1_pki_ramp_init(int configured_mode)
{
   if (configured_mode <= 0)
      return configured_mode;
   sqlite3 *db = db1_conn();
   if (ramp_tables_ensure(db) != 0 || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   char hash[65];
   int rc = -1, persisted = configured_mode, txn_open = 1;
   if (ramp_roster_snapshot(db, (long)time(NULL), hash, NULL, NULL) != 0)
      goto done;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db,
                          "INSERT INTO pki_mtls_ramp(id,ramp_state,roster_hash,last_advance_ts) "
                          "VALUES(1,?,?,?) ON CONFLICT(id) DO NOTHING",
                          -1, &st, NULL) != SQLITE_OK)
      goto done;
   sqlite3_bind_int(st, 1, configured_mode >= 2 ? 2 : 1);
   sqlite3_bind_text(st, 2, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(st, 3, configured_mode >= 2 ? (long)time(NULL) : 0);
   int step = sqlite3_step(st);
   sqlite3_finalize(st);
   if (step != SQLITE_DONE)
      goto done;
   if (sqlite3_prepare_v2(db, "SELECT ramp_state,roster_hash FROM pki_mtls_ramp WHERE id=1", -1,
                          &st, NULL) != SQLITE_OK)
      goto done;
   step = sqlite3_step(st);
   if (step == SQLITE_ROW)
   {
      persisted = sqlite3_column_int(st, 0);
      const char *stored = (const char *)sqlite3_column_text(st, 1);
      if ((persisted != 1 && persisted != 2) || !stored || strlen(stored) != 64)
         persisted = -1;
   }
   sqlite3_finalize(st);
   if (persisted < 0)
      goto done;
   if (configured_mode >= 2 && persisted < 2)
   {
      if (sqlite3_prepare_v2(db,
                             "UPDATE pki_mtls_ramp SET ramp_state=2,roster_hash=?,"
                             "last_advance_ts=? WHERE id=1 AND ramp_state=1",
                             -1, &st, NULL) != SQLITE_OK)
         goto done;
      sqlite3_bind_text(st, 1, hash, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 2, (long)time(NULL));
      step = sqlite3_step(st);
      sqlite3_finalize(st);
      if (step != SQLITE_DONE)
         goto done;
      persisted = 2;
   }
   else if (persisted == 1 && ramp_refresh_hash(db, (long)time(NULL)) != 0)
      goto done;
   rc = db1_txn_end(db, "COMMIT");
   txn_open = 0;
done:
   if (rc != 0)
   {
      if (txn_open)
         db1_txn_end(db, "ROLLBACK");
      /* The refusal is logged by the caller, which owns the audit tap and is
         the side that will decline to start mTLS. */
      return -1;
   }
   return persisted > configured_mode ? persisted : configured_mode;
}

int db1_pki_ramp_ready(long now)
{
   sqlite3 *db = db1_conn();
   if (ramp_tables_ensure(db) != 0 || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   if (ramp_refresh_hash(db, now) != 0)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   int rc = ramp_readiness(db, now, 0);
   int end = db1_txn_end(db, rc >= 0 ? "COMMIT" : "ROLLBACK");
   return end == 0 ? rc : -1;
}

int db1_pki_ramp_advance(long now)
{
   sqlite3 *db = db1_conn();
   if (ramp_tables_ensure(db) != 0 || db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   if (ramp_refresh_hash(db, now) != 0)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1;
   }
   int rc = ramp_readiness(db, now, 1);
   int end = db1_txn_end(db, rc >= 0 ? "COMMIT" : "ROLLBACK");
   return end == 0 ? rc : -1;
}

int db1_pki_ramp_get(int *state_out, char *hash_out, size_t hash_len, long *advanced_at_out)
{
   sqlite3 *db = db1_conn();
   if (ramp_tables_ensure(db) != 0)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(
           db, "SELECT ramp_state,roster_hash,last_advance_ts FROM pki_mtls_ramp WHERE id=1", -1,
           &st, NULL) != SQLITE_OK)
      return -1;
   int rc = -1;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const char *hash = (const char *)sqlite3_column_text(st, 1);
      if (hash && strlen(hash) == 64)
      {
         if (state_out)
            *state_out = sqlite3_column_int(st, 0);
         if (hash_out && hash_len)
            snprintf(hash_out, hash_len, "%s", hash);
         if (advanced_at_out)
            *advanced_at_out = (long)sqlite3_column_int64(st, 2);
         rc = 0;
      }
   }
   sqlite3_finalize(st);
   return rc;
}

int db1_pki_cert_list(db1_pki_cert_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   db_ensure_tables();
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(
           db,
           "SELECT serial,cn,issued_at,expires_at,revoked FROM pki_certs ORDER BY issued_at DESC",
           -1, &st, NULL) != SQLITE_OK)
      return -1;
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW)
   {
      db1_pki_cert_t *row = &out[n];
      memset(row, 0, sizeof *row);
      const char *serial = (const char *)sqlite3_column_text(st, 0);
      const char *cn = (const char *)sqlite3_column_text(st, 1);
      snprintf(row->serial, sizeof row->serial, "%s", serial ? serial : "");
      snprintf(row->cn, sizeof row->cn, "%s", cn ? cn : "");
      row->issued_at = sqlite3_column_int64(st, 2);
      row->expires_at = sqlite3_column_int64(st, 3);
      row->revoked = sqlite3_column_int(st, 4);
      n++;
   }
   sqlite3_finalize(st);
   return n;
}

int db1_pki_revoked_serials(char (*out)[DB1_PKI_SERIAL_MAX], int max)
{
   if (!out || max <= 0)
      return -1;
   db_ensure_tables();
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, "SELECT serial FROM pki_certs WHERE revoked=1", -1, &st, NULL) !=
       SQLITE_OK)
      return -1;
   int n = 0;
   while (n < max && sqlite3_step(st) == SQLITE_ROW)
   {
      const char *serial = (const char *)sqlite3_column_text(st, 0);
      snprintf(out[n], DB1_PKI_SERIAL_MAX, "%s", serial ? serial : "");
      n++;
   }
   sqlite3_finalize(st);
   return n;
}
