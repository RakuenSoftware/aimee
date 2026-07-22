#include "management_client_instance.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
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

static int exact_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static void hex_encode(const uint8_t in[32], char out[65])
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
   {
      out[2 * i] = digits[in[i] >> 4];
      out[2 * i + 1] = digits[in[i] & 15];
   }
   out[64] = 0;
}

static int hex_decode(const char *s, uint8_t out[32])
{
   if (!exact_hex(s, 64))
      return -1;
   for (size_t i = 0; i < 32; ++i)
   {
      unsigned a = (unsigned)(s[2 * i] <= '9' ? s[2 * i] - '0' : s[2 * i] - 'a' + 10);
      unsigned b = (unsigned)(s[2 * i + 1] <= '9' ? s[2 * i + 1] - '0' : s[2 * i + 1] - 'a' + 10);
      out[i] = (uint8_t)((a << 4) | b);
   }
   return 0;
}

static int strict_text(const char *s, size_t cap)
{
   size_t n = s ? strnlen(s, cap) : 0;
   if (n == 0 || n >= cap)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e)
         return 0;
   return 1;
}

static int copy_text_col(aimee_pg_stmt_t *st, int col, char *out, size_t cap)
{
   const char *s = aimee_pg_column_text(st, col);
   if (!strict_text(s, cap))
      return -1;
   memcpy(out, s, strlen(s) + 1);
   return 0;
}

static int col_i64(aimee_pg_stmt_t *st, int col, int64_t *out)
{
   const char *s = aimee_pg_column_text(st, col);
   char *end = NULL;
   if (!s || !*s || aimee_pg_column_is_null(st, col))
      return -1;
   errno = 0;
   long long v = strtoll(s, &end, 10);
   if (errno || !end || *end)
      return -1;
   *out = (int64_t)v;
   return 0;
}

static int col_bool(aimee_pg_stmt_t *st, int col, int *out)
{
   const char *s = aimee_pg_column_text(st, col);
   if (s && (!strcmp(s, "t") || !strcmp(s, "true") || !strcmp(s, "1")))
      *out = 1;
   else if (s && (!strcmp(s, "f") || !strcmp(s, "false") || !strcmp(s, "0")))
      *out = 0;
   else
      return -1;
   return 0;
}

static int col_hex(aimee_pg_stmt_t *st, int col, uint8_t out[32])
{
   return hex_decode(aimee_pg_column_text(st, col), out);
}

static int parse_kind(const char *s, db2_management_client_issue_kind_t *out)
{
   if (s && !strcmp(s, "initial"))
      *out = DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL;
   else if (s && !strcmp(s, "renew"))
      *out = DB2_MANAGEMENT_CLIENT_ISSUE_RENEW;
   else
      return -1;
   return 0;
}

static int parse_state(const char *s, db2_management_client_issue_state_t *out)
{
   if (s && !strcmp(s, "pending"))
      *out = DB2_MANAGEMENT_CLIENT_ISSUE_PENDING;
   else if (s && !strcmp(s, "active"))
      *out = DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE;
   else if (s && !strcmp(s, "expired"))
      *out = DB2_MANAGEMENT_CLIENT_ISSUE_EXPIRED;
   else if (s && !strcmp(s, "quarantined"))
      *out = DB2_MANAGEMENT_CLIENT_ISSUE_QUARANTINED;
   else
      return -1;
   return 0;
}

