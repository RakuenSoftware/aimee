#include "management_jwks_publication.h"

#include "db_postgres.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static int idle(const db2_management_jwks_publication_ctx_t *ctx)
{
   return ctx && ctx->connection && ctx->barrier_lock_held && ctx->publication_lock_held &&
          !aimee_pg_in_transaction(ctx->connection);
}

static int truth(const char *value)
{
   return value && (value[0] == 't' || value[0] == '1') && value[1] == '\0';
}

static kb_mgmt_jwks_db_result_t classify(const char *error)
{
   if (error && strstr(error, "sealed"))
      return KB_MGMT_JWKS_DB_SEALED;
   if (error && (strstr(error, "invalid input") || strstr(error, "replay mismatch") ||
                 strstr(error, "inconsistent") || strstr(error, "immutable binding")))
      return KB_MGMT_JWKS_DB_INTEGRITY;
   if (error && (strstr(error, "state mismatch") || strstr(error, "binding mismatch") ||
                 strstr(error, "existing state") || strstr(error, "roots not final") ||
                 strstr(error, "partial empty") || strstr(error, "CAS not recorded") ||
                 strstr(error, "seal changed")))
      return KB_MGMT_JWKS_DB_CONFLICT;
   return KB_MGMT_JWKS_DB_RETRY;
}

static int copy_text(char *out, size_t cap, const char *in)
{
   size_t n = in ? strlen(in) : 0;
   if (!out || !cap || !n || n >= cap)
      return -1;
   memcpy(out, in, n + 1);
   return 0;
}

static int copy_blob(aimee_pg_stmt_t *stmt, int column, void *out, size_t cap, size_t *out_len,
                     size_t exact, int nullable)
{
   if (aimee_pg_column_is_null(stmt, column))
   {
      if (out_len)
         *out_len = 0;
      return nullable ? 0 : -1;
   }
   int n = aimee_pg_column_bytes(stmt, column);
   const void *value = aimee_pg_column_blob(stmt, column);
   if (!value || n < 1 || (size_t)n > cap || (exact && (size_t)n != exact))
      return -1;
   memcpy(out, value, (size_t)n);
   if (out_len)
      *out_len = (size_t)n;
   return 0;
}

static int copy_json(aimee_pg_stmt_t *stmt, int column, char *out, size_t cap, size_t *out_len)
{
   size_t n = 0;
   if (!out || cap < 2 || copy_blob(stmt, column, out, cap - 1, &n, 0, 0))
      return -1;
   out[n] = '\0';
   *out_len = n;
   return 0;
}

static int sha256_value(const void *value, size_t len, uint8_t out[32])
{
   unsigned int n = 0;
   OPENSSL_cleanse(out, 32);
   return value && len && EVP_Digest(value, len, out, &n, EVP_sha256(), NULL) == 1 && n == 32 ? 0
                                                                                              : -1;
}

static int exec_lock(db2_management_jwks_publication_ctx_t *ctx, const char *sql, char *errbuf,
                     size_t errlen)
{
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(ctx->connection, sql, errbuf, errlen);
   int ok = stmt && aimee_pg_step(stmt, errbuf, errlen) == AIMEE_PG_ROW &&
            aimee_pg_step(stmt, errbuf, errlen) == AIMEE_PG_DONE;
   if (stmt)
      aimee_pg_finalize(stmt);
   return ok ? 0 : -1;
}

