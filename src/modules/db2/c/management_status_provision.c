#include "management_status_provision.h"

#include "db_postgres.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

static int idle(const db2_management_status_provision_ctx_t *ctx)
{
   return ctx && ctx->connection && !aimee_pg_in_transaction(ctx->connection);
}

static int bool_text(const char *value)
{
   return value && ((value[0] == 't' || value[0] == '1') && value[1] == '\0');
}

static int provision_role_assert(void *connection, char *errbuf, size_t errlen)
{
   static const char sql[] = "SELECT (current_user='aimee_kb_migrate' "
                             "AND session_user<>current_user "
                             "AND NOT e.rolcanlogin AND NOT e.rolsuper AND NOT e.rolbypassrls "
                             "AND NOT s.rolsuper AND NOT s.rolbypassrls AND NOT s.rolcreatedb "
                             "AND NOT s.rolcreaterole AND NOT s.rolreplication "
                             "AND pg_catalog.pg_has_role(session_user,'aimee_kb_migrate','MEMBER') "
                             "AND current_setting('search_path')='pg_catalog, pg_temp' "
                             "AND current_setting('row_security')='on') AS provision_role_ok "
                             "FROM pg_catalog.pg_roles e CROSS JOIN pg_catalog.pg_roles s "
                             "WHERE e.rolname=current_user AND s.rolname=session_user";
   aimee_pg_stmt_t *stmt = aimee_pg_prepare(connection, sql, errbuf, errlen);
   if (!stmt)
      return -1;
   int ok = aimee_pg_step(stmt, errbuf, errlen) == AIMEE_PG_ROW &&
            !aimee_pg_column_is_null(stmt, 0) && bool_text(aimee_pg_column_text(stmt, 0)) &&
            aimee_pg_step(stmt, errbuf, errlen) == AIMEE_PG_DONE;
   aimee_pg_finalize(stmt);
   return ok ? 0 : -1;
}

static int hex64(const char *s)
{
   if (!s || strlen(s) != 64)
      return 0;
   for (size_t i = 0; i < 64; ++i)
      if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
         return 0;
   return 1;
}

static int copy_text(char *out, size_t cap, const char *s)
{
   size_t n = s ? strlen(s) : 0;
   if (!out || !cap || n >= cap)
      return -1;
   memcpy(out, s, n + 1);
   return 0;
}

static int copy_blob(aimee_pg_stmt_t *s, int col, uint8_t *out, size_t exact, int nullable)
{
   if (aimee_pg_column_is_null(s, col))
      return nullable ? 0 : -1;
   const void *p = aimee_pg_column_blob(s, col);
   int n = aimee_pg_column_bytes(s, col);
   if (!p || n < 0 || (size_t)n != exact)
      return -1;
   memcpy(out, p, exact);
   return 0;
}

int db2_management_status_provision_open(db2_management_status_provision_ctx_t *ctx,
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
       provision_role_assert(ctx->connection, errbuf, errlen))
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "management status provisioner database role assertion failed");
      aimee_pg_close(ctx->connection);
      memset(ctx, 0, sizeof(*ctx));
      return -1;
   }
   return 0;
}

void db2_management_status_provision_close(db2_management_status_provision_ctx_t *ctx)
{
   if (!ctx)
      return;
   if (ctx->connection && aimee_pg_in_transaction(ctx->connection))
   {
      char err[128] = "";
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", err, sizeof(err));
   }
   if (ctx->connection)
      aimee_pg_close(ctx->connection);
   memset(ctx, 0, sizeof(*ctx));
}

int db2_management_status_provision_bootstrap_id(const char *custody_key_id, char out[65])
{
   static const char domain[] = "aimee-p5-status-bootstrap-v1|";
   if (!custody_key_id || !*custody_key_id || strlen(custody_key_id) > 600 || !out)
      return -1;
   EVP_MD_CTX *md = EVP_MD_CTX_new();
   uint8_t digest[32];
   unsigned int n = 0;
   int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1 &&
            EVP_DigestUpdate(md, domain, sizeof(domain) - 1) == 1 &&
            EVP_DigestUpdate(md, custody_key_id, strlen(custody_key_id)) == 1 &&
            EVP_DigestFinal_ex(md, digest, &n) == 1 && n == sizeof(digest);
   EVP_MD_CTX_free(md);
   if (!ok)
   {
      memset(digest, 0, sizeof(digest));
      return -1;
   }
   for (size_t i = 0; i < sizeof(digest); ++i)
      (void)snprintf(out + i * 2, 3, "%02x", digest[i]);
   memset(digest, 0, sizeof(digest));
   return 0;
}