static int binding_valid(const db2_management_client_instance_binding_t *b)
{
   uint8_t digest[32];
   if (!b || db2_management_client_instance_binding_digest(b->issuer, b->subject, b->proof_anchor,
                                                           b->custody_anchor, digest) !=
                 DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      return 0;
   int ok = CRYPTO_memcmp(digest, b->binding_digest, 32) == 0;
   OPENSSL_cleanse(digest, sizeof(digest));
   return ok;
}

static db2_management_client_instance_result_t runtime_stmt(const char *sql, aimee_pg_stmt_t **out)
{
   *out = NULL;
   if (db2_tenant_require_pg() != 0)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   if (!conn || !(*out = aimee_pg_prepare_ex(conn, sql, &kind, err, sizeof(err))))
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   return DB2_MANAGEMENT_CLIENT_INSTANCE_OK;
}

static db2_management_client_instance_result_t finish_row(aimee_pg_stmt_t *st, int decode_ok)
{
   char err[256] = "";
   db2_management_client_instance_result_t rc =
       decode_ok ? DB2_MANAGEMENT_CLIENT_INSTANCE_OK : DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY;
   if (decode_ok)
   {
      aimee_pg_step_t next = aimee_pg_step(st, err, sizeof(err));
      if (next == AIMEE_PG_ERR)
         rc = db2_management_client_instance_classify_sqlstate(aimee_pg_sqlstate(st));
      else if (next != AIMEE_PG_DONE)
         rc = DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY;
   }
   aimee_pg_finalize(st);
   return rc;
}

static db2_management_client_instance_result_t first_row(aimee_pg_stmt_t *st)
{
   char err[256] = "";
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ERR)
      return db2_management_client_instance_classify_sqlstate(aimee_pg_sqlstate(st));
   return step == AIMEE_PG_ROW ? DB2_MANAGEMENT_CLIENT_INSTANCE_OK
                               : DB2_MANAGEMENT_CLIENT_INSTANCE_INTEGRITY;
}

static int decode_pending(aimee_pg_stmt_t *st, db2_management_client_pending_t *o)
{
   int64_t previous = 0;
   if (aimee_pg_column_count(st) != 18 || col_bool(st, 0, &o->replayed) ||
       copy_text_col(st, 1, o->installation_id, sizeof(o->installation_id)) ||
       !exact_hex(o->installation_id, 32) ||
       copy_text_col(st, 2, o->replacement_lineage_id, sizeof(o->replacement_lineage_id)) ||
       !exact_hex(o->replacement_lineage_id, 32) ||
       copy_text_col(st, 3, o->authority_id, sizeof(o->authority_id)) ||
       !exact_hex(o->authority_id, 32) || col_i64(st, 4, &o->team_id) || o->team_id < 1 ||
       col_hex(st, 5, o->binding_digest) ||
       strcmp(aimee_pg_column_text(st, 6) ? aimee_pg_column_text(st, 6) : "", "active") ||
       col_i64(st, 7, &o->generation) || o->generation < 1 ||
       copy_text_col(st, 8, o->operation_id, sizeof(o->operation_id)) ||
       !exact_hex(o->operation_id, 64) || parse_kind(aimee_pg_column_text(st, 9), &o->issue_kind) ||
       parse_state(aimee_pg_column_text(st, 10), &o->issue_state) ||
       col_hex(st, 15, o->csr_digest) || col_hex(st, 16, o->csr_spki_digest) ||
       col_i64(st, 17, &o->pending_expires_at_epoch) || o->pending_expires_at_epoch < 1)
      return -1;
   if (!aimee_pg_column_is_null(st, 11))
   {
      if (col_i64(st, 11, &previous) || previous < 1 ||
          copy_text_col(st, 12, o->previous_cert_issuer, sizeof(o->previous_cert_issuer)) ||
          copy_text_col(st, 13, o->previous_cert_serial_norm,
                        sizeof(o->previous_cert_serial_norm)) ||
          !exact_hex(o->previous_cert_serial_norm, strlen(o->previous_cert_serial_norm)) ||
          col_hex(st, 14, o->previous_cert_fingerprint))
         return -1;
      o->has_previous = 1;
      o->previous_enrollment_id = previous;
   }
   else if (!aimee_pg_column_is_null(st, 12) || !aimee_pg_column_is_null(st, 13) ||
            !aimee_pg_column_is_null(st, 14))
      return -1;
   return (o->issue_kind == DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL) == !o->has_previous ? 0 : -1;
}