static int role_assert(void *connection)
{
   static const char sql[] =
       "SELECT current_user='aimee_kb_jwks_publish' AND session_user<>current_user "
       "AND NOT r.rolcanlogin AND NOT r.rolinherit AND NOT r.rolsuper AND NOT r.rolbypassrls "
       "AND NOT r.rolcreatedb AND NOT r.rolcreaterole AND NOT r.rolreplication "
       "AND NOT s.rolsuper AND NOT s.rolbypassrls AND NOT s.rolcreatedb "
       "AND NOT s.rolcreaterole AND NOT s.rolreplication "
       "AND pg_has_role(session_user,'aimee_kb_migrate','MEMBER') "
       "AND current_setting('search_path')='pg_catalog, pg_temp' "
       "AND current_setting('row_security')='on' "
       "AND NOT has_schema_privilege('public','CREATE') "
       "FROM pg_catalog.pg_roles r CROSS JOIN pg_catalog.pg_roles s "
       "WHERE r.rolname=current_user AND s.rolname=session_user";
   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(connection, sql, error, sizeof(error));
   int ok = stmt && aimee_pg_step(stmt, error, sizeof(error)) == AIMEE_PG_ROW &&
            !aimee_pg_column_is_null(stmt, 0) && truth(aimee_pg_column_text(stmt, 0)) &&
            aimee_pg_step(stmt, error, sizeof(error)) == AIMEE_PG_DONE;
   if (stmt)
      aimee_pg_finalize(stmt);
   OPENSSL_cleanse(error, sizeof(error));
   return ok ? 0 : -1;
}

int db2_management_jwks_publication_open(db2_management_jwks_publication_ctx_t *ctx,
                                         const char *conninfo, char *errbuf, size_t errlen)
{
   static const char barrier[] =
       "SELECT pg_catalog.pg_advisory_lock_shared(-7046029254386353131::BIGINT)";
   static const char publication[] =
       "SELECT pg_catalog.pg_advisory_lock("
       "pg_catalog.hashtextextended('kb-management-jwks-publication-v1',0))";
   if (!ctx || !conninfo || !*conninfo)
      return -1;
   memset(ctx, 0, sizeof(*ctx));
   ctx->connection = aimee_pg_open(conninfo, errbuf, errlen);
   if (!ctx->connection)
      return -1;
   if (aimee_pg_exec(ctx->connection, "SET search_path = pg_catalog, pg_temp", errbuf, errlen) ||
       aimee_pg_exec(ctx->connection, "SET row_security = on", errbuf, errlen) ||
       aimee_pg_exec(ctx->connection, "SET ROLE aimee_kb_jwks_publish", errbuf, errlen) ||
       role_assert(ctx->connection) || exec_lock(ctx, barrier, errbuf, errlen))
      goto fail;
   ctx->barrier_lock_held = 1;
   if (exec_lock(ctx, publication, errbuf, errlen))
      goto fail;
   ctx->publication_lock_held = 1;
   return 0;
fail:
   if (errbuf && errlen)
      snprintf(errbuf, errlen, "management JWKS publication database session assertion failed");
   aimee_pg_close(ctx->connection); /* Session close is the fail-safe unlock. */
   OPENSSL_cleanse(ctx, sizeof(*ctx));
   return -1;
}

static void unlock(db2_management_jwks_publication_ctx_t *ctx, const char *sql)
{
   char error[128] = "";
   if (ctx && ctx->connection)
   {
      aimee_pg_stmt_t *stmt = aimee_pg_prepare(ctx->connection, sql, error, sizeof(error));
      if (stmt)
      {
         (void)aimee_pg_step(stmt, error, sizeof(error));
         aimee_pg_finalize(stmt);
      }
   }
   OPENSSL_cleanse(error, sizeof(error));
}

void db2_management_jwks_publication_close(db2_management_jwks_publication_ctx_t *ctx)
{
   if (!ctx)
      return;
   if (ctx->connection && aimee_pg_in_transaction(ctx->connection))
   {
      char error[128] = "";
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", error, sizeof(error));
      OPENSSL_cleanse(error, sizeof(error));
   }
   if (ctx->publication_lock_held)
      unlock(ctx, "SELECT pg_catalog.pg_advisory_unlock("
                  "pg_catalog.hashtextextended('kb-management-jwks-publication-v1',0))");
   if (ctx->barrier_lock_held)
      unlock(ctx, "SELECT pg_catalog.pg_advisory_unlock_shared(-7046029254386353131::BIGINT)");
   if (ctx->connection)
      aimee_pg_close(ctx->connection);
   OPENSSL_cleanse(ctx, sizeof(*ctx));
}

