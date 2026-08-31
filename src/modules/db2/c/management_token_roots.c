#include "management_token_roots.h"

#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

static int idle(const db2_management_token_roots_ctx_t *ctx)
{
   return ctx && ctx->connection && ctx->session_lock_held &&
          !aimee_pg_in_transaction(ctx->connection);
}

static int session_lock_acquire(db2_management_token_roots_ctx_t *ctx, char *errbuf, size_t errlen)
{
   static const char sql[] = "SELECT pg_catalog.pg_advisory_lock("
                             "pg_catalog.hashtextextended('kb-management-token-roots-v1',0))";
   aimee_pg_stmt_t *s = aimee_pg_prepare(ctx->connection, sql, errbuf, errlen);
   int ok = s && aimee_pg_step(s, errbuf, errlen) == AIMEE_PG_ROW &&
            aimee_pg_step(s, errbuf, errlen) == AIMEE_PG_DONE;
   if (s)
      aimee_pg_finalize(s);
   if (ok)
      ctx->session_lock_held = 1;
   return ok ? 0 : -1;
}

static void session_lock_release(db2_management_token_roots_ctx_t *ctx)
{
   if (!ctx || !ctx->connection || !ctx->session_lock_held)
      return;
   char e[128] = "";
   static const char sql[] = "SELECT pg_catalog.pg_advisory_unlock("
                             "pg_catalog.hashtextextended('kb-management-token-roots-v1',0))";
   aimee_pg_stmt_t *s = aimee_pg_prepare(ctx->connection, sql, e, sizeof(e));
   if (s)
   {
      (void)aimee_pg_step(s, e, sizeof(e));
      aimee_pg_finalize(s);
   }
   /* Closing the connection below is the fail-safe unlock path. */
   ctx->session_lock_held = 0;
}

static int truth(const char *s)
{
   return s && (s[0] == 't' || s[0] == '1') && s[1] == '\0';
}

static const char *kind_name(kb_mgmt_root_kind_t kind)
{
   return kind == KB_MGMT_ROOT_TOKEN ? "token" : kind == KB_MGMT_ROOT_MANIFEST ? "manifest" : NULL;
}

static kb_mgmt_root_db_result_t classify(const char *e)
{
   if (e && strstr(e, "sealed"))
      return KB_MGMT_ROOT_DB_SEALED;
   if (e &&
       (strstr(e, "invalid input") || strstr(e, "replay mismatch") ||
        strstr(e, "binding mismatch") || strstr(e, "inconsistent") || strstr(e, "partial empty")))
      return KB_MGMT_ROOT_DB_INTEGRITY;
   if (e && (strstr(e, "state mismatch") || strstr(e, "existing state") ||
             strstr(e, "roots not final") || strstr(e, "CAS not recorded") ||
             strstr(e, "current mismatch") || strstr(e, "rotation mismatch") ||
             strstr(e, "registry mismatch") || strstr(e, "query returned no rows") ||
             strstr(e, "seal changed")))
      return KB_MGMT_ROOT_DB_CONFLICT;
   return KB_MGMT_ROOT_DB_RETRY;
}

static int role_assert(void *connection)
{
   char e[256] = "";
   static const char sql[] =
       "SELECT current_user='aimee_kb_token_roots_provision' "
       "AND session_user<>current_user AND NOT r.rolcanlogin AND NOT r.rolsuper "
       "AND NOT r.rolbypassrls AND NOT r.rolcreatedb AND NOT r.rolcreaterole "
       "AND NOT r.rolreplication AND NOT s.rolsuper AND NOT s.rolbypassrls "
       "AND NOT s.rolcreatedb AND NOT s.rolcreaterole AND NOT s.rolreplication "
       "AND pg_has_role(session_user,'aimee_kb_migrate','MEMBER') "
       "AND current_setting('search_path')='pg_catalog, pg_temp' "
       "AND current_setting('row_security')='on' "
       "AND NOT has_schema_privilege('public','CREATE') "
       "FROM pg_catalog.pg_roles r CROSS JOIN pg_catalog.pg_roles s "
       "WHERE r.rolname=current_user AND s.rolname=session_user";
   aimee_pg_stmt_t *s = aimee_pg_prepare(connection, sql, e, sizeof(e));
   int ok = s && aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW && !aimee_pg_column_is_null(s, 0) &&
            truth(aimee_pg_column_text(s, 0)) && aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_DONE;
   if (s)
      aimee_pg_finalize(s);
   return ok ? 0 : -1;
}