static int valid_record(const db2_management_status_provision_record_t *r)
{
   return r && hex64(r->bootstrap_id) && r->custody_key_id[0] && strlen(r->custody_key_id) <= 600 &&
          r->wire_key_id[0] && strlen(r->wire_key_id) <= 64 && r->v1.hwm_attestation_len == 64 &&
          r->v1.ciphertext_len == 32 && r->v2.ciphertext_len == 32;
}

int db2_management_status_provision_stage(db2_management_status_provision_ctx_t *ctx,
                                          const db2_management_status_provision_record_t *r,
                                          int64_t *rotation_id, int64_t *seal_epoch)
{
   if (!idle(ctx) || !valid_record(r) || !rotation_id || !seal_epoch)
      return -1;
   *rotation_id = 0;
   *seal_epoch = 0;
   char err[256] = "";
   aimee_pg_stmt_t *s =
       aimee_pg_prepare(ctx->connection,
                        "SELECT * FROM "
                        "public.kb_management_status_key_bootstrap_stage(?1,?2,?3,?4,?5,?"
                        "6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)",
                        err, sizeof(err));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", r->bootstrap_id);
   aimee_pg_bind_text(s, "?2", r->custody_key_id);
   aimee_pg_bind_text(s, "?3", r->wire_key_id);
   aimee_pg_bind_blob(s, "?4", r->public_key, sizeof(r->public_key));
   aimee_pg_bind_blob(s, "?5", r->v1.hwm_attestation, (int)r->v1.hwm_attestation_len);
   aimee_pg_bind_blob(s, "?6", r->v1.wrapped_dek, sizeof(r->v1.wrapped_dek));
   aimee_pg_bind_blob(s, "?7", r->v1.nonce, sizeof(r->v1.nonce));
   aimee_pg_bind_blob(s, "?8", r->v1.ciphertext, (int)r->v1.ciphertext_len);
   aimee_pg_bind_blob(s, "?9", r->v1.tag, sizeof(r->v1.tag));
   aimee_pg_bind_blob(s, "?10", r->v2.wrapped_dek, sizeof(r->v2.wrapped_dek));
   aimee_pg_bind_blob(s, "?11", r->v2.nonce, sizeof(r->v2.nonce));
   aimee_pg_bind_blob(s, "?12", r->v2.ciphertext, (int)r->v2.ciphertext_len);
   aimee_pg_bind_blob(s, "?13", r->v2.tag, sizeof(r->v2.tag));
   aimee_pg_bind_blob(s, "?14", r->public_key_digest, sizeof(r->public_key_digest));
   aimee_pg_bind_blob(s, "?15", r->v1_envelope_digest, sizeof(r->v1_envelope_digest));
   aimee_pg_bind_blob(s, "?16", r->v2_envelope_digest, sizeof(r->v2_envelope_digest));
   int ok = aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW && !aimee_pg_column_is_null(s, 0) &&
            !aimee_pg_column_is_null(s, 2);
   if (ok)
   {
      *rotation_id = aimee_pg_column_int64(s, 0);
      *seal_epoch = aimee_pg_column_int64(s, 2);
      ok = *rotation_id > 0 && *seal_epoch > 0;
   }
   aimee_pg_finalize(s);
   return ok ? 0 : -1;
}