int db2_management_jwks_publication_set_provider_binding(db2_management_jwks_publication_ctx_t *ctx,
                                                         const char *helper,
                                                         const char *verifier_domain,
                                                         const uint8_t identity_digest[32])
{
   if (!idle(ctx) || ctx->snapshot_valid || !helper || !verifier_domain || !identity_digest ||
       !helper[0] || strlen(helper) > 128 || !verifier_domain[0] || strlen(verifier_domain) > 128)
      return -1;
   snprintf(ctx->provider_helper, sizeof(ctx->provider_helper), "%s", helper);
   snprintf(ctx->provider_verifier_domain, sizeof(ctx->provider_verifier_domain), "%s",
            verifier_domain);
   memcpy(ctx->provider_identity_digest, identity_digest, 32);
   ctx->provider_binding_set = 1;
   return 0;
}

static kb_mgmt_jwks_db_result_t inspect_roots(db2_management_jwks_publication_ctx_t *ctx,
                                              kb_mgmt_jwks_roots_t *roots)
{
   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_jwks_publication_roots()", error,
       sizeof(error));
   if (!stmt)
      return classify(error);
   aimee_pg_step_t step = aimee_pg_step(stmt, error, sizeof(error));
   int64_t epoch = step == AIMEE_PG_ROW ? aimee_pg_column_int64(stmt, 0) : 0;
   uint8_t exponent[3] = {0};
   uint8_t token_hwm_digest[32] = {0};
   memset(roots, 0, sizeof(*roots));
   int ok = step == AIMEE_PG_ROW && epoch > 0;
   if (ok)
   {
      roots->token.kind = KB_MGMT_ROOT_TOKEN;
      roots->token.phase = KB_MGMT_ROOT_FINAL;
      roots->token.seal_epoch = (uint64_t)epoch;
      roots->token.v2.version = 2;
      roots->manifest.kind = KB_MGMT_ROOT_MANIFEST;
      roots->manifest.phase = KB_MGMT_ROOT_FINAL;
      roots->manifest.seal_epoch = (uint64_t)epoch;
      roots->manifest.v2.version = 2;
      roots->publication.bound = 1;
      ok =
          !copy_text(roots->token.wire_id, sizeof(roots->token.wire_id),
                     aimee_pg_column_text(stmt, 1)) &&
          !copy_blob(stmt, 2, roots->token.public_key, sizeof(roots->token.public_key),
                     &roots->token.public_key_len, KB_MGMT_TOKEN_MODULUS_LEN, 0) &&
          !copy_blob(stmt, 3, exponent, sizeof(exponent), NULL, 3, 0) && exponent[0] == 1 &&
          exponent[1] == 0 && exponent[2] == 1 &&
          !copy_blob(stmt, 4, roots->token.public_digest, 32, NULL, 32, 0) &&
          !copy_blob(stmt, 5, roots->token.jwk_digest, 32, NULL, 32, 0) &&
          aimee_pg_column_int64(stmt, 6) == 2 &&
          !copy_blob(stmt, 7, token_hwm_digest, sizeof(token_hwm_digest), NULL, 32, 0) &&
          !copy_text(roots->manifest.custody_key_id, sizeof(roots->manifest.custody_key_id),
                     aimee_pg_column_text(stmt, 8)) &&
          !copy_text(roots->manifest.wire_id, sizeof(roots->manifest.wire_id),
                     aimee_pg_column_text(stmt, 9)) &&
          !copy_blob(stmt, 10, roots->manifest.public_key, sizeof(roots->manifest.public_key),
                     &roots->manifest.public_key_len, 32, 0) &&
          !copy_blob(stmt, 11, roots->manifest.public_digest, 32, NULL, 32, 0) &&
          aimee_pg_column_int64(stmt, 12) == 2 &&
          !copy_blob(stmt, 13, roots->manifest.v2.wrapped_dek,
                     sizeof(roots->manifest.v2.wrapped_dek), NULL,
                     sizeof(roots->manifest.v2.wrapped_dek), 0) &&
          !copy_blob(stmt, 14, roots->manifest.v2.nonce, sizeof(roots->manifest.v2.nonce), NULL,
                     sizeof(roots->manifest.v2.nonce), 0) &&
          !copy_blob(stmt, 15, roots->manifest.v2.ciphertext, sizeof(roots->manifest.v2.ciphertext),
                     &roots->manifest.v2.ciphertext_len, 32, 0) &&
          !copy_blob(stmt, 16, roots->manifest.v2.tag, sizeof(roots->manifest.v2.tag), NULL,
                     sizeof(roots->manifest.v2.tag), 0) &&
          !copy_blob(stmt, 17, roots->manifest.hwm2_attestation,
                     sizeof(roots->manifest.hwm2_attestation),
                     &roots->manifest.hwm2_attestation_len, 0, 0) &&
          !copy_text(roots->publication.custody_key_id, sizeof(roots->publication.custody_key_id),
                     aimee_pg_column_text(stmt, 18)) &&
          !copy_text(roots->publication.helper, sizeof(roots->publication.helper),
                     aimee_pg_column_text(stmt, 19)) &&
          !copy_text(roots->publication.verifier_domain, sizeof(roots->publication.verifier_domain),
                     aimee_pg_column_text(stmt, 20)) &&
          !copy_blob(stmt, 21, roots->publication.identity_digest, 32, NULL, 32, 0) &&
          !copy_blob(stmt, 22, roots->publication.hwm1_attestation,
                     sizeof(roots->publication.hwm1_attestation),
                     &roots->publication.hwm1_attestation_len, 0, 0) &&
          aimee_pg_step(stmt, error, sizeof(error)) == AIMEE_PG_DONE;
   }
   aimee_pg_finalize(stmt);
   OPENSSL_cleanse(token_hwm_digest, sizeof(token_hwm_digest));
   if (!ok)
   {
      OPENSSL_cleanse(roots, sizeof(*roots));
      return step == AIMEE_PG_ROW ? KB_MGMT_JWKS_DB_INTEGRITY : classify(error);
   }
   return KB_MGMT_JWKS_DB_OK;
}

