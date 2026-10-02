#include "management_action_journal.h"

#include "kb_store_internal.h"
#include "management_intent_fields.h"
#include "kb_store_tenant.h"
#include "db_postgres.h"
#include "platform_random.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define MANAGEMENT_ACTION_JSON_INT_MAX INT64_C(9007199254740991)

kb_store_management_action_result_t kb_store_management_action_classify_sqlstate(const char *s)
{
   if (!s || strlen(s) != 5)
      return KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE;
   if (!strcmp(s, "22023"))
      return KB_STORE_MANAGEMENT_ACTION_INVALID;
   if (!strcmp(s, "28000") || !strcmp(s, "42501"))
      return KB_STORE_MANAGEMENT_ACTION_DENIED;
   if (!strcmp(s, "23505"))
      return KB_STORE_MANAGEMENT_ACTION_CONFLICT;
   if (!strcmp(s, "40001") || !strcmp(s, "40P01"))
      return KB_STORE_MANAGEMENT_ACTION_RETRY;
   if (!strcmp(s, "25006"))
      return KB_STORE_MANAGEMENT_ACTION_RETRY;
   if (!strcmp(s, "55000") || !strcmp(s, "P0002"))
      return KB_STORE_MANAGEMENT_ACTION_INTEGRITY;
   return KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE;
}