int db2_management_token_roots_open(db2_management_token_roots_ctx_t *ctx, const char *conninfo,
                                    char *errbuf, size_t errlen)
{
   if (!ctx || !conninfo || !*conninfo)
      return -1;
   memset(ctx, 0, sizeof(*ctx));
   ctx->connection = aimee_pg_open(conninfo, errbuf, errlen);
   if (!ctx->connection)
      return -1;
   if (aimee_pg_exec(ctx->connection, "SET search_path = pg_catalog, pg_temp", errbuf, errlen) ||
       aimee_pg_exec(ctx->connection, "SET row_security = on", errbuf, errlen) ||
       aimee_pg_exec(ctx->connection, "SET ROLE aimee_kb_token_roots_provision", errbuf, errlen) ||
       role_assert(ctx->connection) || session_lock_acquire(ctx, errbuf, errlen))
   {
      if (errbuf && errlen)
         snprintf(errbuf, errlen, "management token roots database role assertion failed");
      aimee_pg_close(ctx->connection);
      memset(ctx, 0, sizeof(*ctx));
      return -1;
   }
   return 0;
}

void db2_management_token_roots_close(db2_management_token_roots_ctx_t *ctx)
{
   if (!ctx)
      return;
   if (ctx->connection && aimee_pg_in_transaction(ctx->connection))
   {
      char e[128] = "";
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", e, sizeof(e));
   }
   session_lock_release(ctx);
   if (ctx->connection)
      aimee_pg_close(ctx->connection);
   memset(ctx, 0, sizeof(*ctx));
}

static int copy_text(char *out, size_t cap, const char *in)
{
   size_t n = in ? strlen(in) : 0;
   if (!out || !cap || n >= cap)
      return -1;
   memcpy(out, in, n + 1);
   return 0;
}

static int copy_blob(aimee_pg_stmt_t *s, int col, uint8_t *out, size_t cap, size_t *len,
                     size_t exact, int nullable)
{
   if (aimee_pg_column_is_null(s, col))
   {
      if (len)
         *len = 0;
      return nullable ? 0 : -1;
   }
   int n = aimee_pg_column_bytes(s, col);
   const void *p = aimee_pg_column_blob(s, col);
   if (!p || n < 1 || (size_t)n > cap || (exact && (size_t)n != exact))
      return -1;
   memcpy(out, p, (size_t)n);
   if (len)
      *len = (size_t)n;
   return 0;
}

