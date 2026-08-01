#include "management_status_key.h"

#include "db_postgres.h"

#include <string.h>

static int ctx_idle(const db2_management_status_key_ctx_t *ctx)
{
   return ctx && ctx->connection && !ctx->transaction_active &&
          !aimee_pg_in_transaction(ctx->connection);
}

int db2_management_status_key_ctx_open(db2_management_status_key_ctx_t *ctx, const char *conninfo,
                                       char *errbuf, size_t errlen)
{
   if (!ctx || !conninfo || !*conninfo)
      return DB2_VAULT_KEY_USE_ERROR;
   memset(ctx, 0, sizeof(*ctx));
   ctx->connection = aimee_pg_open(conninfo, errbuf, errlen);
   if (!ctx->connection)
      return DB2_VAULT_KEY_USE_ERROR;
   ctx->owns_connection = 1;
   return 0;
}

int db2_management_status_key_ctx_borrow_hardened(db2_management_status_key_ctx_t *ctx,
                                                  const db2_management_status_runtime_t *runtime)
{
   if (!ctx || !runtime || !runtime->connection || runtime->transaction_active ||
       aimee_pg_in_transaction(runtime->connection))
      return DB2_VAULT_KEY_USE_ERROR;
   memset(ctx, 0, sizeof(*ctx));
   ctx->connection = runtime->connection;
   ctx->owns_connection = 0;
   return 0;
}

void db2_management_status_key_ctx_close(db2_management_status_key_ctx_t *ctx)
{
   if (!ctx)
      return;
   if (ctx->connection && (ctx->transaction_active || aimee_pg_in_transaction(ctx->connection)))
   {
      char e[128] = "";
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", e, sizeof(e));
   }
   if (ctx->connection && ctx->owns_connection)
      aimee_pg_close(ctx->connection);
   memset(ctx, 0, sizeof(*ctx));
}

static int classify(const char *e)
{
   if (e && strstr(e, "org_vault_control: sealed"))
      return DB2_VAULT_KEY_USE_SEALED;
   if (e && (strstr(e, "invalid input") || strstr(e, "replay mismatch") ||
             strstr(e, "attestation mismatch")))
      return DB2_VAULT_KEY_USE_INTEGRITY;
   return DB2_VAULT_KEY_USE_ERROR;
}

static int blob(aimee_pg_stmt_t *s, int col, uint8_t *out, size_t cap, size_t *len, size_t exact)
{
   const void *p = aimee_pg_column_blob(s, col);
   int n = aimee_pg_column_bytes(s, col);
   if (!p || n <= 0 || (size_t)n > cap || (exact && (size_t)n != exact))
      return -1;
   memcpy(out, p, (size_t)n);
   if (len)
      *len = (size_t)n;
   return 0;
}

static int envelope(aimee_pg_stmt_t *s, int col, int64_t version, db2_vault_key_use_envelope_t *out)
{
   int64_t epoch = out->seal_epoch;
   memset(out, 0, sizeof(*out));
   out->seal_epoch = epoch;
   out->version = version;
   return blob(s, col, out->wrapped_dek, sizeof(out->wrapped_dek), NULL,
               sizeof(out->wrapped_dek)) ||
                  blob(s, col + 1, out->nonce, sizeof(out->nonce), NULL, sizeof(out->nonce)) ||
                  blob(s, col + 2, out->ciphertext, sizeof(out->ciphertext), &out->ciphertext_len,
                       0) ||
                  blob(s, col + 3, out->tag, sizeof(out->tag), NULL, sizeof(out->tag)) ||
                  blob(s, col + 4, out->hwm_attestation, sizeof(out->hwm_attestation),
                       &out->hwm_attestation_len, 0)
              ? -1
              : 0;
}

int db2_management_status_key_candidate(db2_management_status_key_ctx_t *ctx, const char *key_id,
                                        const char *wire_key_id, int64_t version,
                                        db2_vault_key_use_envelope_t *out)
{
   if (!ctx_idle(ctx) || !key_id || !*key_id || strlen(key_id) > 600 || !wire_key_id ||
       !*wire_key_id || strlen(wire_key_id) > 64 || version < 1 || !out)
      return DB2_VAULT_KEY_USE_ERROR;
   memset(out, 0, sizeof(*out));
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_status_key_candidate(?1,?2,?3)", e,
       sizeof(e));
   if (!s)
      return DB2_VAULT_KEY_USE_ERROR;
   aimee_pg_bind_text(s, "?1", key_id);
   aimee_pg_bind_text(s, "?2", wire_key_id);
   aimee_pg_bind_int64(s, "?3", version);
   aimee_pg_step_t step = aimee_pg_step(s, e, sizeof(e));
   int rc = step == AIMEE_PG_ROW
                ? (envelope(s, 0, version, out) ? DB2_VAULT_KEY_USE_INTEGRITY : 0)
                : (step == AIMEE_PG_DONE ? DB2_VAULT_KEY_USE_MISSING : classify(e));
   aimee_pg_finalize(s);
   return rc;
}

