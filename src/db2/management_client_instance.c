#include "management_client_instance.h"

#include <openssl/evp.h>

#include <string.h>

static int binding_text(const char *value, size_t *length)
{
   if (!value || !length)
      return 0;
   size_t n = strnlen(value, DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX + 1);
   if (n == 0 || n > DB2_MANAGEMENT_CLIENT_INSTANCE_TEXT_MAX)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)value[i] < 0x21 || (unsigned char)value[i] > 0x7e)
         return 0;
   *length = n;
   return 1;
}

static int digest_u32(EVP_MD_CTX *ctx, uint32_t value)
{
   const unsigned char encoded[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16),
                                     (unsigned char)(value >> 8), (unsigned char)value};
   return EVP_DigestUpdate(ctx, encoded, sizeof(encoded)) == 1;
}

db2_management_client_instance_result_t
db2_management_client_instance_classify_sqlstate(const char *sqlstate)
{
   if (!sqlstate || strlen(sqlstate) != 5)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   if (strcmp(sqlstate, "22023") == 0)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   if (strcmp(sqlstate, "28000") == 0 || strcmp(sqlstate, "42501") == 0)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_DENIED;
   if (strcmp(sqlstate, "23505") == 0)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_CONFLICT;
   if (strcmp(sqlstate, "40001") == 0 || strcmp(sqlstate, "40P01") == 0)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_RETRY;
   if (strcmp(sqlstate, "55000") == 0)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
}

db2_management_client_instance_result_t db2_management_client_instance_binding_digest(
    const char *issuer, const char *subject,
    const uint8_t proof_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
    const uint8_t custody_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
    uint8_t out[DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN])
{
   static const unsigned char domain[] = "aimee.p5.management-instance.binding.v1";
   if (!out)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   memset(out, 0, DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN);

   size_t issuer_len = 0, subject_len = 0;
   if (!binding_text(issuer, &issuer_len) || !binding_text(subject, &subject_len) ||
       !proof_anchor || !custody_anchor)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;

   EVP_MD_CTX *ctx = EVP_MD_CTX_new();
   unsigned int digest_len = 0;
   int ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(ctx, domain, sizeof(domain) - 1) == 1 &&
            digest_u32(ctx, (uint32_t)issuer_len) &&
            EVP_DigestUpdate(ctx, issuer, issuer_len) == 1 &&
            digest_u32(ctx, (uint32_t)subject_len) &&
            EVP_DigestUpdate(ctx, subject, subject_len) == 1 && digest_u32(ctx, 32) &&
            EVP_DigestUpdate(ctx, proof_anchor, DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN) == 1 &&
            digest_u32(ctx, 32) &&
            EVP_DigestUpdate(ctx, custody_anchor, DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN) == 1 &&
            EVP_DigestFinal_ex(ctx, out, &digest_len) == 1 &&
            digest_len == DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN;
   EVP_MD_CTX_free(ctx);
   if (!ok)
   {
      memset(out, 0, DB2_MANAGEMENT_CLIENT_INSTANCE_DIGEST_LEN);
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   }
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

db2_management_client_instance_result_t db2_management_client_instance_binding_init(
    const char *issuer, const char *subject,
    const uint8_t proof_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
    const uint8_t custody_anchor[DB2_MANAGEMENT_CLIENT_INSTANCE_ANCHOR_LEN],
    db2_management_client_instance_binding_t *out)
{
   if (!out)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   memset(out, 0, sizeof(*out));

   size_t issuer_len = 0, subject_len = 0;
   if (!binding_text(issuer, &issuer_len) || !binding_text(subject, &subject_len) ||
       !proof_anchor || !custody_anchor)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;

   db2_management_client_instance_binding_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   memcpy(candidate.issuer, issuer, issuer_len);
   memcpy(candidate.subject, subject, subject_len);
   memcpy(candidate.proof_anchor, proof_anchor, sizeof(candidate.proof_anchor));
   memcpy(candidate.custody_anchor, custody_anchor, sizeof(candidate.custody_anchor));
   db2_management_client_instance_result_t rc = db2_management_client_instance_binding_digest(
       candidate.issuer, candidate.subject, candidate.proof_anchor, candidate.custody_anchor,
       candidate.binding_digest);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
   {
      memset(&candidate, 0, sizeof(candidate));
      return rc;
   }
   *out = candidate;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}