static kb_store_management_action_result_t
operation_init_common(int64_t team_id, const char *target,
                      kb_store_management_action_capability_t capability, const char *digest,
                      const char *issuer, const char *kid, int ttl, const char *installation,
                      kb_store_management_action_operation_t *out)
{
   if (!out)
      return KB_STORE_MANAGEMENT_ACTION_INVALID;
   memset(out, 0, sizeof(*out));
   if (team_id < 1 || capability != KB_STORE_MANAGEMENT_ACTION_CAP_REMOTE_WRITES || ttl < 1 ||
       ttl > 90 || !target || !issuer || !kid || !installation || !digest ||
       !kb_store_intent_input_text(target, KB_STORE_MANAGEMENT_ACTION_SERVER_MAX, 1) ||
       !kb_store_intent_input_text(issuer, KB_STORE_MANAGEMENT_ACTION_TOKEN_ISSUER_MAX, 0) ||
       !kb_store_intent_input_text(kid, KB_STORE_MANAGEMENT_ACTION_KID_MAX, 1) ||
       !kb_store_intent_input_hex(digest, 64) || !kb_store_intent_input_hex(installation, 32))
      return KB_STORE_MANAGEMENT_ACTION_INVALID;

   kb_store_management_action_operation_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   candidate.team_id = team_id;
   candidate.capability = capability;
   candidate.ttl_seconds = ttl;
   memcpy(candidate.target_server_id, target, strlen(target));
   memcpy(candidate.request_sha256, digest, 65);
   memcpy(candidate.token_issuer, issuer, strlen(issuer));
   memcpy(candidate.kid, kid, strlen(kid));
   memcpy(candidate.installation_id, installation, 32);
   if (kb_store_intent_generate_id(candidate.correlation_id) ||
       kb_store_intent_generate_id(candidate.jti))
   {
      memset(&candidate, 0, sizeof(candidate));
      return KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   *out = candidate;
   return KB_STORE_MANAGEMENT_ACTION_OK;
}

kb_store_management_action_result_t kb_store_management_action_operation_init(
    int64_t team_id, const char *target, kb_store_management_action_capability_t capability,
    const uint8_t digest[32], const char *issuer, const char *kid, int ttl,
    const char *installation, kb_store_management_action_operation_t *out)
{
   char hex[65] = "";
   if (digest)
      kb_store_intent_hex_encode_32(digest, hex);
   return operation_init_common(team_id, target, capability, digest ? hex : NULL, issuer, kid, ttl,
                                installation, out);
}

kb_store_management_action_result_t kb_store_management_action_operation_init_hex(
    int64_t team_id, const char *target, kb_store_management_action_capability_t capability,
    const char *digest, const char *issuer, const char *kid, int ttl, const char *installation,
    kb_store_management_action_operation_t *out)
{
   return operation_init_common(team_id, target, capability, digest, issuer, kid, ttl, installation,
                                out);
}

static int operation_valid(const kb_store_management_action_operation_t *o)
{
   return o && kb_store_intent_fixed_hex(o->correlation_id, sizeof(o->correlation_id), 64) &&
          kb_store_intent_fixed_hex(o->jti, sizeof(o->jti), 64) && o->team_id > 0 &&
          o->capability == KB_STORE_MANAGEMENT_ACTION_CAP_REMOTE_WRITES &&
          kb_store_intent_fixed_text(o->target_server_id, sizeof(o->target_server_id), 127, 1) &&
          kb_store_intent_fixed_hex(o->request_sha256, sizeof(o->request_sha256), 64) &&
          kb_store_intent_fixed_text(o->token_issuer, sizeof(o->token_issuer), 255, 0) &&
          kb_store_intent_fixed_text(o->kid, sizeof(o->kid), 64, 1) && o->ttl_seconds >= 1 &&
          o->ttl_seconds <= 90 &&
          kb_store_intent_fixed_hex(o->installation_id, sizeof(o->installation_id), 32);
}

static int parse_cap(const char *s, kb_store_management_action_capability_t *out)
{
   if (!s || strcmp(s, "remote_writes"))
      return -1;
   *out = KB_STORE_MANAGEMENT_ACTION_CAP_REMOTE_WRITES;
   return 0;
}

static int parse_result(const char *s, kb_store_management_action_outcome_result_t *out)
{
   if (s && !strcmp(s, "succeeded"))
      *out = KB_STORE_MANAGEMENT_ACTION_SUCCEEDED;
   else if (s && !strcmp(s, "denied"))
      *out = KB_STORE_MANAGEMENT_ACTION_DENIED_RESULT;
   else if (s && !strcmp(s, "failed"))
      *out = KB_STORE_MANAGEMENT_ACTION_FAILED;
   else if (s && !strcmp(s, "indeterminate"))
      *out = KB_STORE_MANAGEMENT_ACTION_INDETERMINATE;
   else
      return -1;
   return 0;
}

static int parse_class(const char *s, kb_store_management_action_outcome_class_t *out)
{
   if (s && !strcmp(s, "remote_success"))
      *out = KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS;
   else if (s && !strcmp(s, "remote_denied"))
      *out = KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED;
   else if (s && !strcmp(s, "remote_failure"))
      *out = KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE;
   else if (s && !strcmp(s, "transport_ambiguous"))
      *out = KB_STORE_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS;
   else if (s && !strcmp(s, "protocol_failure"))
      *out = KB_STORE_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE;
   else if (s && !strcmp(s, "local_failure"))
      *out = KB_STORE_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE;
   else
      return -1;
   return 0;
}

static const char *result_text(kb_store_management_action_outcome_result_t r)
{
   switch (r)
   {
   case KB_STORE_MANAGEMENT_ACTION_SUCCEEDED:
      return "succeeded";
   case KB_STORE_MANAGEMENT_ACTION_DENIED_RESULT:
      return "denied";
   case KB_STORE_MANAGEMENT_ACTION_FAILED:
      return "failed";
   case KB_STORE_MANAGEMENT_ACTION_INDETERMINATE:
      return "indeterminate";
   }
   return NULL;
}

static const char *class_text(kb_store_management_action_outcome_class_t c)
{
   switch (c)
   {
   case KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS:
      return "remote_success";
   case KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED:
      return "remote_denied";
   case KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE:
      return "remote_failure";
   case KB_STORE_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS:
      return "transport_ambiguous";
   case KB_STORE_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE:
      return "protocol_failure";
   case KB_STORE_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE:
      return "local_failure";
   }
   return NULL;
}

static int result_class_valid(kb_store_management_action_outcome_result_t r,
                              kb_store_management_action_outcome_class_t c)
{
   return (r == KB_STORE_MANAGEMENT_ACTION_SUCCEEDED &&
           c == KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_SUCCESS) ||
          (r == KB_STORE_MANAGEMENT_ACTION_DENIED_RESULT &&
           c == KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_DENIED) ||
          (r == KB_STORE_MANAGEMENT_ACTION_INDETERMINATE &&
           (c == KB_STORE_MANAGEMENT_ACTION_CLASS_TRANSPORT_AMBIGUOUS ||
            c == KB_STORE_MANAGEMENT_ACTION_CLASS_PROTOCOL_FAILURE)) ||
          (r == KB_STORE_MANAGEMENT_ACTION_FAILED &&
           (c == KB_STORE_MANAGEMENT_ACTION_CLASS_REMOTE_FAILURE ||
            c == KB_STORE_MANAGEMENT_ACTION_CLASS_LOCAL_FAILURE));
}

static kb_store_management_action_result_t first_row(aimee_pg_stmt_t *st)
{
   char err[256] = "";
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ERR)
      return kb_store_management_action_classify_sqlstate(aimee_pg_sqlstate(st));
   return step == AIMEE_PG_ROW ? KB_STORE_MANAGEMENT_ACTION_OK
                               : KB_STORE_MANAGEMENT_ACTION_INTEGRITY;
}