static kb_mgmt_root_db_result_t inspect_root(void *opaque, kb_mgmt_root_kind_t kind,
                                             const char *custody_id, kb_mgmt_root_record_t *out)
{
   db2_management_token_roots_ctx_t *ctx = opaque;
   const char *kind_s = kind_name(kind);
   if (!idle(ctx) || !kind_s || !custody_id || !*custody_id ||
       strlen(custody_id) > KB_MGMT_ROOT_CUSTODY_ID_MAX || !out)
      return KB_MGMT_ROOT_DB_INTEGRITY;
   memset(out, 0, sizeof(*out));
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_token_root_bootstrap_resume(?1,?2)", e,
       sizeof(e));
   if (!s)
      return classify(e);
   aimee_pg_bind_text(s, "?1", kind_s);
   aimee_pg_bind_text(s, "?2", custody_id);
   aimee_pg_step_t step = aimee_pg_step(s, e, sizeof(e));
   int ok = step == AIMEE_PG_ROW;
   if (ok)
   {
      const char *phase = aimee_pg_column_text(s, 0);
      out->kind = kind;
      ok = !copy_text(out->custody_key_id, sizeof(out->custody_key_id), custody_id) &&
           !copy_text(out->bootstrap_id, sizeof(out->bootstrap_id), aimee_pg_column_text(s, 1));
      if (phase && strcmp(phase, "empty") == 0)
      {
         out->phase = KB_MGMT_ROOT_EMPTY;
         out->seal_epoch = (uint64_t)aimee_pg_column_int64(s, 6);
         ok = ok && out->seal_epoch > 0;
      }
      else
      {
         out->phase = phase && strcmp(phase, "staged") == 0     ? KB_MGMT_ROOT_STAGED
                      : phase && strcmp(phase, "cas_done") == 0 ? KB_MGMT_ROOT_CAS_DONE
                      : phase && strcmp(phase, "final") == 0    ? KB_MGMT_ROOT_FINAL
                                                                : KB_MGMT_ROOT_EMPTY;
         ok = ok && out->phase != KB_MGMT_ROOT_EMPTY &&
              !copy_text(out->wire_id, sizeof(out->wire_id), aimee_pg_column_text(s, 2)) &&
              !copy_blob(s, 3, out->public_key, sizeof(out->public_key), &out->public_key_len,
                         kind == KB_MGMT_ROOT_TOKEN ? KB_MGMT_TOKEN_MODULUS_LEN : 32, 0) &&
              !copy_blob(s, 4, out->public_digest, sizeof(out->public_digest), NULL, 32, 0) &&
              !copy_blob(s, 5, out->jwk_digest, sizeof(out->jwk_digest), NULL, 32, 0) &&
              !copy_blob(s, 8, out->v1.wrapped_dek, sizeof(out->v1.wrapped_dek), NULL,
                         sizeof(out->v1.wrapped_dek), 0) &&
              !copy_blob(s, 9, out->v1.nonce, sizeof(out->v1.nonce), NULL, sizeof(out->v1.nonce),
                         0) &&
              !copy_blob(s, 10, out->v1.ciphertext, sizeof(out->v1.ciphertext),
                         &out->v1.ciphertext_len, 0, 0) &&
              !copy_blob(s, 11, out->v1.tag, sizeof(out->v1.tag), NULL, sizeof(out->v1.tag), 0) &&
              !copy_blob(s, 12, out->hwm1_attestation, sizeof(out->hwm1_attestation),
                         &out->hwm1_attestation_len, 0, 0) &&
              !copy_blob(s, 13, out->v2.wrapped_dek, sizeof(out->v2.wrapped_dek), NULL,
                         sizeof(out->v2.wrapped_dek), 0) &&
              !copy_blob(s, 14, out->v2.nonce, sizeof(out->v2.nonce), NULL, sizeof(out->v2.nonce),
                         0) &&
              !copy_blob(s, 15, out->v2.ciphertext, sizeof(out->v2.ciphertext),
                         &out->v2.ciphertext_len, 0, 0) &&
              !copy_blob(s, 16, out->v2.tag, sizeof(out->v2.tag), NULL, sizeof(out->v2.tag), 0) &&
              !copy_blob(s, 17, out->hwm2_attestation, sizeof(out->hwm2_attestation),
                         &out->hwm2_attestation_len, 0, 1) &&
              !copy_blob(s, 18, out->v1_digest, sizeof(out->v1_digest), NULL, 32, 0) &&
              !copy_blob(s, 19, out->v2_digest, sizeof(out->v2_digest), NULL, 32, 0);
         out->seal_epoch = (uint64_t)aimee_pg_column_int64(s, 6);
         out->v1.version = 1;
         out->v2.version = 2;
         ok = ok && out->seal_epoch > 0;
      }
   }
   aimee_pg_finalize(s);
   if (!ok)
   {
      memset(out, 0, sizeof(*out));
      return step == AIMEE_PG_ROW ? KB_MGMT_ROOT_DB_INTEGRITY : classify(e);
   }
   return KB_MGMT_ROOT_DB_OK;
}