int db2_management_status_key_admit(db2_management_status_key_ctx_t *ctx,
                                    const db2_management_status_admission_t *p,
                                    db2_vault_key_use_envelope_t *out)
{
   if (!ctx_idle(ctx) || !p || !out || !p->use_id || !p->custody_key_id || !p->wire_key_id ||
       !p->request_digest || !p->caller_issuer || !p->caller_serial_norm ||
       !p->caller_fingerprint || !p->target_server_id || !p->target_mgmt_fingerprint ||
       p->version < 1 || p->revocation_generation < 1 || !p->hwm_attestation ||
       p->hwm_attestation_len < 1 || p->hwm_attestation_len > DB2_VAULT_KEY_USE_ATTEST_MAX)
      return DB2_VAULT_KEY_USE_ERROR;
   memset(out, 0, sizeof(*out));
   char e[256] = "";
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection,
       "SELECT * FROM "
       "public.kb_management_status_key_admit(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)",
       e, sizeof(e));
   if (!s)
      return DB2_VAULT_KEY_USE_ERROR;
   aimee_pg_bind_text(s, "?1", p->use_id);
   aimee_pg_bind_text(s, "?2", p->custody_key_id);
   aimee_pg_bind_text(s, "?3", p->wire_key_id);
   aimee_pg_bind_int64(s, "?4", p->version);
   aimee_pg_bind_text(s, "?5", p->request_digest);
   aimee_pg_bind_text(s, "?6", p->caller_issuer);
   aimee_pg_bind_text(s, "?7", p->caller_serial_norm);
   aimee_pg_bind_text(s, "?8", p->caller_fingerprint);
   aimee_pg_bind_text(s, "?9", p->target_server_id);
   aimee_pg_bind_text(s, "?10", p->target_mgmt_fingerprint);
   aimee_pg_bind_int64(s, "?11", p->revocation_generation);
   aimee_pg_bind_blob(s, "?12", p->hwm_attestation, (int)p->hwm_attestation_len);
   aimee_pg_step_t step = aimee_pg_step(s, e, sizeof(e));
   if (step != AIMEE_PG_ROW)
   {
      int rc = classify(e);
      aimee_pg_finalize(s);
      return rc;
   }
   const char *b = aimee_pg_column_text(s, 0);
   int fresh = b && (b[0] == 't' || b[0] == '1');
   int rc = DB2_VAULT_KEY_USE_INTEGRITY;
   if (b && !aimee_pg_column_is_null(s, 1) && aimee_pg_column_int64(s, 1) > 0)
   {
      out->seal_epoch = aimee_pg_column_int64(s, 1);
      if (!fresh)
      {
         rc = 0;
         for (int col = 2; col < 7; ++col)
            if (!aimee_pg_column_is_null(s, col))
               rc = DB2_VAULT_KEY_USE_INTEGRITY;
      }
      else if (!envelope(s, 2, p->version, out))
         rc = 1;
   }
   aimee_pg_finalize(s);
   return rc;
}

static int begin_one(db2_management_status_key_ctx_t *ctx, const char *sql, int64_t arg, int bind)
{
   char e[256] = "";
   if (!ctx_idle(ctx) || aimee_pg_exec(ctx->connection, "BEGIN", e, sizeof(e)))
      return DB2_VAULT_KEY_USE_ERROR;
   ctx->transaction_active = 1;
   aimee_pg_stmt_t *s = aimee_pg_prepare(ctx->connection, sql, e, sizeof(e));
   if (s && bind)
      aimee_pg_bind_int64(s, "?1", arg);
   int rc = s && aimee_pg_step(s, e, sizeof(e)) == AIMEE_PG_ROW ? 0 : classify(e);
   if (s)
      aimee_pg_finalize(s);
   if (rc)
   {
      (void)aimee_pg_exec(ctx->connection, "ROLLBACK", e, sizeof(e));
      ctx->transaction_active = 0;
   }
   return rc;
}

int db2_management_status_key_guard_begin(db2_management_status_key_ctx_t *ctx, int64_t epoch)
{
   if (epoch < 1)
      return DB2_VAULT_KEY_USE_INTEGRITY;
   return begin_one(ctx, "SELECT public.kb_management_status_key_use_guard(?1)", epoch, 1);
}

int db2_management_status_key_guard_end(db2_management_status_key_ctx_t *ctx, int commit)
{
   char e[256] = "";
   if (!ctx || !ctx->connection || !ctx->transaction_active)
      return DB2_VAULT_KEY_USE_ERROR;
   if (commit && !aimee_pg_exec(ctx->connection, "COMMIT", e, sizeof(e)))
   {
      ctx->transaction_active = 0;
      return 0;
   }
   (void)aimee_pg_exec(ctx->connection, "ROLLBACK", e, sizeof(e));
   ctx->transaction_active = 0;
   return commit ? DB2_VAULT_KEY_USE_ERROR : 0;
}

int db2_management_status_key_startup_begin(db2_management_status_key_ctx_t *ctx, int64_t *epoch,
                                            int *sealed)
{
   if (!epoch || !sealed)
      return DB2_VAULT_KEY_USE_ERROR;
   *epoch = 0;
   *sealed = 0;
   int rc = begin_one(ctx, "SELECT * FROM public.kb_management_status_key_startup_status()", 0, 0);
   if (rc)
      return rc;
   char e[256] = "";
   /* begin_one consumed the row; issue inside the still-open transaction to copy it. */
   aimee_pg_stmt_t *s = aimee_pg_prepare(
       ctx->connection, "SELECT * FROM public.kb_management_status_key_startup_status()", e,
       sizeof(e));
   if (!s || aimee_pg_step(s, e, sizeof(e)) != AIMEE_PG_ROW || aimee_pg_column_is_null(s, 0) ||
       aimee_pg_column_int64(s, 0) < 1)
   {
      if (s)
         aimee_pg_finalize(s);
      (void)db2_management_status_key_guard_end(ctx, 0);
      return DB2_VAULT_KEY_USE_ERROR;
   }
   const char *v = aimee_pg_column_text(s, 1);
   *epoch = aimee_pg_column_int64(s, 0);
   *sealed = v && (v[0] == 't' || v[0] == '1');
   aimee_pg_finalize(s);
   return 0;
}

int db2_management_status_key_startup_end(db2_management_status_key_ctx_t *ctx, int commit)
{
   return db2_management_status_key_guard_end(ctx, commit);
}