static int bind_binding(aimee_pg_stmt_t *st, const db2_management_client_instance_binding_t *b,
                        int first)
{
   char proof[65], custody[65], digest[65], name[16];
   hex_encode(b->proof_anchor, proof);
   hex_encode(b->custody_anchor, custody);
   hex_encode(b->binding_digest, digest);
#define BIND_TEXT_AT(offset, value)                                                                \
   (snprintf(name, sizeof(name), "?%d", first + (offset)), aimee_pg_bind_text(st, name, (value)))
   int rc = BIND_TEXT_AT(0, b->issuer) || BIND_TEXT_AT(1, b->subject) || BIND_TEXT_AT(2, proof) ||
            BIND_TEXT_AT(3, custody) || BIND_TEXT_AT(4, digest);
#undef BIND_TEXT_AT
   return rc ? -1 : 0;
}

static db2_management_client_instance_result_t pending_call(aimee_pg_stmt_t *st,
                                                            db2_management_client_pending_t *out)
{
   db2_management_client_instance_result_t rc = first_row(st);
   db2_management_client_pending_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      rc = finish_row(st, decode_pending(st, &candidate) == 0);
   else
      aimee_pg_finalize(st);
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      *out = candidate;
   return rc;
}

db2_management_client_instance_result_t db2_management_client_instance_grant_preflight(
    const db2_management_client_grant_preflight_request_t *r,
    db2_management_client_grant_preflight_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!r || !out || !exact_hex(r->installation_id, 32) || !binding_valid(&r->binding))
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   aimee_pg_stmt_t *st = NULL;
   db2_management_client_instance_result_t rc = runtime_stmt(
       "SELECT * FROM public.kb_management_instance_grant_preflight(?1,?2,?3,?4,?5,?6)", &st);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      return rc;
   if (aimee_pg_bind_text(st, "?1", r->installation_id) || bind_binding(st, &r->binding, 2))
   {
      aimee_pg_finalize(st);
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   }
   rc = first_row(st);
   db2_management_client_grant_preflight_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   int decoded =
       rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK && aimee_pg_column_count(st) == 3 &&
       copy_text_col(st, 0, candidate.installation_id, sizeof(candidate.installation_id)) == 0 &&
       exact_hex(candidate.installation_id, 32) &&
       copy_text_col(st, 1, candidate.replacement_lineage_id,
                     sizeof(candidate.replacement_lineage_id)) == 0 &&
       exact_hex(candidate.replacement_lineage_id, 32) &&
       col_i64(st, 2, &candidate.expires_at_epoch) == 0 && candidate.expires_at_epoch > 0 &&
       strcmp(candidate.installation_id, r->installation_id) == 0;
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      rc = finish_row(st, decoded);
   else
      aimee_pg_finalize(st);
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      *out = candidate;
   else
      memset(out, 0, sizeof(*out));
   return rc;
}

db2_management_client_instance_result_t
db2_management_client_instance_begin_initial(const db2_management_client_initial_request_t *r,
                                             db2_management_client_pending_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!r || !out || !exact_hex(r->operation_id, 64) || !exact_hex(r->authority_id, 32) ||
       !exact_hex(r->installation_id, 32) || !exact_hex(r->expected_lineage_id, 32) ||
       !binding_valid(&r->binding))
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   aimee_pg_stmt_t *st = NULL;
   db2_management_client_instance_result_t rc = runtime_stmt(
       "SELECT * FROM "
       "public.kb_management_instance_begin_initial(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
       &st);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      return rc;
   char csr[65], spki[65];
   hex_encode(r->csr_digest, csr);
   hex_encode(r->csr_spki_digest, spki);
   if (aimee_pg_bind_text(st, "?1", r->operation_id) ||
       aimee_pg_bind_text(st, "?2", r->authority_id) ||
       aimee_pg_bind_text(st, "?3", r->installation_id) ||
       aimee_pg_bind_text(st, "?4", r->expected_lineage_id) || bind_binding(st, &r->binding, 5) ||
       aimee_pg_bind_text(st, "?10", csr) || aimee_pg_bind_text(st, "?11", spki))
   {
      aimee_pg_finalize(st);
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   }
   rc = pending_call(st, out);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      memset(out, 0, sizeof(*out));
   return rc;
}