static kb_mgmt_jwks_db_result_t inspect(void *opaque, kb_mgmt_jwks_roots_t *roots,
                                        kb_mgmt_jwks_record_t *record)
{
   db2_management_jwks_publication_ctx_t *ctx = opaque;
   if (!idle(ctx) || !ctx->provider_binding_set || !roots || !record)
      return KB_MGMT_JWKS_DB_INTEGRITY;
   ctx->snapshot_valid = 0;
   OPENSSL_cleanse(&ctx->roots, sizeof(ctx->roots));
   kb_mgmt_jwks_db_result_t rc = inspect_roots(ctx, roots);
   if (rc != KB_MGMT_JWKS_DB_OK)
      return rc;
   if (strcmp(roots->publication.helper, ctx->provider_helper) ||
       strcmp(roots->publication.verifier_domain, ctx->provider_verifier_domain) ||
       CRYPTO_memcmp(roots->publication.identity_digest, ctx->provider_identity_digest, 32))
   {
      OPENSSL_cleanse(roots, sizeof(*roots));
      return KB_MGMT_JWKS_DB_INTEGRITY;
   }
   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_jwks_publication_inspect()", error,
       sizeof(error));
   if (!stmt)
      return classify(error);
   aimee_pg_step_t step = aimee_pg_step(stmt, error, sizeof(error));
   memset(record, 0, sizeof(*record));
   const char *phase = step == AIMEE_PG_ROW ? aimee_pg_column_text(stmt, 0) : NULL;
   int64_t generation = step == AIMEE_PG_ROW ? aimee_pg_column_int64(stmt, 1) : 0;
   int64_t epoch = step == AIMEE_PG_ROW ? aimee_pg_column_int64(stmt, 11) : 0;
   int ok = step == AIMEE_PG_ROW && generation == 1 && epoch > 0;
   if (ok && phase && strcmp(phase, "empty") == 0)
   {
      record->phase = KB_MGMT_JWKS_EMPTY;
      record->generation = 1;
      record->seal_epoch = (uint64_t)epoch;
   }
   else if (ok)
   {
      record->phase = phase && strcmp(phase, "staged") == 0     ? KB_MGMT_JWKS_STAGED
                      : phase && strcmp(phase, "cas_done") == 0 ? KB_MGMT_JWKS_CAS_DONE
                      : phase && strcmp(phase, "final") == 0    ? KB_MGMT_JWKS_FINAL
                                                                : KB_MGMT_JWKS_EMPTY;
      record->generation = 1;
      record->valid_from = aimee_pg_column_int64(stmt, 3);
      record->valid_until = aimee_pg_column_int64(stmt, 4);
      record->seal_epoch = (uint64_t)epoch;
      ok = record->phase != KB_MGMT_JWKS_EMPTY && record->valid_from >= 0 &&
           record->valid_until > record->valid_from &&
           !copy_text(record->candidate_id, sizeof(record->candidate_id),
                      aimee_pg_column_text(stmt, 2)) &&
           !copy_json(stmt, 5, record->jwks, sizeof(record->jwks), &record->jwks_len) &&
           !copy_json(stmt, 6, record->payload, sizeof(record->payload), &record->payload_len) &&
           !copy_json(stmt, 7, record->envelope, sizeof(record->envelope), &record->envelope_len) &&
           !copy_blob(stmt, 8, record->manifest_digest, 32, NULL, 32, 0) &&
           !copy_blob(stmt, 9, record->signature, sizeof(record->signature), NULL,
                      sizeof(record->signature), 0) &&
           !copy_blob(stmt, 10, record->hwm2_attestation_digest,
                      sizeof(record->hwm2_attestation_digest), NULL, 32,
                      record->phase == KB_MGMT_JWKS_STAGED) &&
           !sha256_value(record->jwks, record->jwks_len, record->jwks_digest) &&
           !sha256_value(record->payload, record->payload_len, record->payload_digest) &&
           !sha256_value(record->envelope, record->envelope_len, record->envelope_digest);
      snprintf(record->manifest_id, sizeof(record->manifest_id), "%s", roots->manifest.wire_id);
      memcpy(record->token_public_digest, roots->token.public_digest, 32);
      memcpy(record->token_jwk_digest, roots->token.jwk_digest, 32);
      memcpy(record->manifest_public_digest, roots->manifest.public_digest, 32);
      memcpy(record->publication_identity_digest, roots->publication.identity_digest, 32);
      memcpy(record->hwm1_attestation, roots->publication.hwm1_attestation,
             roots->publication.hwm1_attestation_len);
      record->hwm1_attestation_len = roots->publication.hwm1_attestation_len;
   }
   if (ok)
      ok = aimee_pg_step(stmt, error, sizeof(error)) == AIMEE_PG_DONE;
   aimee_pg_finalize(stmt);
   if (!ok)
   {
      OPENSSL_cleanse(record, sizeof(*record));
      OPENSSL_cleanse(roots, sizeof(*roots));
      return step == AIMEE_PG_ROW ? KB_MGMT_JWKS_DB_INTEGRITY : classify(error);
   }
   memcpy(&ctx->roots, roots, sizeof(*roots));
   ctx->snapshot_valid = 1;
   return KB_MGMT_JWKS_DB_OK;
}