static kb_mgmt_root_db_result_t stage_root(void *opaque, const kb_mgmt_root_record_t *r)
{
   db2_management_token_roots_ctx_t *ctx = opaque;
   const char *kind = r ? kind_name(r->kind) : NULL;
   if (!idle(ctx) || !kind || !r || r->phase != KB_MGMT_ROOT_STAGED || r->seal_epoch < 1 ||
       r->v1.version != 1 || r->v2.version != 2 || !r->v1.ciphertext_len ||
       r->v1.ciphertext_len > sizeof(r->v1.ciphertext) || !r->v2.ciphertext_len ||
       r->v2.ciphertext_len > sizeof(r->v2.ciphertext) || !r->hwm1_attestation_len ||
       r->hwm1_attestation_len > sizeof(r->hwm1_attestation))
      return KB_MGMT_ROOT_DB_INTEGRITY;
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection,
       "SELECT public.kb_management_token_root_bootstrap_stage(?1,?2,?3,?4,?5,?6,?7,"
       "?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19)",
       e, sizeof(e));
   if (!s)
      return classify(e);
#define BT(n, v)    aimee_pg_bind_text(s, "?" #n, (v))
#define BB(n, v, z) aimee_pg_bind_blob(s, "?" #n, (v), (int)(z))
   BT(1, kind);
   BT(2, r->bootstrap_id);
   BT(3, r->custody_key_id);
   BT(4, r->wire_id);
   BB(5, r->public_key, r->public_key_len);
   BB(6, r->public_digest, 32);
   BB(7, r->jwk_digest, 32);
   aimee_pg_bind_int64(s, "?8", (int64_t)r->seal_epoch);
   BB(9, r->hwm1_attestation, r->hwm1_attestation_len);
   BB(10, r->v1.wrapped_dek, sizeof(r->v1.wrapped_dek));
   BB(11, r->v1.nonce, sizeof(r->v1.nonce));
   BB(12, r->v1.ciphertext, r->v1.ciphertext_len);
   BB(13, r->v1.tag, sizeof(r->v1.tag));
   BB(14, r->v2.wrapped_dek, sizeof(r->v2.wrapped_dek));
   BB(15, r->v2.nonce, sizeof(r->v2.nonce));
   BB(16, r->v2.ciphertext, r->v2.ciphertext_len);
   BB(17, r->v2.tag, sizeof(r->v2.tag));
   BB(18, r->v1_digest, 32);
   BB(19, r->v2_digest, 32);
#undef BT
#undef BB
   int ok = aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW;
   aimee_pg_finalize(s);
   return ok ? KB_MGMT_ROOT_DB_OK : classify(e);
}

static kb_mgmt_root_db_result_t record_cas(void *opaque, const kb_mgmt_root_record_t *r,
                                           const uint8_t *att, size_t att_len)
{
   db2_management_token_roots_ctx_t *ctx = opaque;
   const char *kind = r ? kind_name(r->kind) : NULL;
   if (!idle(ctx) || !kind || !r || !att || !att_len || att_len > KB_MGMT_ROOT_ATTEST_MAX)
      return KB_MGMT_ROOT_DB_INTEGRITY;
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT public.kb_management_token_root_bootstrap_record_cas(?1,?2,?3)", e,
       sizeof(e));
   if (!s)
      return classify(e);
   aimee_pg_bind_text(s, "?1", kind);
   aimee_pg_bind_text(s, "?2", r->bootstrap_id);
   aimee_pg_bind_blob(s, "?3", att, (int)att_len);
   int ok = aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW;
   aimee_pg_finalize(s);
   return ok ? KB_MGMT_ROOT_DB_OK : classify(e);
}

