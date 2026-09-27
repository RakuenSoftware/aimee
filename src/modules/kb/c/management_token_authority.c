#include "management_token_authority.h"

#include "db_postgres.h"

#include <errno.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>

#define AUTHORITY_ERROR_MAX 256U

static const char SQL_ADMIT[] = "SELECT * FROM public.kb_management_token_authority_admit(?1,?2)";
static const char SQL_READBACK[] =
    "SELECT * FROM public.kb_management_token_authority_readback(?1,?2)";
static const char SQL_USE[] = "SELECT * FROM public.kb_management_token_authority_use(?1,?2)";
static const char SQL_FINALIZE[] = "SELECT public.kb_management_token_authority_finalize(?1,?2)";
static const char SQL_KIND[] = "SELECT public.kb_management_token_intent_kind(?1,?2)";
static const char SQL_READ_CLAIM[] =
    "SELECT * FROM public.kb_management_read_authority_claim(?1,?2,?3,5)";
static const char SQL_READ_FINALIZE[] =
    "SELECT public.kb_management_read_authority_finalize(?1,?2,?3,?4)";
static const char SQL_READ_READBACK[] =
    "SELECT * FROM public.kb_management_read_authority_readback(?1,?2)";
static const char SQL_IDENTITY_ADMIT[] =
    "SELECT * FROM public.kb_management_identity_authority_admit(?1,?2)";
static const char SQL_IDENTITY_USE[] =
    "SELECT * FROM public.kb_management_identity_authority_use(?1,?2)";
static const char SQL_IDENTITY_READBACK[] =
    "SELECT * FROM public.kb_management_identity_authority_readback(?1,?2)";
static const char SQL_IDENTITY_FINALIZE[] =
    "SELECT public.kb_management_identity_authority_finalize(?1,?2)";

static db2_mgmt_token_record_valid_fn g_management_record_valid;
static db2_identity_token_record_valid_fn g_identity_record_valid;

void aimee_db2_register_token_record_validators(db2_mgmt_token_record_valid_fn management,
                                                db2_identity_token_record_valid_fn identity)
{
   g_management_record_valid = management;
   g_identity_record_valid = identity;
}

int db2_management_token_authority_record_validate(const kb_mgmt_token_authority_record_t *record)
{
   return g_management_record_valid && g_management_record_valid(record) == 1;
}

int db2_management_identity_authority_record_validate(
    const kb_identity_token_authority_record_t *record)
{
   return g_identity_record_valid && g_identity_record_valid(record) == 1;
}

static int exact_hex_input(const char *s, size_t n)
{
   if (!s || strnlen(s, n + 1) != n)
      return 0;
   for (size_t i = 0; i < n; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static db2_management_token_authority_result_t classify(const char *sqlstate, const char *error)
{
   if (error && strstr(error, "expired"))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_EXPIRED;
   if (error && strstr(error, "sealed"))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_SEALED;
   if (error &&
       (strstr(error, "denied") || strstr(error, "not authorized") || strstr(error, "not active")))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_DENIED;
   if (error && (strstr(error, "conflict") || strstr(error, "replay") ||
                 strstr(error, "already used") || strstr(error, "outcome exists")))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_CONFLICT;
   if (error && (strstr(error, "mismatch") || strstr(error, "inconsistent") ||
                 strstr(error, "invalid input") || strstr(error, "corrupt")))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   if (sqlstate && !strcmp(sqlstate, "42501"))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_DENIED;
   if (sqlstate && !strcmp(sqlstate, "23505"))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_CONFLICT;
   if (sqlstate &&
       (!strcmp(sqlstate, "22023") || !strcmp(sqlstate, "55000") || !strcmp(sqlstate, "P0002")))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   return DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
}

static int boolean(aimee_pg_stmt_t *st, int col, int *out)
{
   const char *s = aimee_pg_column_is_null(st, col) ? NULL : aimee_pg_column_text(st, col);
   if (s && ((!strcmp(s, "t")) || !strcmp(s, "true") || !strcmp(s, "1")))
      *out = 1;
   else if (s && ((!strcmp(s, "f")) || !strcmp(s, "false") || !strcmp(s, "0")))
      *out = 0;
   else
      return -1;
   return 0;
}

static int integer64(aimee_pg_stmt_t *st, int col, int64_t *out)
{
   const char *s = aimee_pg_column_is_null(st, col) ? NULL : aimee_pg_column_text(st, col);
   char *end = NULL;
   if (!s || !*s)
      return -1;
   errno = 0;
   long long value = strtoll(s, &end, 10);
   if (errno || !end || *end)
      return -1;
   *out = (int64_t)value;
   return 0;
}

static int text_col(aimee_pg_stmt_t *st, int col, char *out, size_t cap)
{
   const char *s = aimee_pg_column_is_null(st, col) ? NULL : aimee_pg_column_text(st, col);
   size_t n = s ? strnlen(s, cap) : cap;
   if (!out || cap < 2 || n == 0 || n == cap)
      return -1;
   memset(out, 0, cap);
   memcpy(out, s, n);
   for (size_t i = 0; i < n; ++i)
      if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] == 0x7f)
         return -1;
   return 0;
}