static kb_store_management_action_result_t finish_row(aimee_pg_stmt_t *st, int valid)
{
   char err[256] = "";
   kb_store_management_action_result_t rc =
       valid ? KB_STORE_MANAGEMENT_ACTION_OK : KB_STORE_MANAGEMENT_ACTION_INTEGRITY;
   if (valid)
   {
      aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
      if (step == AIMEE_PG_ERR)
         rc = kb_store_management_action_classify_sqlstate(aimee_pg_sqlstate(st));
      else if (step != AIMEE_PG_DONE)
         rc = KB_STORE_MANAGEMENT_ACTION_INTEGRITY;
   }
   aimee_pg_finalize(st);
   return rc;
}

static int decode_intent(aimee_pg_stmt_t *st, kb_store_management_action_intent_t *o)
{
   if (aimee_pg_column_count(st) != 25 || kb_store_intent_col_bool(st, 0, &o->replayed) ||
       kb_store_intent_copy_hex_col(st, 1, o->correlation_id, 64) ||
       kb_store_intent_copy_hex_col(st, 2, o->jti, 64) ||
       kb_store_intent_col_i64(st, 3, &o->team_id) || o->team_id < 1 ||
       kb_store_intent_copy_col(st, 4, o->actor_identity, sizeof(o->actor_identity), 576, 0) ||
       !kb_store_intent_canonical_actor(o->actor_identity, sizeof(o->actor_identity)) ||
       parse_cap(aimee_pg_column_text(st, 5), &o->capability) ||
       kb_store_intent_copy_col(st, 6, o->target_server_id, sizeof(o->target_server_id), 127, 1) ||
       kb_store_intent_copy_hex_col(st, 7, o->request_sha256, 64) ||
       kb_store_intent_copy_col(st, 8, o->token_issuer, sizeof(o->token_issuer), 255, 0) ||
       kb_store_intent_copy_col(st, 9, o->audience, sizeof(o->audience), 127, 1) ||
       kb_store_intent_copy_col(st, 10, o->kid, sizeof(o->kid), 64, 1) ||
       kb_store_intent_col_i64(st, 11, &o->issued_at) ||
       kb_store_intent_col_i64(st, 12, &o->expires_at) || o->issued_at < 1 ||
       o->issued_at > MANAGEMENT_ACTION_JSON_INT_MAX || o->expires_at <= o->issued_at ||
       o->expires_at > MANAGEMENT_ACTION_JSON_INT_MAX || o->expires_at - o->issued_at > 90 ||
       kb_store_intent_copy_hex_col(st, 13, o->installation_id, 32) ||
       kb_store_intent_col_i64(st, 14, &o->installation_generation) ||
       o->installation_generation < 1 ||
       kb_store_intent_col_i64(st, 15, &o->installation_enrollment_id) ||
       o->installation_enrollment_id < 1 ||
       kb_store_intent_copy_col(st, 16, o->local_cert_issuer, sizeof(o->local_cert_issuer), 511,
                                0) ||
       kb_store_intent_copy_col(st, 17, o->local_cert_serial_norm,
                                sizeof(o->local_cert_serial_norm), 79, 1) ||
       !kb_store_intent_fixed_hex(o->local_cert_serial_norm, strlen(o->local_cert_serial_norm) + 1,
                                  strlen(o->local_cert_serial_norm)) ||
       kb_store_intent_copy_hex_col(st, 18, o->local_cert_fingerprint, 64) ||
       kb_store_intent_col_i64(st, 19, &o->target_enrollment_id) || o->target_enrollment_id < 1 ||
       kb_store_intent_copy_col(st, 20, o->target_mgmt_issuer, sizeof(o->target_mgmt_issuer), 511,
                                0) ||
       kb_store_intent_copy_col(st, 21, o->target_mgmt_serial_norm,
                                sizeof(o->target_mgmt_serial_norm), 79, 1) ||
       !kb_store_intent_fixed_hex(o->target_mgmt_serial_norm,
                                  strlen(o->target_mgmt_serial_norm) + 1,
                                  strlen(o->target_mgmt_serial_norm)) ||
       kb_store_intent_copy_hex_col(st, 22, o->target_mgmt_fingerprint, 64) ||
       kb_store_intent_col_i64(st, 23, &o->revocation_generation) || o->revocation_generation < 1 ||
       kb_store_intent_col_i64(st, 24, &o->created_at_epoch) || o->created_at_epoch < 1)
      return -1;
   if (strcmp(o->audience, o->target_server_id))
      return -1;
   o->dispatch_eligibility = KB_STORE_MANAGEMENT_ACTION_JOURNALED_ONLY;
   return 0;
}

