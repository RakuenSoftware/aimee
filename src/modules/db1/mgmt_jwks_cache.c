/* db1/mgmt_jwks_cache.c: the management-JWKS cache row, and nothing else.
 *
 * Lifted out of server/server_mgmt_jwks_cache.c, which kept 780 lines of trust
 * checking and three functions of SQL in the same file. The trust checking
 * stayed where it was; this is the row.
 *
 * Digests are stored as BLOBs and travel as hex (see the header for why), so
 * this is the only place that converts between the two.
 */
#include "mgmt_jwks_cache.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static const char HEX[] = "0123456789abcdef";

/* A digest column as hex. Anything that is not exactly 32 bytes is refused
   rather than padded: a short digest that compared equal is the failure this
   whole path exists to avoid. */
static int digest_to_hex(sqlite3_stmt *st, int column, char *out)
{
   const void *raw = sqlite3_column_blob(st, column);
   if (!raw || sqlite3_column_bytes(st, column) != 32)
      return -1;
   const unsigned char *bytes = raw;
   for (int i = 0; i < 32; i++)
   {
      out[i * 2] = HEX[bytes[i] >> 4];
      out[i * 2 + 1] = HEX[bytes[i] & 0x0f];
   }
   out[64] = '\0';
   return 0;
}

static int hex_nibble(char c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   return -1;
}

/* Hex back to bytes. Returns the byte count, or -1 on any character that is not
   lowercase hex or an odd length -- an encoding that cannot round-trip is a
   caller bug, not something to bind half of. */
static int hex_to_bytes(const char *hex, unsigned char *out, size_t cap)
{
   size_t n = strlen(hex);
   if (n % 2 || n / 2 > cap)
      return -1;
   for (size_t i = 0; i < n; i += 2)
   {
      int hi = hex_nibble(hex[i]), lo = hex_nibble(hex[i + 1]);
      if (hi < 0 || lo < 0)
         return -1;
      out[i / 2] = (unsigned char)((hi << 4) | lo);
   }
   return (int)(n / 2);
}

int db1_mgmt_jwks_read(db1_mgmt_jwks_row_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   sqlite3_stmt *q = NULL;
   if (!db || sqlite3_prepare_v2(db,
                                 "SELECT generation,envelope_bytes,valid_from,valid_until,"
                                 "envelope_sha256,manifest_sha256,trust_bundle_sha256 FROM "
                                 "server_management_jwks_cache WHERE singleton=1",
                                 -1, &q, NULL) != SQLITE_OK)
      return -1;
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
   out->generation = sqlite3_column_int64(q, 0);
   const void *raw = sqlite3_column_blob(q, 1);
   int raw_n = sqlite3_column_bytes(q, 1);
   if (!raw || raw_n < 1 || raw_n >= (int)sizeof(out->envelope))
   {
      sqlite3_finalize(q);
      return -1;
   }
   memcpy(out->envelope, raw, (size_t)raw_n);
   out->envelope[raw_n] = '\0';
   out->valid_from = sqlite3_column_int64(q, 2);
   out->valid_until = sqlite3_column_int64(q, 3);
   if (digest_to_hex(q, 4, out->envelope_sha256) != 0 ||
       digest_to_hex(q, 5, out->manifest_sha256) != 0 ||
       digest_to_hex(q, 6, out->trust_bundle_sha256) != 0)
   {
      sqlite3_finalize(q);
      return -1;
   }
   sqlite3_finalize(q);
   return 1;
}

int db1_mgmt_jwks_generation(int64_t *out)
{
   if (!out)
      return -1;
   *out = 0;
   sqlite3 *db = db1_conn();
   sqlite3_stmt *q = NULL;
   if (!db || sqlite3_prepare_v2(db,
                                 "SELECT generation FROM server_management_jwks_cache "
                                 "WHERE singleton=1",
                                 -1, &q, NULL) != SQLITE_OK)
      return -1;
   int step = sqlite3_step(q);
   if (step != SQLITE_ROW)
   {
      sqlite3_finalize(q);
      return step == SQLITE_DONE ? 0 : -1;
   }
   *out = sqlite3_column_int64(q, 0);
   sqlite3_finalize(q);
   return 1;
}