static int blob_col(aimee_pg_stmt_t *st, int col, void *out, size_t cap, size_t *len, size_t exact)
{
   const void *p = aimee_pg_column_is_null(st, col) ? NULL : aimee_pg_column_blob(st, col);
   int n = p ? aimee_pg_column_bytes(st, col) : 0;
   if (!out || !p || n < 1 || (size_t)n > cap || (exact && (size_t)n != exact))
      return -1;
   memcpy(out, p, (size_t)n);
   if (len)
      *len = (size_t)n;
   return 0;
}

static int decode_record(aimee_pg_stmt_t *st, kb_mgmt_token_authority_record_t *r)
{
   memset(r, 0, sizeof(*r));
   if (aimee_pg_column_count(st) != 42 || boolean(st, 0, &r->newly_admitted) ||
       text_col(st, 1, r->correlation_id, sizeof(r->correlation_id)) ||
       text_col(st, 2, r->jti, sizeof(r->jti)) || integer64(st, 3, &r->team_id) ||
       text_col(st, 4, r->actor_identity, sizeof(r->actor_identity)) ||
       !aimee_pg_column_text(st, 5) ||
       (strcmp(aimee_pg_column_text(st, 5), "remote_writes") &&
        strcmp(aimee_pg_column_text(st, 5), "remote_reads")) ||
       text_col(st, 6, r->target_server_id, sizeof(r->target_server_id)) ||
       text_col(st, 7, r->request_sha256, sizeof(r->request_sha256)) ||
       text_col(st, 8, r->token_issuer, sizeof(r->token_issuer)) ||
       text_col(st, 9, r->audience, sizeof(r->audience)) ||
       text_col(st, 10, r->kid, sizeof(r->kid)) || integer64(st, 11, &r->issued_at) ||
       integer64(st, 12, &r->expires_at) ||
       text_col(st, 13, r->installation_id, sizeof(r->installation_id)) ||
       integer64(st, 14, &r->installation_generation) ||
       integer64(st, 15, &r->installation_enrollment_id) ||
       text_col(st, 16, r->local_cert_issuer, sizeof(r->local_cert_issuer)) ||
       text_col(st, 17, r->local_cert_serial_norm, sizeof(r->local_cert_serial_norm)) ||
       text_col(st, 18, r->local_cert_fingerprint, sizeof(r->local_cert_fingerprint)) ||
       integer64(st, 19, &r->target_enrollment_id) ||
       text_col(st, 20, r->target_mgmt_issuer, sizeof(r->target_mgmt_issuer)) ||
       text_col(st, 21, r->target_mgmt_serial_norm, sizeof(r->target_mgmt_serial_norm)) ||
       text_col(st, 22, r->target_mgmt_fingerprint, sizeof(r->target_mgmt_fingerprint)) ||
       integer64(st, 23, &r->revocation_generation) ||
       integer64(st, 24, &r->publication_generation) ||
       text_col(st, 25, r->publication_candidate_id, sizeof(r->publication_candidate_id)) ||
       blob_col(st, 26, r->publication_manifest_sha256, 32, NULL, 32) ||
       blob_col(st, 27, r->publication_envelope_sha256, 32, NULL, 32) ||
       text_col(st, 28, r->token_custody_key_id, sizeof(r->token_custody_key_id)) ||
       integer64(st, 29, &r->token_version) ||
       blob_col(st, 30, r->token_public_key, sizeof(r->token_public_key), NULL,
                sizeof(r->token_public_key)) ||
       blob_col(st, 31, r->token_public_exponent, sizeof(r->token_public_exponent), NULL,
                sizeof(r->token_public_exponent)) ||
       blob_col(st, 32, r->token_public_digest, 32, NULL, 32) ||
       blob_col(st, 33, r->token_jwk_digest, 32, NULL, 32) ||
       integer64(st, 34, &r->vault_seal_epoch) ||
       blob_col(st, 35, r->hwm_attestation, sizeof(r->hwm_attestation), &r->hwm_attestation_len,
                0) ||
       blob_col(st, 36, r->hwm_attestation_digest, 32, NULL, 32) ||
       blob_col(st, 37, r->envelope.wrapped_dek, sizeof(r->envelope.wrapped_dek), NULL,
                sizeof(r->envelope.wrapped_dek)) ||
       blob_col(st, 38, r->envelope.nonce, sizeof(r->envelope.nonce), NULL,
                sizeof(r->envelope.nonce)) ||
       blob_col(st, 39, r->envelope.ciphertext, sizeof(r->envelope.ciphertext),
                &r->envelope.ciphertext_len, 0) ||
       blob_col(st, 40, r->envelope.tag, sizeof(r->envelope.tag), NULL, sizeof(r->envelope.tag)) ||
       integer64(st, 41, &r->key_use_created_at_epoch))
      goto invalid;
   r->capability = !strcmp(aimee_pg_column_text(st, 5), "remote_writes")
                       ? KB_MGMT_TOKEN_CAP_REMOTE_WRITES
                       : KB_MGMT_TOKEN_CAP_REMOTE_READS;
   r->envelope.seal_epoch = r->vault_seal_epoch;
   r->envelope.version = r->token_version;
   memcpy(r->envelope.hwm_attestation, r->hwm_attestation, r->hwm_attestation_len);
   r->envelope.hwm_attestation_len = r->hwm_attestation_len;
   if (!db2_management_token_authority_record_validate(r))
      goto invalid;
   return 0;
invalid:
   OPENSSL_cleanse(r, sizeof(*r));
   return -1;
}