static kb_mgmt_root_db_result_t finalize_root(void *opaque, const kb_mgmt_root_record_t *r)
{
   db2_management_token_roots_ctx_t *ctx = opaque;
   const char *kind = r ? kind_name(r->kind) : NULL;
   if (!idle(ctx) || !kind || !r)
      return KB_MGMT_ROOT_DB_INTEGRITY;
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT public.kb_management_token_root_bootstrap_finalize(?1,?2)", e,
       sizeof(e));
   if (!s)
      return classify(e);
   aimee_pg_bind_text(s, "?1", kind);
   aimee_pg_bind_text(s, "?2", r->bootstrap_id);
   int ok = aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW;
   aimee_pg_finalize(s);
   return ok ? KB_MGMT_ROOT_DB_OK : classify(e);
}

static kb_mgmt_root_db_result_t inspect_publication(void *opaque, kb_mgmt_publication_root_t *out)
{
   db2_management_token_roots_ctx_t *ctx = opaque;
   if (!idle(ctx) || !out)
      return KB_MGMT_ROOT_DB_INTEGRITY;
   memset(out, 0, sizeof(*out));
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_jwks_publication_root_inspect()", e,
       sizeof(e));
   if (!s)
      return classify(e);
   aimee_pg_step_t step = aimee_pg_step(s, e, sizeof(e));
   int ok = step == AIMEE_PG_DONE;
   if (step == AIMEE_PG_ROW)
   {
      out->bound = 1;
      ok = !copy_text(out->custody_key_id, sizeof(out->custody_key_id),
                      aimee_pg_column_text(s, 1)) &&
           !copy_text(out->helper, sizeof(out->helper), aimee_pg_column_text(s, 2)) &&
           !copy_text(out->verifier_domain, sizeof(out->verifier_domain),
                      aimee_pg_column_text(s, 3)) &&
           !copy_blob(s, 4, out->identity_digest, sizeof(out->identity_digest), NULL, 32, 0) &&
           !copy_blob(s, 5, out->hwm1_attestation, sizeof(out->hwm1_attestation),
                      &out->hwm1_attestation_len, 0, 0) &&
           aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_DONE;
   }
   aimee_pg_finalize(s);
   if (!ok)
      memset(out, 0, sizeof(*out));
   return ok ? KB_MGMT_ROOT_DB_OK : classify(e);
}

static kb_mgmt_root_db_result_t bind_publication(void *opaque, const kb_mgmt_publication_root_t *r)
{
   db2_management_token_roots_ctx_t *ctx = opaque;
   if (!idle(ctx) || !r || !r->custody_key_id[0] || !r->helper[0] || !r->verifier_domain[0] ||
       !r->hwm1_attestation_len || r->hwm1_attestation_len > sizeof(r->hwm1_attestation))
      return KB_MGMT_ROOT_DB_INTEGRITY;
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT public.kb_management_jwks_publication_root_bind(?1,?2,?3,?4,?5)", e,
       sizeof(e));
   if (!s)
      return classify(e);
   aimee_pg_bind_text(s, "?1", r->custody_key_id);
   aimee_pg_bind_text(s, "?2", r->helper);
   aimee_pg_bind_text(s, "?3", r->verifier_domain);
   aimee_pg_bind_blob(s, "?4", r->identity_digest, 32);
   aimee_pg_bind_blob(s, "?5", r->hwm1_attestation, (int)r->hwm1_attestation_len);
   int ok = aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW;
   aimee_pg_finalize(s);
   return ok ? KB_MGMT_ROOT_DB_OK : classify(e);
}

int db2_management_token_roots_bind(db2_management_token_roots_ctx_t *ctx, kb_mgmt_roots_db_t *out)
{
   if (!idle(ctx) || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->inspect_root = inspect_root;
   out->stage_root = stage_root;
   out->record_cas = record_cas;
   out->finalize_root = finalize_root;
   out->inspect_publication = inspect_publication;
   out->bind_publication = bind_publication;
   out->ctx = ctx;
   return 0;
}