db2_management_client_instance_result_t
db2_management_client_instance_begin_renewal(const db2_management_client_renewal_request_t *r,
                                             db2_management_client_pending_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!r || !out || !exact_hex(r->operation_id, 64) || !exact_hex(r->installation_id, 32) ||
       !binding_valid(&r->binding) || r->generation < 2 || r->previous_enrollment_id < 1 ||
       !strict_text(r->previous_cert_issuer, sizeof(r->previous_cert_issuer)) ||
       !strict_text(r->previous_cert_serial_norm, sizeof(r->previous_cert_serial_norm)) ||
       !exact_hex(r->previous_cert_serial_norm, strlen(r->previous_cert_serial_norm)))
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   aimee_pg_stmt_t *st = NULL;
   db2_management_client_instance_result_t rc =
       runtime_stmt("SELECT * FROM "
                    "public.kb_management_instance_begin_renewal(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?"
                    "11,?12,?13,?14)",
                    &st);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      return rc;
   char prev[65], csr[65], spki[65];
   hex_encode(r->previous_cert_fingerprint, prev);
   hex_encode(r->csr_digest, csr);
   hex_encode(r->csr_spki_digest, spki);
   if (aimee_pg_bind_text(st, "?1", r->operation_id) ||
       aimee_pg_bind_text(st, "?2", r->installation_id) || bind_binding(st, &r->binding, 3) ||
       aimee_pg_bind_int64(st, "?8", r->generation) ||
       aimee_pg_bind_int64(st, "?9", r->previous_enrollment_id) ||
       aimee_pg_bind_text(st, "?10", r->previous_cert_issuer) ||
       aimee_pg_bind_text(st, "?11", r->previous_cert_serial_norm) ||
       aimee_pg_bind_text(st, "?12", prev) || aimee_pg_bind_text(st, "?13", csr) ||
       aimee_pg_bind_text(st, "?14", spki))
   {
      aimee_pg_finalize(st);
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   }
   rc = pending_call(st, out);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      memset(out, 0, sizeof(*out));
   return rc;
}

static int decode_active(aimee_pg_stmt_t *st, int snapshot, db2_management_client_active_t *o)
{
   int c = 0;
   if (aimee_pg_column_count(st) != (snapshot ? 22 : 24) || col_bool(st, c++, &o->replayed) ||
       copy_text_col(st, c++, o->installation_id, sizeof(o->installation_id)) ||
       !exact_hex(o->installation_id, 32) ||
       copy_text_col(st, c++, o->replacement_lineage_id, sizeof(o->replacement_lineage_id)) ||
       !exact_hex(o->replacement_lineage_id, 32) ||
       copy_text_col(st, c++, o->authority_id, sizeof(o->authority_id)) ||
       !exact_hex(o->authority_id, 32) || col_i64(st, c++, &o->team_id) || o->team_id < 1 ||
       col_hex(st, c++, o->binding_digest) ||
       strcmp(aimee_pg_column_text(st, c) ? aimee_pg_column_text(st, c) : "", "active"))
      return -1;
   c++;
   if (col_i64(st, c++, &o->generation) || o->generation < 1 ||
       col_i64(st, c++, &o->enrollment_id) || o->enrollment_id < 1 ||
       copy_text_col(st, c++, o->operation_id, sizeof(o->operation_id)) ||
       !exact_hex(o->operation_id, 64) ||
       parse_kind(aimee_pg_column_text(st, c++), &o->issue_kind) ||
       parse_state(aimee_pg_column_text(st, c++), &o->issue_state) ||
       o->issue_state != DB2_MANAGEMENT_CLIENT_ISSUE_ACTIVE)
      return -1;
   if (!snapshot && (col_hex(st, c++, o->csr_digest) || col_hex(st, c++, o->csr_spki_digest)))
      return -1;
   if (col_hex(st, c++, o->public_bundle_digest) ||
       copy_text_col(st, c++, o->cert_identity, sizeof(o->cert_identity)) ||
       copy_text_col(st, c++, o->cert_issuer, sizeof(o->cert_issuer)) ||
       copy_text_col(st, c++, o->cert_serial_norm, sizeof(o->cert_serial_norm)) ||
       !exact_hex(o->cert_serial_norm, strlen(o->cert_serial_norm)) ||
       col_hex(st, c++, o->cert_fingerprint) || col_hex(st, c++, o->cert_spki_digest) ||
       col_i64(st, c++, &o->cert_not_before_epoch) || col_i64(st, c++, &o->cert_not_after_epoch) ||
       o->cert_not_after_epoch <= o->cert_not_before_epoch ||
       col_i64(st, c++, &o->revocation_generation) || o->revocation_generation < 1 ||
       col_i64(st, c++, &o->activated_at_epoch) || o->activated_at_epoch < 1)
      return -1;
   return c == aimee_pg_column_count(st) ? 0 : -1;
}