/* Decode the identity admit row. `namespace_jti` is the 64-hex handle the caller
 * asked for; it is verified here but NOT stored, because the record carries the
 * token's own jti claim (8..128 chars) instead. Conflating the two would let a
 * token be minted under an identifier the server never replay-checks. */
static int decode_identity_record(aimee_pg_stmt_t *st, const char *namespace_jti,
                                  kb_identity_token_authority_record_t *r)
{
   memset(r, 0, sizeof(*r));
   char ns[65] = "";
   const char *tier = NULL;
   if (aimee_pg_column_count(st) != 35 || boolean(st, 0, &r->newly_admitted) ||
       text_col(st, 1, r->correlation_id, sizeof(r->correlation_id)) ||
       text_col(st, 2, ns, sizeof(ns)) || strcmp(ns, namespace_jti) ||
       text_col(st, 3, r->jti, sizeof(r->jti)) || integer64(st, 4, &r->team_id) ||
       text_col(st, 5, r->subject, sizeof(r->subject)) || !(tier = aimee_pg_column_text(st, 6)) ||
       text_col(st, 7, r->token_issuer, sizeof(r->token_issuer)) ||
       text_col(st, 8, r->audience, sizeof(r->audience)) ||
       text_col(st, 9, r->kid, sizeof(r->kid)) || integer64(st, 10, &r->issued_at) ||
       integer64(st, 11, &r->expires_at) ||
       text_col(st, 12, r->installation_id, sizeof(r->installation_id)) ||
       integer64(st, 13, &r->installation_generation) ||
       integer64(st, 14, &r->installation_enrollment_id) ||
       integer64(st, 15, &r->target_enrollment_id) ||
       integer64(st, 16, &r->revocation_generation) ||
       integer64(st, 17, &r->publication_generation) ||
       text_col(st, 18, r->publication_candidate_id, sizeof(r->publication_candidate_id)) ||
       blob_col(st, 19, r->publication_manifest_sha256, 32, NULL, 32) ||
       blob_col(st, 20, r->publication_envelope_sha256, 32, NULL, 32) ||
       text_col(st, 21, r->token_custody_key_id, sizeof(r->token_custody_key_id)) ||
       integer64(st, 22, &r->token_version) ||
       blob_col(st, 23, r->token_public_key, sizeof(r->token_public_key), NULL,
                sizeof(r->token_public_key)) ||
       blob_col(st, 24, r->token_public_exponent, sizeof(r->token_public_exponent), NULL,
                sizeof(r->token_public_exponent)) ||
       blob_col(st, 25, r->token_public_digest, 32, NULL, 32) ||
       blob_col(st, 26, r->token_jwk_digest, 32, NULL, 32) ||
       integer64(st, 27, &r->vault_seal_epoch) ||
       blob_col(st, 28, r->hwm_attestation, sizeof(r->hwm_attestation), &r->hwm_attestation_len,
                0) ||
       blob_col(st, 29, r->hwm_attestation_digest, 32, NULL, 32) ||
       blob_col(st, 30, r->envelope.wrapped_dek, sizeof(r->envelope.wrapped_dek), NULL,
                sizeof(r->envelope.wrapped_dek)) ||
       blob_col(st, 31, r->envelope.nonce, sizeof(r->envelope.nonce), NULL,
                sizeof(r->envelope.nonce)) ||
       blob_col(st, 32, r->envelope.ciphertext, sizeof(r->envelope.ciphertext),
                &r->envelope.ciphertext_len, 0) ||
       blob_col(st, 33, r->envelope.tag, sizeof(r->envelope.tag), NULL, sizeof(r->envelope.tag)) ||
       integer64(st, 34, &r->key_use_created_at_epoch))
      goto invalid;
   if (!strcmp(tier, "off"))
      r->tier = KB_IDENTITY_TIER_OFF;
   else if (!strcmp(tier, "data"))
      r->tier = KB_IDENTITY_TIER_DATA;
   else if (!strcmp(tier, "full"))
      r->tier = KB_IDENTITY_TIER_FULL;
   else
      goto invalid; /* an unrecognized tier is a corrupt row, never a default */
   r->envelope.seal_epoch = r->vault_seal_epoch;
   r->envelope.version = r->token_version;
   memcpy(r->envelope.hwm_attestation, r->hwm_attestation, r->hwm_attestation_len);
   r->envelope.hwm_attestation_len = r->hwm_attestation_len;
   if (!db2_management_identity_authority_record_validate(r))
      goto invalid;
   return 0;
invalid:
   OPENSSL_cleanse(r, sizeof(*r));
   OPENSSL_cleanse(ns, sizeof(ns));
   return -1;
}

