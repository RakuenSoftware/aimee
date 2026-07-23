#include "management_action_journal.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"
#include "platform_random.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MANAGEMENT_ACTION_JSON_INT_MAX INT64_C(9007199254740991)

static int fixed_text(const char *s, size_t cap, size_t max, int token)
{
   if (!s || cap < 2 || max >= cap)
      return 0;
   size_t n = strnlen(s, cap);
   if (n == 0 || n > max || n == cap)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f ||
          (token && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-')))
         return 0;
   }
   for (size_t i = n + 1; i < cap; ++i)
      if (s[i] != 0)
         return 0;
   return 1;
}

static int input_text(const char *s, size_t max, int token)
{
   if (!s)
      return 0;
   size_t n = strnlen(s, max + 1);
   if (n == 0 || n > max)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f ||
          (token && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-')))
         return 0;
   }
   return 1;
}

static int exact_hex_record(const char *s, size_t cap, size_t n)
{
   if (!s || cap != n + 1 || s[n] != 0)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int input_exact_hex(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static void hex_encode_32(const uint8_t in[32], char out[65])
{
   static const char digits[] = "0123456789abcdef";
   for (size_t i = 0; i < 32; ++i)
   {
      out[i * 2] = digits[in[i] >> 4];
      out[i * 2 + 1] = digits[in[i] & 15];
   }
   out[64] = 0;
}

static int generate_id(char out[65])
{
   uint8_t raw[32];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      return -1;
   hex_encode_32(raw, out);
   return 0;
}

static int encoded_component(const char *s, size_t n)
{
   if (!s || n == 0)
      return 0;
   for (size_t i = 0; i < n; ++i)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f || c == ':')
         return 0;
      if (c == '%')
      {
         if (i + 2 >= n ||
             !((s[i + 1] == '2' && s[i + 2] == '5') || (s[i + 1] == '3' && s[i + 2] == 'A')))
            return 0;
         i += 2;
      }
   }
   return 1;
}

static int canonical_actor(const char *s, size_t cap)
{
   if (!fixed_text(s, cap, DB2_MANAGEMENT_ACTION_ACTOR_MAX, 0))
      return 0;
   if (!strcmp(s, "owner"))
      return 1;
   int cert = !strncmp(s, "cert:", 5);
   size_t prefix = cert ? 5 : (!strncmp(s, "oidc:", 5) ? 5 : 0);
   if (!prefix)
      return 0;
   const char *middle = strchr(s + prefix, ':');
   if (!middle || strchr(middle + 1, ':') ||
       !encoded_component(s + prefix, (size_t)(middle - (s + prefix))) ||
       !encoded_component(middle + 1, strlen(middle + 1)))
      return 0;
   if (cert)
   {
      size_t serial_len = strlen(middle + 1);
      if (serial_len == 0 || serial_len > 79)
         return 0;
      for (const char *p = middle + 1; *p; ++p)
         if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
            return 0;
   }
   return 1;
}

db2_management_action_result_t db2_management_action_classify_sqlstate(const char *s)
{
   if (!s || strlen(s) != 5)
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   if (!strcmp(s, "22023"))
      return DB2_MANAGEMENT_ACTION_INVALID;
   if (!strcmp(s, "28000") || !strcmp(s, "42501"))
      return DB2_MANAGEMENT_ACTION_DENIED;
   if (!strcmp(s, "23505"))
      return DB2_MANAGEMENT_ACTION_CONFLICT;
   if (!strcmp(s, "40001") || !strcmp(s, "40P01"))
      return DB2_MANAGEMENT_ACTION_RETRY;
   if (!strcmp(s, "25006"))
      return DB2_MANAGEMENT_ACTION_RETRY;
   if (!strcmp(s, "55000") || !strcmp(s, "P0002"))
      return DB2_MANAGEMENT_ACTION_INTEGRITY;
   return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
}