static kb_mgmt_jwks_db_result_t execute_void(aimee_pg_stmt_t *stmt, char error[256])
{
   if (!stmt)
      return classify(error);
   int ok = aimee_pg_step(stmt, error, 256) == AIMEE_PG_ROW;
   aimee_pg_finalize(stmt);
   return ok ? KB_MGMT_JWKS_DB_OK : classify(error);
}

static kb_mgmt_jwks_db_result_t stage(void *opaque, const kb_mgmt_jwks_record_t *record)
{
   db2_management_jwks_publication_ctx_t *ctx = opaque;
   if (!idle(ctx) || !ctx->snapshot_valid || !record || record->phase != KB_MGMT_JWKS_STAGED ||
       record->generation != 1 || !record->candidate_id[0] || !record->jwks_len ||
       !record->payload_len || !record->envelope_len || !record->hwm1_attestation_len)
      return KB_MGMT_JWKS_DB_INTEGRITY;
   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       ctx->connection,
       "SELECT public.kb_management_jwks_publication_stage(?1,?2,?3,?4,?5,?6,?7,?8,?9,"
       "?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21)",
       error, sizeof(error));
   if (!stmt)
      return classify(error);
   uint8_t previous[32] = {0};
#define BI(n, v)    aimee_pg_bind_int64(stmt, "?" #n, (int64_t)(v))
#define BT(n, v)    aimee_pg_bind_text(stmt, "?" #n, (v))
#define BB(n, v, z) aimee_pg_bind_blob(stmt, "?" #n, (v), (int)(z))
   BI(1, record->generation);
   BT(2, record->candidate_id);
   BI(3, record->valid_from);
   BI(4, record->valid_until);
   BB(5, previous, sizeof(previous));
   BB(6, record->jwks, record->jwks_len);
   BB(7, record->jwks_digest, 32);
   BB(8, record->payload, record->payload_len);
   BB(9, record->payload_digest, 32);
   BB(10, record->envelope, record->envelope_len);
   BB(11, record->envelope_digest, 32);
   BB(12, record->manifest_digest, 32);
   BB(13, record->signature, sizeof(record->signature));
   BT(14, ctx->roots.token.wire_id);
   BB(15, record->token_public_digest, 32);
   BB(16, record->token_jwk_digest, 32);
   BT(17, record->manifest_id);
   BB(18, record->manifest_public_digest, 32);
   BB(19, record->publication_identity_digest, 32);
   BB(20, record->hwm1_attestation, record->hwm1_attestation_len);
   BI(21, record->seal_epoch);