static int role_assert(void *connection)
{
   static const char sql[] = "SELECT current_user='aimee_kb_token_authority_runtime' "
                             "AND session_user=current_user AND r.rolcanlogin AND NOT r.rolinherit "
                             "AND NOT r.rolsuper AND NOT r.rolbypassrls AND NOT r.rolcreatedb "
                             "AND NOT r.rolcreaterole AND NOT r.rolreplication "
                             "AND current_setting('search_path')='pg_catalog, pg_temp' "
                             "AND current_setting('row_security')='on' "
                             "AND NOT has_schema_privilege('public','CREATE') "
                             "FROM pg_catalog.pg_roles r WHERE r.rolname=current_user";
   char error[AUTHORITY_ERROR_MAX] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(connection, sql, error, sizeof(error));
   int value = 0;
   int ok = st && aimee_pg_step(st, error, sizeof(error)) == AIMEE_PG_ROW &&
            !boolean(st, 0, &value) && value &&
            aimee_pg_step(st, error, sizeof(error)) == AIMEE_PG_DONE;
   if (st)
      aimee_pg_finalize(st);
   OPENSSL_cleanse(error, sizeof(error));
   return ok ? 0 : -1;
}

int db2_management_token_authority_open(db2_management_token_authority_ctx_t *ctx,
                                        const char *conninfo, char *errbuf, size_t errlen)
{
   if (!ctx || !conninfo || !*conninfo)
      return -1;
   memset(ctx, 0, sizeof(*ctx));
   ctx->connection = aimee_pg_open(conninfo, errbuf, errlen);
   if (!ctx->connection)
      return -1;
   if (aimee_pg_exec(ctx->connection, "SET search_path = pg_catalog, pg_temp", errbuf, errlen) ||
       aimee_pg_exec(ctx->connection, "SET row_security = on", errbuf, errlen) ||
       role_assert(ctx->connection))
   {
      if (errbuf && errlen)
      {
         static const char message[] = "token authority database role assertion failed";
         size_t n = sizeof(message) - 1 < errlen - 1 ? sizeof(message) - 1 : errlen - 1;
         memcpy(errbuf, message, n);
         errbuf[n] = 0;
      }
      aimee_pg_close(ctx->connection);
      OPENSSL_cleanse(ctx, sizeof(*ctx));
      return -1;
   }
   return 0;
}

void db2_management_token_authority_abort(db2_management_token_authority_ctx_t *ctx)
{
   if (!ctx || !ctx->connection || !ctx->use_transaction_open)
      return;
   char error[AUTHORITY_ERROR_MAX] = "";
   (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
   ctx->use_transaction_open = 0;
   ctx->use_kind = 0;
   OPENSSL_cleanse(ctx->correlation_id, sizeof(ctx->correlation_id));
   OPENSSL_cleanse(ctx->jti, sizeof(ctx->jti));
   OPENSSL_cleanse(&ctx->use_record, sizeof(ctx->use_record));
   OPENSSL_cleanse(error, sizeof(error));
}

void db2_management_token_authority_close(db2_management_token_authority_ctx_t *ctx)
{
   if (!ctx)
      return;
   db2_management_token_authority_abort(ctx);
   if (ctx->connection)
      aimee_pg_close(ctx->connection);
   OPENSSL_cleanse(ctx, sizeof(*ctx));
}

static db2_management_token_authority_result_t
row_call(db2_management_token_authority_ctx_t *ctx, const char *sql, const char *correlation_id,
         const char *jti, kb_mgmt_token_authority_record_t *candidate,
         db2_management_token_authority_result_t empty_result)
{
   char error[AUTHORITY_ERROR_MAX] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(ctx->connection, sql, error, sizeof(error));
   if (!st)
      return classify(NULL, error);
   int bound = aimee_pg_bind_text(st, "?1", correlation_id) || aimee_pg_bind_text(st, "?2", jti);
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && decode_record(st, candidate) == 0)
   {
      step = aimee_pg_step(st, error, sizeof(error));
      rc = step == AIMEE_PG_DONE
               ? DB2_MANAGEMENT_TOKEN_AUTHORITY_OK
               : (step == AIMEE_PG_ERR ? classify(aimee_pg_sqlstate(st), error)
                                       : DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY);
   }
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = empty_result;
   aimee_pg_finalize(st);
   OPENSSL_cleanse(error, sizeof(error));
   if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK ||
       strcmp(candidate->correlation_id, correlation_id) || strcmp(candidate->jti, jti))
   {
      OPENSSL_cleanse(candidate, sizeof(*candidate));
      return rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK ? DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY
                                                     : rc;
   }
   return rc;
}