static db2_management_action_result_t
operation_init_common(int64_t team_id, const char *target,
                      db2_management_action_capability_t capability, const char *digest,
                      const char *issuer, const char *kid, int ttl, const char *installation,
                      db2_management_action_operation_t *out)
{
   if (!out)
      return DB2_MANAGEMENT_ACTION_INVALID;
   memset(out, 0, sizeof(*out));
   if (team_id < 1 || capability != DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES || ttl < 1 ||
       ttl > 90 || !target || !issuer || !kid || !installation || !digest ||
       !input_text(target, DB2_MANAGEMENT_ACTION_SERVER_MAX, 1) ||
       !input_text(issuer, DB2_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX, 0) ||
       !input_text(kid, DB2_MANAGEMENT_ACTION_KID_MAX, 1) || !input_exact_hex(digest, 64) ||
       !input_exact_hex(installation, 32))
      return DB2_MANAGEMENT_ACTION_INVALID;

   db2_management_action_operation_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   candidate.team_id = team_id;
   candidate.capability = capability;
   candidate.ttl_seconds = ttl;
   memcpy(candidate.target_server_id, target, strlen(target));
   memcpy(candidate.request_sha256, digest, 65);
   memcpy(candidate.token_issuer, issuer, strlen(issuer));
   memcpy(candidate.kid, kid, strlen(kid));
   memcpy(candidate.installation_id, installation, 32);
   if (generate_id(candidate.correlation_id) || generate_id(candidate.jti))
   {
      memset(&candidate, 0, sizeof(candidate));
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   *out = candidate;
   return DB2_MANAGEMENT_ACTION_OK;
}

db2_management_action_result_t db2_management_action_operation_init(
    int64_t team_id, const char *target, db2_management_action_capability_t capability,
    const uint8_t digest[32], const char *issuer, const char *kid, int ttl,
    const char *installation, db2_management_action_operation_t *out)
{
   char hex[65] = "";
   if (digest)
      hex_encode_32(digest, hex);
   return operation_init_common(team_id, target, capability, digest ? hex : NULL, issuer, kid, ttl,
                                installation, out);
}

db2_management_action_result_t db2_management_action_operation_init_hex(
    int64_t team_id, const char *target, db2_management_action_capability_t capability,
    const char *digest, const char *issuer, const char *kid, int ttl, const char *installation,
    db2_management_action_operation_t *out)
{
   return operation_init_common(team_id, target, capability, digest, issuer, kid, ttl, installation,
                                out);
}

static int operation_valid(const db2_management_action_operation_t *o)
{
   return o && exact_hex_record(o->correlation_id, sizeof(o->correlation_id), 64) &&
          exact_hex_record(o->jti, sizeof(o->jti), 64) && o->team_id > 0 &&
          o->capability == DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES &&
          fixed_text(o->target_server_id, sizeof(o->target_server_id), 127, 1) &&
          exact_hex_record(o->request_sha256, sizeof(o->request_sha256), 64) &&
          fixed_text(o->token_issuer, sizeof(o->token_issuer), 255, 0) &&
          fixed_text(o->kid, sizeof(o->kid), 64, 1) && o->ttl_seconds >= 1 &&
          o->ttl_seconds <= 90 &&
          exact_hex_record(o->installation_id, sizeof(o->installation_id), 32);
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

static int col_int(aimee_pg_stmt_t *st, int col, int *out)
{
   int64_t value = 0;
   if (col_i64(st, col, &value) || value < INT_MIN || value > INT_MAX)
      return -1;
   *out = (int)value;
   return 0;
}

static int copy_col(aimee_pg_stmt_t *st, int col, char *out, size_t cap, size_t max, int token)
{
   const char *s = aimee_pg_column_text(st, col);
   if (aimee_pg_column_is_null(st, col) || !s)
      return -1;
   size_t n = strnlen(s, cap);
   if (n == cap || n == 0 || n > max)
      return -1;
   memset(out, 0, cap);
   memcpy(out, s, n);
   return fixed_text(out, cap, max, token) ? 0 : -1;
}

static int copy_hex_col(aimee_pg_stmt_t *st, int col, char *out, size_t n)
{
   if (aimee_pg_column_is_null(st, col))
      return -1;
   const char *s = aimee_pg_column_text(st, col);
   if (!s || strnlen(s, n + 1) != n)
      return -1;
   memset(out, 0, n + 1);
   memcpy(out, s, n);
   return exact_hex_record(out, n + 1, n) ? 0 : -1;
}

static int parse_cap(const char *s, db2_management_action_capability_t *out)
{
   if (!s || strcmp(s, "remote_writes"))
      return -1;
   *out = DB2_MANAGEMENT_ACTION_CAP_REMOTE_WRITES;
   return 0;
}

static int parse_result(const char *s, db2_management_action_outcome_result_t *out)
{
   if (s && !strcmp(s, "succeeded"))
      *out = DB2_MANAGEMENT_ACTION_SUCCEEDED;
   else if (s && !strcmp(s, "denied"))
      *out = DB2_MANAGEMENT_ACTION_DENIED_RESULT;
   else if (s && !strcmp(s, "failed"))
      *out = DB2_MANAGEMENT_ACTION_FAILED;
   else if (s && !strcmp(s, "indeterminate"))
      *out = DB2_MANAGEMENT_ACTION_INDETERMINATE;
   else
      return -1;
   return 0;
}

static int parse_class(const char *s, db2_management_action_outcome_class_t *out)
{
   if (s && !strcmp(s, "remote_success"))
      *out = DB2_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS;
   else if (s && !strcmp(s, "remote_denied"))
      *out = DB2_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED;
   else if (s && !strcmp(s, "remote_failure"))
      *out = DB2_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE;
   else if (s && !strcmp(s, "transport_ambiguous"))
      *out = DB2_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS;
   else if (s && !strcmp(s, "protocol_failure"))
      *out = DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE;
   else if (s && !strcmp(s, "local_failure"))
      *out = DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE;
   else
      return -1;
   return 0;
}

static const char *result_text(db2_management_action_outcome_result_t r)
{
   switch (r)
   {
   case DB2_MANAGEMENT_ACTION_SUCCEEDED:
      return "succeeded";
   case DB2_MANAGEMENT_ACTION_DENIED_RESULT:
      return "denied";
   case DB2_MANAGEMENT_ACTION_FAILED:
      return "failed";
   case DB2_MANAGEMENT_ACTION_INDETERMINATE:
      return "indeterminate";
   }
   return NULL;
}

static const char *class_text(db2_management_action_outcome_class_t c)
{
   switch (c)
   {
   case DB2_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS:
      return "remote_success";
   case DB2_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED:
      return "remote_denied";
   case DB2_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE:
      return "remote_failure";
   case DB2_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS:
      return "transport_ambiguous";
   case DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE:
      return "protocol_failure";
   case DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE:
      return "local_failure";
   }
   return NULL;
}

static int result_class_valid(db2_management_action_outcome_result_t r,
                              db2_management_action_outcome_class_t c)
{
   return (r == DB2_MANAGEMENT_ACTION_SUCCEEDED &&
           c == DB2_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS) ||
          (r == DB2_MANAGEMENT_ACTION_DENIED_RESULT &&
           c == DB2_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED) ||
          (r == DB2_MANAGEMENT_ACTION_INDETERMINATE &&
           (c == DB2_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS ||
            c == DB2_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE)) ||
          (r == DB2_MANAGEMENT_ACTION_FAILED && (c == DB2_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE ||
                                                 c == DB2_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE));
}

static db2_management_action_result_t first_row(aimee_pg_stmt_t *st)
{
   char err[256] = "";
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ERR)
      return db2_management_action_classify_sqlstate(aimee_pg_sqlstate(st));
   return step == AIMEE_PG_ROW ? DB2_MANAGEMENT_ACTION_OK : DB2_MANAGEMENT_ACTION_INTEGRITY;
}

static db2_management_action_result_t finish_row(aimee_pg_stmt_t *st, int valid)
{
   char err[256] = "";
   db2_management_action_result_t rc =
       valid ? DB2_MANAGEMENT_ACTION_OK : DB2_MANAGEMENT_ACTION_INTEGRITY;
   if (valid)
   {
      aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
      if (step == AIMEE_PG_ERR)
         rc = db2_management_action_classify_sqlstate(aimee_pg_sqlstate(st));
      else if (step != AIMEE_PG_DONE)
         rc = DB2_MANAGEMENT_ACTION_INTEGRITY;
   }
   aimee_pg_finalize(st);
   return rc;
}

static int decode_intent(aimee_pg_stmt_t *st, db2_management_action_intent_t *o)
{
   if (aimee_pg_column_count(st) != 25 || col_bool(st, 0, &o->replayed) ||
       copy_hex_col(st, 1, o->correlation_id, 64) || copy_hex_col(st, 2, o->jti, 64) ||
       col_i64(st, 3, &o->team_id) || o->team_id < 1 ||
       copy_col(st, 4, o->actor_identity, sizeof(o->actor_identity), 576, 0) ||
       !canonical_actor(o->actor_identity, sizeof(o->actor_identity)) ||
       parse_cap(aimee_pg_column_text(st, 5), &o->capability) ||
       copy_col(st, 6, o->target_server_id, sizeof(o->target_server_id), 127, 1) ||
       copy_hex_col(st, 7, o->request_sha256, 64) ||
       copy_col(st, 8, o->token_issuer, sizeof(o->token_issuer), 255, 0) ||
       copy_col(st, 9, o->audience, sizeof(o->audience), 127, 1) ||
       copy_col(st, 10, o->kid, sizeof(o->kid), 64, 1) || col_i64(st, 11, &o->issued_at) ||
       col_i64(st, 12, &o->expires_at) || o->issued_at < 1 ||
       o->issued_at > MANAGEMENT_ACTION_JSON_INT_MAX || o->expires_at <= o->issued_at ||
       o->expires_at > MANAGEMENT_ACTION_JSON_INT_MAX || o->expires_at - o->issued_at > 90 ||
       copy_hex_col(st, 13, o->installation_id, 32) ||
       col_i64(st, 14, &o->installation_generation) || o->installation_generation < 1 ||
       col_i64(st, 15, &o->installation_enrollment_id) || o->installation_enrollment_id < 1 ||
       copy_col(st, 16, o->local_cert_issuer, sizeof(o->local_cert_issuer), 511, 0) ||
       copy_col(st, 17, o->local_cert_serial_norm, sizeof(o->local_cert_serial_norm), 79, 1) ||
       !exact_hex_record(o->local_cert_serial_norm, strlen(o->local_cert_serial_norm) + 1,
                         strlen(o->local_cert_serial_norm)) ||
       copy_hex_col(st, 18, o->local_cert_fingerprint, 64) ||
       col_i64(st, 19, &o->target_enrollment_id) || o->target_enrollment_id < 1 ||
       copy_col(st, 20, o->target_mgmt_issuer, sizeof(o->target_mgmt_issuer), 511, 0) ||
       copy_col(st, 21, o->target_mgmt_serial_norm, sizeof(o->target_mgmt_serial_norm), 79, 1) ||
       !exact_hex_record(o->target_mgmt_serial_norm, strlen(o->target_mgmt_serial_norm) + 1,
                         strlen(o->target_mgmt_serial_norm)) ||
       copy_hex_col(st, 22, o->target_mgmt_fingerprint, 64) ||
       col_i64(st, 23, &o->revocation_generation) || o->revocation_generation < 1 ||
       col_i64(st, 24, &o->created_at_epoch) || o->created_at_epoch < 1)
      return -1;
   if (strcmp(o->audience, o->target_server_id))
      return -1;
   o->dispatch_eligibility = DB2_MANAGEMENT_ACTION_JOURNALED_ONLY;
   return 0;
}

static int decode_outcome(aimee_pg_stmt_t *st, db2_management_action_outcome_t *o)
{
   if (aimee_pg_column_count(st) != 8 || col_bool(st, 0, &o->replayed) ||
       copy_hex_col(st, 1, o->correlation_id, 64) || col_i64(st, 2, &o->team_id) ||
       o->team_id < 1 || parse_result(aimee_pg_column_text(st, 3), &o->result) ||
       parse_class(aimee_pg_column_text(st, 4), &o->result_class) ||
       !result_class_valid(o->result, o->result_class))
      return -1;
   if (aimee_pg_column_is_null(st, 5))
      o->has_status_code = 0;
   else if (col_int(st, 5, &o->status_code) || o->status_code < 100 || o->status_code > 599)
      return -1;
   else
      o->has_status_code = 1;
   if (aimee_pg_column_is_null(st, 6))
      o->has_response_sha256 = 0;
   else if (copy_hex_col(st, 6, o->response_sha256, 64))
      return -1;
   else
      o->has_response_sha256 = 1;
   return col_i64(st, 7, &o->completed_at_epoch) || o->completed_at_epoch < 1 ? -1 : 0;
}

db2_management_action_result_t
db2_management_action_intent_start(const kb_principal_t *principal,
                                   const db2_management_action_operation_t *op,
                                   db2_management_action_intent_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || !operation_valid(op) || !principal)
      return DB2_MANAGEMENT_ACTION_INVALID;
   int tx = db2_tenant_scope_begin(principal, op->team_id);
   if (tx != 0)
      return (tx == DB2_ERR_TENANT_DENIED || tx == DB2_ERR_TENANT_UNAUTHENTICATED)
                 ? DB2_MANAGEMENT_ACTION_DENIED
                 : DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(
       db2_conn(),
       "SELECT * FROM public.kb_management_action_intent_start(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
       &kind, err, sizeof(err));
   if (!st)
   {
      db2_tenant_scope_rollback();
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   int bound =
       aimee_pg_bind_text(st, "?1", op->correlation_id) || aimee_pg_bind_text(st, "?2", op->jti) ||
       aimee_pg_bind_int64(st, "?3", op->team_id) ||
       aimee_pg_bind_text(st, "?4", op->target_server_id) ||
       aimee_pg_bind_text(st, "?5", "remote_writes") ||
       aimee_pg_bind_text(st, "?6", op->request_sha256) ||
       aimee_pg_bind_text(st, "?7", op->token_issuer) || aimee_pg_bind_text(st, "?8", op->kid) ||
       aimee_pg_bind_int(st, "?9", op->ttl_seconds) ||
       aimee_pg_bind_text(st, "?10", op->installation_id);
   db2_management_action_intent_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_action_result_t rc = bound ? DB2_MANAGEMENT_ACTION_UNAVAILABLE : first_row(st);
   if (bound)
      aimee_pg_finalize(st);
   else if (rc == DB2_MANAGEMENT_ACTION_OK)
      rc = finish_row(st, decode_intent(st, &candidate) == 0 &&
                              !strcmp(candidate.correlation_id, op->correlation_id) &&
                              !strcmp(candidate.jti, op->jti) && candidate.team_id == op->team_id &&
                              candidate.capability == op->capability &&
                              !strcmp(candidate.target_server_id, op->target_server_id) &&
                              !strcmp(candidate.request_sha256, op->request_sha256) &&
                              !strcmp(candidate.token_issuer, op->token_issuer) &&
                              !strcmp(candidate.kid, op->kid) &&
                              candidate.expires_at - candidate.issued_at == op->ttl_seconds &&
                              !strcmp(candidate.installation_id, op->installation_id));
   else
      aimee_pg_finalize(st);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      db2_tenant_scope_rollback();
      return rc;
   }
   if (db2_tenant_scope_commit() != 0)
      return DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS;
   *out = candidate;
   return DB2_MANAGEMENT_ACTION_OK;
}