static db2_management_client_instance_result_t active_call(aimee_pg_stmt_t *st, int snapshot,
                                                           db2_management_client_active_t *out)
{
   db2_management_client_instance_result_t rc = first_row(st);
   db2_management_client_active_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      rc = finish_row(st, decode_active(st, snapshot, &candidate) == 0);
   else
      aimee_pg_finalize(st);
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      *out = candidate;
   return rc;
}

db2_management_client_instance_result_t
db2_management_client_instance_activate(const db2_management_client_activation_request_t *r,
                                        db2_management_client_active_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!r || !out || !exact_hex(r->operation_id, 64) || !exact_hex(r->installation_id, 32) ||
       !binding_valid(&r->binding) || r->generation < 1 ||
       (r->issue_kind != DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL &&
        r->issue_kind != DB2_MANAGEMENT_CLIENT_ISSUE_RENEW) ||
       r->has_previous != (r->issue_kind == DB2_MANAGEMENT_CLIENT_ISSUE_RENEW) ||
       (r->has_previous &&
        (r->previous_enrollment_id < 1 ||
         !strict_text(r->previous_cert_issuer, sizeof(r->previous_cert_issuer)) ||
         !strict_text(r->previous_cert_serial_norm, sizeof(r->previous_cert_serial_norm)) ||
         !exact_hex(r->previous_cert_serial_norm, strlen(r->previous_cert_serial_norm)))) ||
       !strict_text(r->verified_ca_issuer, sizeof(r->verified_ca_issuer)) ||
       !strict_text(r->leaf_issuer, sizeof(r->leaf_issuer)) ||
       !strict_text(r->leaf_serial_norm, sizeof(r->leaf_serial_norm)) ||
       !exact_hex(r->leaf_serial_norm, strlen(r->leaf_serial_norm)) ||
       r->leaf_not_before_epoch < 1 || r->leaf_not_after_epoch <= r->leaf_not_before_epoch)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   aimee_pg_stmt_t *st = NULL;
   db2_management_client_instance_result_t rc =
       runtime_stmt("SELECT * FROM "
                    "public.kb_management_instance_activate(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
                    "?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24)",
                    &st);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      return rc;
   char prev[65], csr[65], csr_spki[65], bundle[65], ca[65], leaf_fp[65], leaf_spki[65];
   hex_encode(r->previous_cert_fingerprint, prev);
   hex_encode(r->csr_digest, csr);
   hex_encode(r->csr_spki_digest, csr_spki);
   hex_encode(r->public_bundle_digest, bundle);
   hex_encode(r->verified_ca_fingerprint, ca);
   hex_encode(r->leaf_fingerprint, leaf_fp);
   hex_encode(r->leaf_spki_digest, leaf_spki);
   const char *kind = r->issue_kind == DB2_MANAGEMENT_CLIENT_ISSUE_INITIAL ? "initial" : "renew";
   int bind_fail = aimee_pg_bind_text(st, "?1", r->operation_id) ||
                   aimee_pg_bind_text(st, "?2", r->installation_id) ||
                   bind_binding(st, &r->binding, 3) || aimee_pg_bind_text(st, "?8", kind) ||
                   aimee_pg_bind_int64(st, "?9", r->generation);
   if (r->has_previous)
      bind_fail = bind_fail || aimee_pg_bind_int64(st, "?10", r->previous_enrollment_id) ||
                  aimee_pg_bind_text(st, "?11", r->previous_cert_issuer) ||
                  aimee_pg_bind_text(st, "?12", r->previous_cert_serial_norm) ||
                  aimee_pg_bind_text(st, "?13", prev);
   else
      bind_fail = bind_fail || aimee_pg_bind_null(st, "?10") || aimee_pg_bind_null(st, "?11") ||
                  aimee_pg_bind_null(st, "?12") || aimee_pg_bind_null(st, "?13");
   bind_fail = bind_fail || aimee_pg_bind_text(st, "?14", csr) ||
               aimee_pg_bind_text(st, "?15", csr_spki) || aimee_pg_bind_text(st, "?16", bundle) ||
               aimee_pg_bind_text(st, "?17", r->verified_ca_issuer) ||
               aimee_pg_bind_text(st, "?18", ca) || aimee_pg_bind_text(st, "?19", r->leaf_issuer) ||
               aimee_pg_bind_text(st, "?20", r->leaf_serial_norm) ||
               aimee_pg_bind_text(st, "?21", leaf_fp) || aimee_pg_bind_text(st, "?22", leaf_spki) ||
               aimee_pg_bind_int64(st, "?23", r->leaf_not_before_epoch) ||
               aimee_pg_bind_int64(st, "?24", r->leaf_not_after_epoch);
   if (bind_fail)
   {
      aimee_pg_finalize(st);
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   }
   rc = active_call(st, 0, out);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      memset(out, 0, sizeof(*out));
   return rc;
}