static db2_management_token_authority_result_t
committed_call(db2_management_token_authority_ctx_t *ctx, const char *begin_sql, const char *sql,
               const char *correlation_id, const char *jti,
               db2_management_token_authority_result_t empty_result,
               kb_mgmt_token_authority_record_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!ctx || !ctx->connection || ctx->use_transaction_open || !out ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   if (aimee_pg_exec(ctx->connection, begin_sql, error, sizeof(error)))
      return classify(NULL, error);
   kb_mgmt_token_authority_record_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_token_authority_result_t rc =
       row_call(ctx, sql, correlation_id, jti, &candidate, empty_result);
   if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(error, sizeof(error));
      return rc;
   }
   if (aimee_pg_exec(ctx->connection, "COMMIT", error, sizeof(error)))
   {
      /* A lost COMMIT acknowledgement makes this session unsafe to reuse. */
      aimee_pg_close(ctx->connection);
      ctx->connection = NULL;
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(error, sizeof(error));
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS;
   }
   *out = candidate;
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(error, sizeof(error));
   return DB2_MANAGEMENT_TOKEN_AUTHORITY_OK;
}

db2_management_token_authority_result_t
db2_management_token_authority_admit(db2_management_token_authority_ctx_t *ctx,
                                     const char correlation_id[65], const char jti[65],
                                     kb_mgmt_token_authority_record_t *out)
{
   return committed_call(ctx, "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", SQL_ADMIT,
                         correlation_id, jti, DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY, out);
}

db2_management_token_authority_result_t
db2_management_token_authority_readback(db2_management_token_authority_ctx_t *ctx,
                                        const char correlation_id[65], const char jti[65],
                                        kb_mgmt_token_authority_record_t *out)
{
   return committed_call(ctx, "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", SQL_READBACK,
                         correlation_id, jti, DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT, out);
}

/* Step one identity row out of `sql` and decode it. The identity counterpart of
 * row_call; separate because the record is a different shape and the namespace
 * jti must be cross-checked inside the decode rather than against the record. */
static db2_management_token_authority_result_t
identity_row_call(db2_management_token_authority_ctx_t *ctx, const char *sql,
                  const char *correlation_id, const char *jti,
                  kb_identity_token_authority_record_t *candidate,
                  db2_management_token_authority_result_t empty_result)
{
   char error[AUTHORITY_ERROR_MAX] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(ctx->connection, sql, error, sizeof(error));
   if (!st)
      return classify(NULL, error);
   int bound = aimee_pg_bind_text(st, "?1", correlation_id) || aimee_pg_bind_text(st, "?2", jti);
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && decode_identity_record(st, jti, candidate) == 0)
   {
      step = aimee_pg_step(st, error, sizeof(error));
      rc = step == AIMEE_PG_DONE
               ? DB2_MANAGEMENT_TOKEN_AUTHORITY_OK
               : (step == AIMEE_PG_ERR ? classify(aimee_pg_sqlstate(st), error)
                                       : DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY);
   }
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = empty_result;
   aimee_pg_finalize(st);
   OPENSSL_cleanse(error, sizeof(error));
   if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK || strcmp(candidate->correlation_id, correlation_id))
   {
      OPENSSL_cleanse(candidate, sizeof(*candidate));
      return rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK ? DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY
                                                     : rc;
   }
   return rc;
}

/* Run `sql` inside its own committed REPEATABLE READ transaction. Mirrors
 * committed_call, including treating a lost COMMIT acknowledgement as terminal
 * rather than retrying, because a retry could duplicate a private-key use. */
static db2_management_token_authority_result_t
identity_committed_call(db2_management_token_authority_ctx_t *ctx, const char *sql,
                        const char *correlation_id, const char *jti,
                        db2_management_token_authority_result_t empty_result,
                        kb_identity_token_authority_record_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!ctx || !ctx->connection || ctx->use_transaction_open || !out ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   if (aimee_pg_exec(ctx->connection, "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", error,
                     sizeof(error)))
      return classify(NULL, error);
   kb_identity_token_authority_record_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_token_authority_result_t rc =
       identity_row_call(ctx, sql, correlation_id, jti, &candidate, empty_result);
   if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(error, sizeof(error));
      return rc;
   }
   if (aimee_pg_exec(ctx->connection, "COMMIT", error, sizeof(error)))
   {
      aimee_pg_close(ctx->connection);
      ctx->connection = NULL;
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(error, sizeof(error));
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS;
   }
   *out = candidate;
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(error, sizeof(error));
   return DB2_MANAGEMENT_TOKEN_AUTHORITY_OK;
}