db2_management_action_result_t
db2_management_action_outcome_append(const kb_principal_t *principal,
                                     const db2_management_action_outcome_operation_t *op,
                                     db2_management_action_outcome_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   const char *result = op ? result_text(op->result) : NULL;
   const char *class_name = op ? class_text(op->result_class) : NULL;
   if (!out || !principal || !op ||
       !exact_hex_record(op->correlation_id, sizeof(op->correlation_id), 64) || op->team_id < 1 ||
       !result || !class_name || !result_class_valid(op->result, op->result_class) ||
       (op->has_status_code != 0 && op->has_status_code != 1) ||
       (op->has_status_code && (op->status_code < 100 || op->status_code > 599)) ||
       (!op->has_status_code && op->status_code != 0) ||
       (op->has_response_sha256 != 0 && op->has_response_sha256 != 1) ||
       (op->has_response_sha256 &&
        !exact_hex_record(op->response_sha256, sizeof(op->response_sha256), 64)) ||
       (!op->has_response_sha256 &&
        memcmp(op->response_sha256, (char[DB2_MANAGEMENT_ACTION_ID_HEX + 1]){0},
               sizeof(op->response_sha256))))
      return DB2_MANAGEMENT_ACTION_INVALID;
   int tx = db2_tenant_scope_begin(principal, op->team_id);
   if (tx != 0)
      return (tx == DB2_ERR_TENANT_DENIED || tx == DB2_ERR_TENANT_UNAUTHENTICATED)
                 ? DB2_MANAGEMENT_ACTION_DENIED
                 : DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(
       db2_conn(), "SELECT * FROM public.kb_management_action_outcome_append(?1,?2,?3,?4,?5)",
       &kind, err, sizeof(err));
   if (!st)
   {
      db2_tenant_scope_rollback();
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   int bound = aimee_pg_bind_text(st, "?1", op->correlation_id) ||
               aimee_pg_bind_text(st, "?2", result) || aimee_pg_bind_text(st, "?3", class_name) ||
               (op->has_status_code ? aimee_pg_bind_int(st, "?4", op->status_code)
                                    : aimee_pg_bind_null(st, "?4")) ||
               (op->has_response_sha256 ? aimee_pg_bind_text(st, "?5", op->response_sha256)
                                        : aimee_pg_bind_null(st, "?5"));
   db2_management_action_outcome_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_action_result_t rc = bound ? DB2_MANAGEMENT_ACTION_UNAVAILABLE : first_row(st);
   if (bound)
      aimee_pg_finalize(st);
   else if (rc == DB2_MANAGEMENT_ACTION_OK)
      rc = finish_row(st, decode_outcome(st, &candidate) == 0 &&
                              !strcmp(candidate.correlation_id, op->correlation_id) &&
                              candidate.team_id == op->team_id && candidate.result == op->result &&
                              candidate.result_class == op->result_class &&
                              candidate.has_status_code == op->has_status_code &&
                              (!op->has_status_code || candidate.status_code == op->status_code) &&
                              candidate.has_response_sha256 == op->has_response_sha256 &&
                              (!op->has_response_sha256 ||
                               !strcmp(candidate.response_sha256, op->response_sha256)));
   else
      aimee_pg_finalize(st);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      db2_tenant_scope_rollback();
      return rc;
   }
   if (db2_tenant_scope_commit() != 0)
      return DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS;
   *out = candidate;
   return DB2_MANAGEMENT_ACTION_OK;
}
