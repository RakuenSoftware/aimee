#include "management_jwks_runtime.h"

#include "db2_internal.h"
#include "db_postgres.h"

#include <openssl/evp.h>
#include <string.h>

static int bounded(const char *value, size_t min, size_t max)
{
   if (!value)
      return 0;
   size_t n = strnlen(value, max + 1);
   return n >= min && n <= max;
}

static int printable(const char *value, size_t min, size_t max)
{
   if (!bounded(value, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
}

static int lower_hex(const char *value, size_t min, size_t max)
{
   if (!bounded(value, min, max))
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
         return 0;
   return 1;
}

static int copy_digest(aimee_pg_stmt_t *stmt, int column, unsigned char out[32])
{
   const void *value = aimee_pg_column_blob(stmt, column);
   int n = aimee_pg_column_bytes(stmt, column);
   if (!value || n != 32)
      return -1;
   memcpy(out, value, 32);
   return 0;
}

db2_management_jwks_runtime_result_t
db2_management_jwks_runtime_fetch(const char *issuer, const char *serial_norm,
                                  const char *fingerprint,
                                  db2_management_jwks_runtime_record_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || !printable(issuer, 1, 600) || !lower_hex(serial_norm, 1, 128) ||
       !lower_hex(fingerprint, 64, 64))
      return DB2_MANAGEMENT_JWKS_RUNTIME_INTEGRITY;
   void *connection = db2_conn();
   if (!connection)
      return DB2_MANAGEMENT_JWKS_RUNTIME_UNAVAILABLE;

   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       connection,
       "SELECT generation,candidate_id,valid_from,valid_until,envelope_bytes,envelope_sha256,"
       "manifest_sha256,jwks_sha256,payload_sha256,hwm2_attestation_digest FROM "
       "public.kb_management_jwks_runtime_fetch(?1,?2,?3)",
       error, sizeof(error));
   if (!stmt)
      return DB2_MANAGEMENT_JWKS_RUNTIME_UNAVAILABLE;
   if (aimee_pg_bind_text(stmt, "?1", issuer) != 0 ||
       aimee_pg_bind_text(stmt, "?2", serial_norm) != 0 ||
       aimee_pg_bind_text(stmt, "?3", fingerprint) != 0)
   {
      aimee_pg_finalize(stmt);
      return DB2_MANAGEMENT_JWKS_RUNTIME_UNAVAILABLE;
   }

   db2_management_jwks_runtime_result_t result = DB2_MANAGEMENT_JWKS_RUNTIME_UNAVAILABLE;
   aimee_pg_step_t first = aimee_pg_step(stmt, error, sizeof(error));
   if (first == AIMEE_PG_ERR)
   {
      const char *state = aimee_pg_sqlstate(stmt);
      if (state && strcmp(state, "28000") == 0)
         result = DB2_MANAGEMENT_JWKS_RUNTIME_DENIED;
      goto done;
   }
   if (first != AIMEE_PG_ROW)
      goto done;

   int64_t generation = aimee_pg_column_int64(stmt, 0);
   const char *candidate = aimee_pg_column_text(stmt, 1);
   int64_t valid_from = aimee_pg_column_int64(stmt, 2);
   int64_t valid_until = aimee_pg_column_int64(stmt, 3);
   const void *envelope = aimee_pg_column_blob(stmt, 4);
   int envelope_len = aimee_pg_column_bytes(stmt, 4);
   unsigned char digest[32];
   unsigned int digest_len = 0;
   if (generation != 1 || !lower_hex(candidate, 64, 64) || valid_from < 0 ||
       valid_until <= valid_from || !envelope || envelope_len < 1 ||
       envelope_len >= DB2_MANAGEMENT_JWKS_ENVELOPE_MAX ||
       memchr(envelope, '\0', (size_t)envelope_len))
   {
      result = DB2_MANAGEMENT_JWKS_RUNTIME_INTEGRITY;
      goto done;
   }
   /* db_postgres owns a single decoded-BYTEA cache per statement.  Reading the
    * next BYTEA column invalidates the preceding pointer, so retain the public
    * envelope before decoding any digest columns. */
   memcpy(out->envelope, envelope, (size_t)envelope_len);
   out->envelope[envelope_len] = '\0';
   out->envelope_len = (size_t)envelope_len;
   if (copy_digest(stmt, 5, out->envelope_sha256) || copy_digest(stmt, 6, out->manifest_sha256) ||
       copy_digest(stmt, 7, out->jwks_sha256) || copy_digest(stmt, 8, out->payload_sha256) ||
       copy_digest(stmt, 9, out->hwm2_attestation_digest) ||
       !EVP_Digest(out->envelope, out->envelope_len, digest, &digest_len, EVP_sha256(), NULL) ||
       digest_len != 32 || memcmp(digest, out->envelope_sha256, 32) != 0)
   {
      result = DB2_MANAGEMENT_JWKS_RUNTIME_INTEGRITY;
      goto done;
   }

   out->generation = generation;
   out->valid_from = valid_from;
   out->valid_until = valid_until;
   memcpy(out->candidate_id, candidate, 65);
   if (aimee_pg_step(stmt, error, sizeof(error)) != AIMEE_PG_DONE)
   {
      result = DB2_MANAGEMENT_JWKS_RUNTIME_INTEGRITY;
      goto done;
   }
   result = DB2_MANAGEMENT_JWKS_RUNTIME_OK;

done:
   aimee_pg_finalize(stmt);
   if (result != DB2_MANAGEMENT_JWKS_RUNTIME_OK)
      memset(out, 0, sizeof(*out));
   return result;
}