db2_management_token_authority_result_t
db2_management_identity_authority_admit(db2_management_token_authority_ctx_t *ctx,
                                        const char correlation_id[65], const char jti[65],
                                        kb_identity_token_authority_record_t *out)
{
   return identity_committed_call(ctx, SQL_IDENTITY_ADMIT, correlation_id, jti,
                                  DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY, out);
}

db2_management_token_authority_result_t
db2_management_identity_authority_readback(db2_management_token_authority_ctx_t *ctx,
                                           const char correlation_id[65], const char jti[65],
                                           kb_identity_token_authority_record_t *out)
{
   return identity_committed_call(ctx, SQL_IDENTITY_READBACK, correlation_id, jti,
                                  DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT, out);
}

/* Opens the REPEATABLE READ transaction that stays held across private-key use
 * and is closed by db2_management_identity_authority_finalize (or abort). */
db2_management_token_authority_result_t
db2_management_identity_authority_use_begin(db2_management_token_authority_ctx_t *ctx,
                                            const char correlation_id[65], const char jti[65],
                                            kb_identity_token_authority_record_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!ctx || !ctx->connection || ctx->use_transaction_open || !out ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   if (aimee_pg_exec(ctx->connection, "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", error,
                     sizeof(error)))
      return classify(NULL, error);
   kb_identity_token_authority_record_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_token_authority_result_t rc =
       identity_row_call(ctx, SQL_IDENTITY_USE, correlation_id, jti, &candidate,
                         DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY);
   if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(error, sizeof(error));
      return rc;
   }
   ctx->use_transaction_open = 1;
   ctx->use_kind = DB2_MANAGEMENT_TOKEN_INTENT_IDENTITY;
   memcpy(ctx->correlation_id, correlation_id, sizeof(ctx->correlation_id));
   memcpy(ctx->jti, jti, sizeof(ctx->jti));
   /* ctx->use_record is the management record and stays zeroed: nothing reads a
    * cached use record back, and storing identity material in a differently
    * typed field would only create a way to misread it. */
   *out = candidate;
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(error, sizeof(error));
   return DB2_MANAGEMENT_TOKEN_AUTHORITY_OK;
}

db2_management_token_authority_result_t
db2_management_identity_authority_finalize(db2_management_token_authority_ctx_t *ctx)
{
   if (!ctx || !ctx->connection || !ctx->use_transaction_open ||
       ctx->use_kind != DB2_MANAGEMENT_TOKEN_INTENT_IDENTITY)
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(ctx->connection, SQL_IDENTITY_FINALIZE, error, sizeof(error));
   int bound = !st || aimee_pg_bind_text(st, "?1", ctx->correlation_id) ||
               aimee_pg_bind_text(st, "?2", ctx->jti);
   int final = 0;
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && !boolean(st, 0, &final) && final)
   {
      step = aimee_pg_step(st, error, sizeof(error));
      rc = step == AIMEE_PG_DONE
               ? DB2_MANAGEMENT_TOKEN_AUTHORITY_OK
               : (step == AIMEE_PG_ERR ? classify(aimee_pg_sqlstate(st), error)
                                       : DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY);
   }
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   if (st)
      aimee_pg_finalize(st);

   if (rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK &&
       aimee_pg_exec(ctx->connection, "COMMIT", error, sizeof(error)))
   {
      /* Never reuse a connection whose signing linearization is ambiguous. */
      aimee_pg_close(ctx->connection);
      ctx->connection = NULL;
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS;
   }
   else if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));

   ctx->use_transaction_open = 0;
   ctx->use_kind = 0;
   OPENSSL_cleanse(ctx->correlation_id, sizeof(ctx->correlation_id));
   OPENSSL_cleanse(ctx->jti, sizeof(ctx->jti));
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}

db2_management_token_authority_result_t
db2_management_token_authority_use_begin(db2_management_token_authority_ctx_t *ctx,
                                         const char correlation_id[65], const char jti[65],
                                         kb_mgmt_token_authority_record_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!ctx || !ctx->connection || ctx->use_transaction_open || !out ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   if (aimee_pg_exec(ctx->connection, "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", error,
                     sizeof(error)))
      return classify(NULL, error);
   kb_mgmt_token_authority_record_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_token_authority_result_t rc = row_call(
       ctx, SQL_USE, correlation_id, jti, &candidate, DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY);
   if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
   {
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
      OPENSSL_cleanse(&candidate, sizeof(candidate));
      OPENSSL_cleanse(error, sizeof(error));
      return rc;
   }
   ctx->use_transaction_open = 1;
   ctx->use_kind = DB2_MANAGEMENT_TOKEN_INTENT_ACTION;
   memcpy(ctx->correlation_id, correlation_id, sizeof(ctx->correlation_id));
   memcpy(ctx->jti, jti, sizeof(ctx->jti));
   ctx->use_record = candidate;
   *out = candidate;
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(error, sizeof(error));
   return DB2_MANAGEMENT_TOKEN_AUTHORITY_OK;
}