static int decode_outcome(aimee_pg_stmt_t *st, kb_store_management_action_outcome_t *o)
{
   if (aimee_pg_column_count(st) != 8 || kb_store_intent_col_bool(st, 0, &o->replayed) ||
       kb_store_intent_copy_hex_col(st, 1, o->correlation_id, 64) ||
       kb_store_intent_col_i64(st, 2, &o->team_id) || o->team_id < 1 ||
       parse_result(aimee_pg_column_text(st, 3), &o->result) ||
       parse_class(aimee_pg_column_text(st, 4), &o->result_class) ||
       !result_class_valid(o->result, o->result_class))
      return -1;
   if (aimee_pg_column_is_null(st, 5))
      o->has_status_code = 0;
   else if (kb_store_intent_col_int(st, 5, &o->status_code) || o->status_code < 100 ||
            o->status_code > 599)
      return -1;
   else
      o->has_status_code = 1;
   if (aimee_pg_column_is_null(st, 6))
      o->has_response_sha256 = 0;
   else if (kb_store_intent_copy_hex_col(st, 6, o->response_sha256, 64))
      return -1;
   else
      o->has_response_sha256 = 1;
   return kb_store_intent_col_i64(st, 7, &o->completed_at_epoch) || o->completed_at_epoch < 1 ? -1
                                                                                              : 0;
}