db2_management_client_instance_result_t
db2_management_client_instance_snapshot(const char installation_id[33],
                                        const db2_management_client_instance_binding_t *binding,
                                        db2_management_client_active_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || !exact_hex(installation_id, 32) || !binding_valid(binding))
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   aimee_pg_stmt_t *st = NULL;
   db2_management_client_instance_result_t rc =
       runtime_stmt("SELECT * FROM public.kb_management_instance_snapshot(?1,?2,?3,?4,?5,?6)", &st);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      return rc;
   if (aimee_pg_bind_text(st, "?1", installation_id) || bind_binding(st, binding, 2))
   {
      aimee_pg_finalize(st);
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   }
   rc = active_call(st, 1, out);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      memset(out, 0, sizeof(*out));
   return rc;
}

db2_management_client_instance_result_t
db2_management_client_instance_expire_quarantine(int limit,
                                                 db2_management_client_maintenance_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || limit < 1 || limit > 100)
      return DB2_MANAGEMENT_CLIENT_INSTANCE_INVALID;
   aimee_pg_stmt_t *st = NULL;
   db2_management_client_instance_result_t rc =
       runtime_stmt("SELECT * FROM public.kb_management_instance_expire_quarantine(?1)", &st);
   if (rc != DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      return rc;
   if (aimee_pg_bind_int(st, "?1", limit))
   {
      aimee_pg_finalize(st);
      return DB2_MANAGEMENT_CLIENT_INSTANCE_UNAVAILABLE;
   }
   rc = first_row(st);
   db2_management_client_maintenance_t candidate = {0};
   int decoded =
       rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK && aimee_pg_column_count(st) == 3 &&
       col_i64(st, 0, &candidate.expired_grants) == 0 &&
       col_i64(st, 1, &candidate.expired_issues) == 0 &&
       col_i64(st, 2, &candidate.quarantined_issues) == 0 && candidate.expired_grants >= 0 &&
       candidate.expired_issues >= 0 && candidate.quarantined_issues >= 0 &&
       candidate.expired_grants + candidate.expired_issues + candidate.quarantined_issues <= limit;
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      rc = finish_row(st, decoded);
   else
      aimee_pg_finalize(st);
   if (rc == DB2_MANAGEMENT_CLIENT_INSTANCE_OK)
      *out = candidate;
   return rc;
}