db2_management_token_authority_result_t
db2_management_token_authority_finalize(db2_management_token_authority_ctx_t *ctx)
{
   if (!ctx || !ctx->connection || !ctx->use_transaction_open ||
       ctx->use_kind != DB2_MANAGEMENT_TOKEN_INTENT_ACTION)
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(ctx->connection, SQL_FINALIZE, error, sizeof(error));
   int bound = !st || aimee_pg_bind_text(st, "?1", ctx->correlation_id) ||
               aimee_pg_bind_text(st, "?2", ctx->jti);
   int final = 0;
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && !boolean(st, 0, &final) && final)
   {
      step = aimee_pg_step(st, error, sizeof(error));
      rc = step == AIMEE_PG_DONE
               ? DB2_MANAGEMENT_TOKEN_AUTHORITY_OK
               : (step == AIMEE_PG_ERR ? classify(aimee_pg_sqlstate(st), error)
                                       : DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY);
   }
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   if (st)
      aimee_pg_finalize(st);

   if (rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK &&
       aimee_pg_exec(ctx->connection, "COMMIT", error, sizeof(error)))
   {
      /* Never reuse a connection whose signing linearization is ambiguous. */
      aimee_pg_close(ctx->connection);
      ctx->connection = NULL;
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS;
   }
   else if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));

   ctx->use_transaction_open = 0;
   ctx->use_kind = 0;
   OPENSSL_cleanse(ctx->correlation_id, sizeof(ctx->correlation_id));
   OPENSSL_cleanse(ctx->jti, sizeof(ctx->jti));
   OPENSSL_cleanse(&ctx->use_record, sizeof(ctx->use_record));
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}

db2_management_token_authority_result_t
db2_management_token_authority_kind(db2_management_token_authority_ctx_t *ctx,
                                    const char correlation_id[65], const char jti[65],
                                    db2_management_token_intent_kind_t *kind)
{
   if (kind)
      *kind = 0;
   if (!ctx || !ctx->connection || ctx->use_transaction_open || !kind ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(ctx->connection, SQL_KIND, error, sizeof(error));
   int bound =
       !st || aimee_pg_bind_text(st, "?1", correlation_id) || aimee_pg_bind_text(st, "?2", jti);
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0))
   {
      const char *value = aimee_pg_column_text(st, 0);
      *kind = value && !strcmp(value, "action")     ? DB2_MANAGEMENT_TOKEN_INTENT_ACTION
              : value && !strcmp(value, "read")     ? DB2_MANAGEMENT_TOKEN_INTENT_READ
              : value && !strcmp(value, "identity") ? DB2_MANAGEMENT_TOKEN_INTENT_IDENTITY
                                                    : 0;
      step = aimee_pg_step(st, error, sizeof(error));
      rc = *kind && step == AIMEE_PG_DONE ? DB2_MANAGEMENT_TOKEN_AUTHORITY_OK
                                          : DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   }
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT;
   if (st)
      aimee_pg_finalize(st);
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}

db2_management_token_authority_result_t
db2_management_token_read_claim(db2_management_token_authority_ctx_t *ctx,
                                const char correlation_id[65], const char jti[65],
                                const char lease_owner[65], kb_mgmt_token_authority_record_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!ctx || !ctx->connection || ctx->use_transaction_open || !out ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64) ||
       !exact_hex_input(lease_owner, 64))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   if (aimee_pg_exec(ctx->connection, "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", error,
                     sizeof(error)))
      return classify(NULL, error);
   aimee_pg_stmt_t *st = aimee_pg_prepare(ctx->connection, SQL_READ_CLAIM, error, sizeof(error));
   int bound = !st || aimee_pg_bind_text(st, "?1", correlation_id) ||
               aimee_pg_bind_text(st, "?2", jti) || aimee_pg_bind_text(st, "?3", lease_owner);
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   kb_mgmt_token_authority_record_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && decode_record(st, &candidate) == 0)
   {
      step = aimee_pg_step(st, error, sizeof(error));
      rc = step == AIMEE_PG_DONE ? DB2_MANAGEMENT_TOKEN_AUTHORITY_OK
                                 : DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   }
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT;
   if (st)
      aimee_pg_finalize(st);
   if (rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK &&
       aimee_pg_exec(ctx->connection, "COMMIT", error, sizeof(error)))
   {
      aimee_pg_close(ctx->connection);
      ctx->connection = NULL;
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS;
   }
   else if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
   if (rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK &&
       (!candidate.newly_admitted || candidate.capability != KB_MGMT_TOKEN_CAP_REMOTE_READS ||
        strcmp(candidate.correlation_id, correlation_id) || strcmp(candidate.jti, jti)))
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   if (rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      *out = candidate;
   OPENSSL_cleanse(&candidate, sizeof(candidate));
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}