#undef BI
#undef BT
#undef BB
   return execute_void(stmt, error);
}

static kb_mgmt_jwks_db_result_t record_cas(void *opaque, const kb_mgmt_jwks_record_t *record,
                                           const uint8_t *attestation, size_t attestation_len)
{
   db2_management_jwks_publication_ctx_t *ctx = opaque;
   if (!idle(ctx) || !record || record->generation != 1 || !record->candidate_id[0] ||
       !attestation || !attestation_len || attestation_len > KB_MGMT_ROOT_ATTEST_MAX)
      return KB_MGMT_JWKS_DB_INTEGRITY;
   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       ctx->connection, "SELECT public.kb_management_jwks_publication_record_cas(?1,?2,?3)", error,
       sizeof(error));
   if (!stmt)
      return classify(error);
   aimee_pg_bind_int64(stmt, "?1", (int64_t)record->generation);
   aimee_pg_bind_text(stmt, "?2", record->candidate_id);
   aimee_pg_bind_blob(stmt, "?3", attestation, (int)attestation_len);
   return execute_void(stmt, error);
}

static kb_mgmt_jwks_db_result_t finalize(void *opaque, const kb_mgmt_jwks_record_t *record)
{
   db2_management_jwks_publication_ctx_t *ctx = opaque;
   if (!idle(ctx) || !record || record->generation != 1 || !record->candidate_id[0])
      return KB_MGMT_JWKS_DB_INTEGRITY;
   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       ctx->connection, "SELECT public.kb_management_jwks_publication_finalize(?1,?2)", error,
       sizeof(error));
   if (!stmt)
      return classify(error);
   aimee_pg_bind_int64(stmt, "?1", (int64_t)record->generation);
   aimee_pg_bind_text(stmt, "?2", record->candidate_id);
   return execute_void(stmt, error);
}