int db1_mgmt_jwks_install(const db1_mgmt_jwks_install_t *in)
{
   if (!in)
      return -1;
   unsigned char env_digest[32], man_digest[32], bundle_digest[32];
   unsigned char jwks[DB1_MGMT_JWKS_BYTES_MAX];
   if (hex_to_bytes(in->envelope_sha256, env_digest, sizeof env_digest) != 32 ||
       hex_to_bytes(in->manifest_sha256, man_digest, sizeof man_digest) != 32 ||
       hex_to_bytes(in->trust_bundle_sha256, bundle_digest, sizeof bundle_digest) != 32)
      return -1;
   int jwks_len = hex_to_bytes(in->jwks, jwks, sizeof jwks);
   if (jwks_len < 0)
      return -1;

   sqlite3 *db = db1_conn();
   if (!db || db1_txn_begin_nowait(db, "BEGIN IMMEDIATE") != 0)
      return -1;
   sqlite3_stmt *q = NULL;
   int result = -1;
   if (sqlite3_prepare_v2(db,
                          "SELECT envelope_sha256,trust_bundle_sha256 FROM "
                          "server_management_jwks_cache WHERE singleton=1",
                          -1, &q, NULL) != SQLITE_OK)
      goto rollback;
   int step = sqlite3_step(q);
   if (step == SQLITE_ROW)
   {
      const void *stored_env = sqlite3_column_blob(q, 0);
      const void *stored_bundle = sqlite3_column_blob(q, 1);
      /* memcmp rather than a constant-time compare: both sides are public
         digests of a signed envelope, and the caller re-verifies the signature
         before trusting anything read back. */
      int same = stored_env && stored_bundle && sqlite3_column_bytes(q, 0) == 32 &&
                 sqlite3_column_bytes(q, 1) == 32 && !memcmp(stored_env, env_digest, 32) &&
                 !memcmp(stored_bundle, bundle_digest, 32);
      sqlite3_finalize(q);
      q = NULL;
      result = same ? 0 : 1;
      if (!same)
         goto rollback;
      return db1_txn_commit_or_rollback(db) == 0 ? 0 : -1;
   }
   sqlite3_finalize(q);
   q = NULL;
   if (step != SQLITE_DONE ||
       sqlite3_prepare_v2(db,
                          "INSERT INTO server_management_jwks_cache(singleton,generation,"
                          "valid_from,valid_until,jwks_bytes,envelope_bytes,envelope_sha256,"
                          "manifest_sha256,trust_bundle_sha256,fetched_at)"
                          " VALUES(1,1,?1,?2,?3,?4,?5,?6,?7,?8)",
                          -1, &q, NULL) != SQLITE_OK ||
       sqlite3_bind_int64(q, 1, in->valid_from) != SQLITE_OK ||
       sqlite3_bind_int64(q, 2, in->valid_until) != SQLITE_OK ||
       sqlite3_bind_blob(q, 3, jwks, jwks_len, SQLITE_TRANSIENT) != SQLITE_OK ||
       sqlite3_bind_blob(q, 4, in->envelope, (int)strlen(in->envelope), SQLITE_TRANSIENT) !=
           SQLITE_OK ||
       sqlite3_bind_blob(q, 5, env_digest, 32, SQLITE_TRANSIENT) != SQLITE_OK ||
       sqlite3_bind_blob(q, 6, man_digest, 32, SQLITE_TRANSIENT) != SQLITE_OK ||
       sqlite3_bind_blob(q, 7, bundle_digest, 32, SQLITE_TRANSIENT) != SQLITE_OK ||
       sqlite3_bind_int64(q, 8, in->fetched_at) != SQLITE_OK || sqlite3_step(q) != SQLITE_DONE)
   {
      result = -1;
      goto rollback;
   }
   sqlite3_finalize(q);
   return db1_txn_commit_or_rollback(db) == 0 ? 0 : -1;

rollback:
   sqlite3_finalize(q);
   db1_txn_end(db, "ROLLBACK");
   return result;
}