db2_management_token_authority_result_t
db2_management_token_read_finalize(db2_management_token_authority_ctx_t *ctx,
                                   const char correlation_id[65], const char jti[65],
                                   const char lease_owner[65], const char *jwt)
{
   size_t jwt_len = jwt ? strnlen(jwt, KB_MGMT_TOKEN_WIRE_MAX + 1) : 0;
   if (!ctx || !ctx->connection || ctx->use_transaction_open ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64) ||
       !exact_hex_input(lease_owner, 64) || !jwt_len || jwt_len > KB_MGMT_TOKEN_WIRE_MAX)
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   if (aimee_pg_exec(ctx->connection, "BEGIN ISOLATION LEVEL REPEATABLE READ READ WRITE", error,
                     sizeof(error)))
      return classify(NULL, error);
   aimee_pg_stmt_t *st = aimee_pg_prepare(ctx->connection, SQL_READ_FINALIZE, error, sizeof(error));
   int bound = !st || aimee_pg_bind_text(st, "?1", correlation_id) ||
               aimee_pg_bind_text(st, "?2", jti) || aimee_pg_bind_text(st, "?3", lease_owner) ||
               aimee_pg_bind_text(st, "?4", jwt);
   int final = 0;
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && !boolean(st, 0, &final) && final &&
       aimee_pg_step(st, error, sizeof(error)) == AIMEE_PG_DONE)
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_OK;
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   if (st)
      aimee_pg_finalize(st);
   if (rc == DB2_MANAGEMENT_TOKEN_AUTHORITY_OK &&
       aimee_pg_exec(ctx->connection, "COMMIT", error, sizeof(error)))
   {
      aimee_pg_close(ctx->connection);
      ctx->connection = NULL;
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_COMMIT_AMBIGUOUS;
   }
   else if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}

db2_management_token_authority_result_t
db2_management_token_read_readback(db2_management_token_authority_ctx_t *ctx,
                                   const char correlation_id[65], const char jti[65],
                                   kb_mgmt_token_authority_output_t *out)
{
   if (out)
      OPENSSL_cleanse(out, sizeof(*out));
   if (!ctx || !ctx->connection || ctx->use_transaction_open || !out ||
       !exact_hex_input(correlation_id, 64) || !exact_hex_input(jti, 64))
      return DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
   char error[AUTHORITY_ERROR_MAX] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(ctx->connection, SQL_READ_READBACK, error, sizeof(error));
   int bound =
       !st || aimee_pg_bind_text(st, "?1", correlation_id) || aimee_pg_bind_text(st, "?2", jti);
   aimee_pg_step_t step = bound ? AIMEE_PG_ERR : aimee_pg_step(st, error, sizeof(error));
   db2_management_token_authority_result_t rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_UNAVAILABLE;
   if (!bound && step == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0) &&
       !aimee_pg_column_is_null(st, 1))
   {
      const char *jwt = aimee_pg_column_text(st, 0);
      const void *stored = aimee_pg_column_blob(st, 1);
      int stored_len = aimee_pg_column_bytes(st, 1);
      size_t n = jwt ? strnlen(jwt, sizeof(out->jwt)) : 0;
      unsigned char actual[SHA256_DIGEST_LENGTH];
      if (n && n < sizeof(out->jwt) && stored && stored_len == SHA256_DIGEST_LENGTH &&
          SHA256((const unsigned char *)jwt, n, actual) &&
          CRYPTO_memcmp(actual, stored, sizeof(actual)) == 0)
      {
         memcpy(out->jwt, jwt, n + 1);
         out->jwt_len = n;
         step = aimee_pg_step(st, error, sizeof(error));
         rc = step == AIMEE_PG_DONE ? DB2_MANAGEMENT_TOKEN_AUTHORITY_OK
                                    : DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
      }
      else
         rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_INTEGRITY;
      OPENSSL_cleanse(actual, sizeof(actual));
   }
   else if (!bound && step == AIMEE_PG_ERR)
      rc = classify(aimee_pg_sqlstate(st), error);
   else if (!bound)
      rc = DB2_MANAGEMENT_TOKEN_AUTHORITY_ABSENT;
   if (st)
      aimee_pg_finalize(st);
   if (rc != DB2_MANAGEMENT_TOKEN_AUTHORITY_OK)
      OPENSSL_cleanse(out, sizeof(*out));
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}