kb_mgmt_jwks_db_result_t db2_management_jwks_manifest_key_admit(
    db2_management_jwks_publication_ctx_t *ctx, const char *use_id, uint64_t generation,
    const char *candidate_id, const kb_mgmt_root_record_t *manifest,
    const uint8_t payload_digest[32], db2_management_jwks_admission_t *out)
{
   if (!idle(ctx) || !use_id || strlen(use_id) != 64 || generation != 1 || !candidate_id ||
       strlen(candidate_id) != 64 || !manifest || manifest->kind != KB_MGMT_ROOT_MANIFEST ||
       manifest->phase != KB_MGMT_ROOT_FINAL || !manifest->custody_key_id[0] ||
       !manifest->wire_id[0] || !manifest->hwm2_attestation_len || !payload_digest || !out)
      return KB_MGMT_JWKS_DB_INTEGRITY;
   OPENSSL_cleanse(out, sizeof(*out));
   char error[256] = "";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(
       ctx->connection,
       "SELECT * FROM public.kb_management_jwks_manifest_key_admit(?1,?2,?3,?4,?5,?6,?7)", error,
       sizeof(error));
   if (!stmt)
      return classify(error);
   aimee_pg_bind_text(stmt, "?1", use_id);
   aimee_pg_bind_int64(stmt, "?2", (int64_t)generation);
   aimee_pg_bind_text(stmt, "?3", candidate_id);
   aimee_pg_bind_text(stmt, "?4", manifest->custody_key_id);
   aimee_pg_bind_text(stmt, "?5", manifest->wire_id);
   aimee_pg_bind_blob(stmt, "?6", payload_digest, 32);
   aimee_pg_bind_blob(stmt, "?7", manifest->hwm2_attestation, (int)manifest->hwm2_attestation_len);
   aimee_pg_step_t step = aimee_pg_step(stmt, error, sizeof(error));
   int64_t epoch = step == AIMEE_PG_ROW ? aimee_pg_column_int64(stmt, 1) : 0;
   int ok = step == AIMEE_PG_ROW && epoch > 0;
   if (ok)
   {
      out->newly_admitted = truth(aimee_pg_column_text(stmt, 0));
      out->seal_epoch = (uint64_t)epoch;
      out->envelope.version = 2;
      ok = !copy_blob(stmt, 2, out->envelope.wrapped_dek, sizeof(out->envelope.wrapped_dek), NULL,
                      sizeof(out->envelope.wrapped_dek), 0) &&
           !copy_blob(stmt, 3, out->envelope.nonce, sizeof(out->envelope.nonce), NULL,
                      sizeof(out->envelope.nonce), 0) &&
           !copy_blob(stmt, 4, out->envelope.ciphertext, sizeof(out->envelope.ciphertext),
                      &out->envelope.ciphertext_len, 32, 0) &&
           !copy_blob(stmt, 5, out->envelope.tag, sizeof(out->envelope.tag), NULL,
                      sizeof(out->envelope.tag), 0) &&
           !copy_blob(stmt, 6, out->hwm_attestation, sizeof(out->hwm_attestation),
                      &out->hwm_attestation_len, 0, 0) &&
           aimee_pg_step(stmt, error, sizeof(error)) == AIMEE_PG_DONE;
   }
   aimee_pg_finalize(stmt);
   if (!ok)
   {
      OPENSSL_cleanse(out, sizeof(*out));
      return step == AIMEE_PG_ROW ? KB_MGMT_JWKS_DB_INTEGRITY : classify(error);
   }
   return KB_MGMT_JWKS_DB_OK;
}

int db2_management_jwks_publication_bind(db2_management_jwks_publication_ctx_t *ctx,
                                         kb_mgmt_jwks_callbacks_t *callbacks)
{
   if (!idle(ctx) || !callbacks)
      return -1;
   memset(callbacks, 0, sizeof(*callbacks));
   callbacks->inspect = inspect;
   callbacks->stage = stage;
   callbacks->record_cas = record_cas;
   callbacks->finalize = finalize;
   callbacks->ctx = ctx;
   return 0;
}