int db2_management_status_provision_resume(db2_management_status_provision_ctx_t *ctx,
                                           const char *bootstrap_id, const char *custody_key_id,
                                           db2_management_status_provision_record_t *out)
{
   if (!idle(ctx) || !hex64(bootstrap_id) || !custody_key_id || !*custody_key_id ||
       strlen(custody_key_id) > 600 || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   char err[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_status_key_bootstrap_resume(?1,?2)",
       err, sizeof(err));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", bootstrap_id);
   aimee_pg_bind_text(s, "?2", custody_key_id);
   int ok = aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW;
   if (ok)
   {
      ok = !copy_text(out->bootstrap_id, sizeof(out->bootstrap_id), bootstrap_id) &&
           !copy_text(out->custody_key_id, sizeof(out->custody_key_id), custody_key_id) &&
           !copy_text(out->state, sizeof(out->state), aimee_pg_column_text(s, 4));
      if (ok && strcmp(out->state, "empty") == 0)
      {
         out->from_version = 1;
         out->to_version = 2;
         out->seal_epoch = aimee_pg_column_int64(s, 14);
         ok = out->seal_epoch > 0;
      }
      else if (ok)
      {
         ok = !copy_text(out->custody_key_id, sizeof(out->custody_key_id),
                         aimee_pg_column_text(s, 0)) &&
              !copy_text(out->wire_key_id, sizeof(out->wire_key_id), aimee_pg_column_text(s, 1)) &&
              !copy_blob(s, 2, out->public_key, sizeof(out->public_key), 0) &&
              !copy_blob(s, 7, out->v2.wrapped_dek, sizeof(out->v2.wrapped_dek), 0) &&
              !copy_blob(s, 8, out->v2.nonce, sizeof(out->v2.nonce), 0) &&
              !copy_blob(s, 9, out->v2.ciphertext, 32, 0) &&
              !copy_blob(s, 10, out->v2.tag, sizeof(out->v2.tag), 0) &&
              !copy_blob(s, 11, out->v1.hwm_attestation, 64, 0) &&
              !copy_blob(s, 12, out->v2.hwm_attestation, 64, 1) &&
              !copy_blob(s, 15, out->public_key_digest, 32, 0) &&
              !copy_blob(s, 16, out->v1_envelope_digest, 32, 0) &&
              !copy_blob(s, 17, out->v2_envelope_digest, 32, 0) &&
              !copy_blob(s, 18, out->v1.wrapped_dek, sizeof(out->v1.wrapped_dek), 0) &&
              !copy_blob(s, 19, out->v1.nonce, sizeof(out->v1.nonce), 0) &&
              !copy_blob(s, 20, out->v1.ciphertext, 32, 0) &&
              !copy_blob(s, 21, out->v1.tag, sizeof(out->v1.tag), 0);
         if (ok)
         {
            out->rotation_id = aimee_pg_column_int64(s, 3);
            out->from_version = aimee_pg_column_int64(s, 5);
            out->to_version = aimee_pg_column_int64(s, 6);
            out->v1.version = 1;
            out->v1.ciphertext_len = 32;
            out->v1.hwm_attestation_len = 64;
            out->v2.version = 2;
            out->v2.ciphertext_len = 32;
            if (!aimee_pg_column_is_null(s, 12))
               out->v2.hwm_attestation_len = 64;
            const char *enabled = aimee_pg_column_text(s, 13);
            out->enabled = enabled && (enabled[0] == 't' || enabled[0] == '1');
            out->seal_epoch = aimee_pg_column_int64(s, 14);
            ok = out->rotation_id > 0 && out->seal_epoch > 0;
         }
      }
   }
   aimee_pg_finalize(s);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok ? 0 : -1;
}

int db2_management_status_provision_inspect(db2_management_status_provision_ctx_t *ctx,
                                            const char *custody_key_id,
                                            db2_management_status_provision_record_t *out)
{
   char bootstrap_id[65];
   if (db2_management_status_provision_bootstrap_id(custody_key_id, bootstrap_id) != 0)
      return -1;
   return db2_management_status_provision_resume(ctx, bootstrap_id, custody_key_id, out);
}

int db2_management_status_provision_prepare_activation(db2_management_status_provision_ctx_t *ctx,
                                                       const char *bootstrap_id,
                                                       int64_t *rotation_id,
                                                       int64_t *expected_version,
                                                       int64_t *next_version)
{
   if (!idle(ctx) || !hex64(bootstrap_id) || !rotation_id || !expected_version || !next_version)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection,
       "SELECT * FROM public.kb_management_status_key_bootstrap_prepare_activation(?1)", err,
       sizeof(err));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", bootstrap_id);
   int ok = aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW;
   if (ok)
   {
      *rotation_id = aimee_pg_column_int64(s, 0);
      *expected_version = aimee_pg_column_int64(s, 2);
      *next_version = aimee_pg_column_int64(s, 3);
      ok = *rotation_id > 0 && *expected_version == 1 && *next_version == 2;
   }
   aimee_pg_finalize(s);
   return ok ? 0 : -1;
}

int db2_management_status_provision_finalize(db2_management_status_provision_ctx_t *ctx,
                                             const char *bootstrap_id,
                                             const uint8_t hwm2_attestation[64],
                                             db2_management_status_provision_record_t *out)
{
   if (!idle(ctx) || !hex64(bootstrap_id) || !hwm2_attestation || !out)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_status_key_bootstrap_finalize(?1,?2)",
       err, sizeof(err));
   if (!s)
      return -1;
   aimee_pg_bind_text(s, "?1", bootstrap_id);
   aimee_pg_bind_blob(s, "?2", hwm2_attestation, 64);
   int ok = aimee_pg_step(s, err, sizeof(err)) == AIMEE_PG_ROW;
   char custody_key_id[601] = "";
   if (ok)
      ok = copy_text(custody_key_id, sizeof(custody_key_id), aimee_pg_column_text(s, 0)) == 0;
   aimee_pg_finalize(s);
   if (!ok)
      return -1;
   return db2_management_status_provision_resume(ctx, bootstrap_id, custody_key_id, out);
}