kb_store_management_action_result_t
kb_store_management_action_intent_start(const kb_principal_t *principal,
                                        const kb_store_management_action_operation_t *op,
                                        kb_store_management_action_intent_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || !operation_valid(op) || !principal)
      return KB_STORE_MANAGEMENT_ACTION_INVALID;
   int tx = kb_store_tenant_scope_begin(principal, op->team_id);
   if (tx != 0)
      return (tx == KB_STORE_ERR_TENANT_DENIED || tx == KB_STORE_ERR_TENANT_UNAUTHENTICATED)
                 ? KB_STORE_MANAGEMENT_ACTION_DENIED
                 : KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE;
   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(
       kb_store_conn(),
       "SELECT * FROM public.kb_management_action_intent_start(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
       &kind, err, sizeof(err));
   if (!st)
   {
      kb_store_tenant_scope_rollback();
      return KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE;
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
   kb_store_management_action_intent_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   kb_store_management_action_result_t rc =
       bound ? KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE : first_row(st);
   if (bound)
      aimee_pg_finalize(st);
   else if (rc == KB_STORE_MANAGEMENT_ACTION_OK)
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
   if (rc != KB_STORE_MANAGEMENT_ACTION_OK)
   {
      kb_store_tenant_scope_rollback();
      return rc;
   }
   if (kb_store_tenant_scope_commit() != 0)
      return KB_STORE_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS;
   *out = candidate;
   return KB_STORE_MANAGEMENT_ACTION_OK;
}

kb_store_management_action_result_t
kb_store_management_action_outcome_append(const kb_principal_t *principal,
                                          const kb_store_management_action_outcome_operation_t *op,
                                          kb_store_management_action_outcome_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   const char *result = op ? result_text(op->result) : NULL;
   const char *class_name = op ? class_text(op->result_class) : NULL;
   if (!out || !principal || !op ||
       !kb_store_intent_fixed_hex(op->correlation_id, sizeof(op->correlation_id), 64) ||
       op->team_id < 1 || !result || !class_name ||
       !result_class_valid(op->result, op->result_class) ||
       (op->has_status_code != 0 && op->has_status_code != 1) ||
       (op->has_status_code && (op->status_code < 100 || op->status_code > 599)) ||
       (!op->has_status_code && op->status_code != 0) ||
       (op->has_response_sha256 != 0 && op->has_response_sha256 != 1) ||
       (op->has_response_sha256 &&
        !kb_store_intent_fixed_hex(op->response_sha256, sizeof(op->response_sha256), 64)) ||
       (!op->has_response_sha256 &&
        memcmp(op->response_sha256, (char[KB_STORE_MANAGEMENT_ACTION_ID_HEX + 1]){0},
               sizeof(op->response_sha256))))
      return KB_STORE_MANAGEMENT_ACTION_INVALID;
   int tx = kb_store_tenant_scope_begin(principal, op->team_id);
   if (tx != 0)
      return (tx == KB_STORE_ERR_TENANT_DENIED || tx == KB_STORE_ERR_TENANT_UNAUTHENTICATED)
                 ? KB_STORE_MANAGEMENT_ACTION_DENIED
                 : KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE;
   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(
       kb_store_conn(), "SELECT * FROM public.kb_management_action_outcome_append(?1,?2,?3,?4,?5)",
       &kind, err, sizeof(err));
   if (!st)
   {
      kb_store_tenant_scope_rollback();
      return KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   int bound = aimee_pg_bind_text(st, "?1", op->correlation_id) ||
               aimee_pg_bind_text(st, "?2", result) || aimee_pg_bind_text(st, "?3", class_name) ||
               (op->has_status_code ? aimee_pg_bind_int(st, "?4", op->status_code)
                                    : aimee_pg_bind_null(st, "?4")) ||
               (op->has_response_sha256 ? aimee_pg_bind_text(st, "?5", op->response_sha256)
                                        : aimee_pg_bind_null(st, "?5"));
   kb_store_management_action_outcome_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   kb_store_management_action_result_t rc =
       bound ? KB_STORE_MANAGEMENT_ACTION_UNAVAILABLE : first_row(st);
   if (bound)
      aimee_pg_finalize(st);
   else if (rc == KB_STORE_MANAGEMENT_ACTION_OK)
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
   if (rc != KB_STORE_MANAGEMENT_ACTION_OK)
   {
      kb_store_tenant_scope_rollback();
      return rc;
   }
   if (kb_store_tenant_scope_commit() != 0)
      return KB_STORE_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS;
   *out = candidate;
   return KB_STORE_MANAGEMENT_ACTION_OK;
}
