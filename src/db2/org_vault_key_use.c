#include "org_vault_key_use.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <string.h>

#define USE_ERR 256

static int classified_error(const char *err)
{
   if (err && strstr(err, "org_vault_control: sealed"))
      return DB2_VAULT_KEY_USE_SEALED;
   return err && (strstr(err, "org_vault_key_use_candidate: invalid input") ||
                  strstr(err, "org_vault_key_use_candidate: not authorized") ||
                  strstr(err, "org_vault_key_use_candidate: unstable key binding") ||
                  strstr(err, "org_vault_key_use_admit: invalid input") ||
                  strstr(err, "org_vault_key_use_admit: not authorized") ||
                  strstr(err, "org_vault_key_use_admit: replay mismatch") ||
                  strstr(err, "org_vault_key_use_admit: unstable key binding") ||
                  strstr(err, "org_vault_key_use_admit: attestation mismatch"))
              ? DB2_VAULT_KEY_USE_INTEGRITY
              : DB2_VAULT_KEY_USE_ERROR;
}

static int valid_text(const char *s, size_t max)
{
   return s && s[0] && strlen(s) <= max;
}

static int copy_blob(aimee_pg_stmt_t *st, int col, uint8_t *dst, size_t cap, size_t *len,
                     size_t exact)
{
   const void *p = aimee_pg_column_blob(st, col);
   int n = aimee_pg_column_bytes(st, col);
   if (!p || n <= 0 || (size_t)n > cap || (exact && (size_t)n != exact))
      return -1;
   memcpy(dst, p, (size_t)n);
   if (len)
      *len = (size_t)n;
   return 0;
}

static int read_envelope(aimee_pg_stmt_t *st, int first, int64_t version,
                         db2_vault_key_use_envelope_t *out)
{
   memset(out, 0, sizeof(*out));
   out->version = version;
   if (copy_blob(st, first, out->wrapped_dek, sizeof(out->wrapped_dek), NULL,
                 sizeof(out->wrapped_dek)) != 0 ||
       copy_blob(st, first + 1, out->nonce, sizeof(out->nonce), NULL, sizeof(out->nonce)) != 0 ||
       copy_blob(st, first + 2, out->ciphertext, sizeof(out->ciphertext), &out->ciphertext_len,
                 0) != 0 ||
       copy_blob(st, first + 3, out->tag, sizeof(out->tag), NULL, sizeof(out->tag)) != 0 ||
       copy_blob(st, first + 4, out->hwm_attestation, sizeof(out->hwm_attestation),
                 &out->hwm_attestation_len, 0) != 0)
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }
   return 0;
}

int db2_vault_control_startup_begin(int64_t *epoch_out, int *sealed_out)
{
   if (epoch_out)
      *epoch_out = 0;
   if (sealed_out)
      *sealed_out = 0;
   if (!epoch_out || !sealed_out)
      return DB2_VAULT_KEY_USE_ERROR;
   if (db2_tenant_require_pg() != 0)
      return DB2_VAULT_KEY_USE_ERROR;

   char err[USE_ERR] = "";
   if (aimee_pg_exec(db2_conn(), "BEGIN", err, sizeof(err)) != 0)
      return DB2_VAULT_KEY_USE_ERROR;
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT seal_epoch,sealed FROM public.org_vault_control_startup_status()", err,
       sizeof(err));
   if (!st)
   {
      (void)aimee_pg_exec(db2_conn(), "ROLLBACK", err, sizeof(err));
      return DB2_VAULT_KEY_USE_ERROR;
   }
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int rc = DB2_VAULT_KEY_USE_ERROR;
   if (step == AIMEE_PG_ROW && !aimee_pg_column_is_null(st, 0) && aimee_pg_column_int64(st, 0) > 0)
   {
      const char *sealed = aimee_pg_column_text(st, 1);
      if (sealed &&
          ((sealed[0] == 't' && sealed[1] == '\0') || (sealed[0] == 'f' && sealed[1] == '\0') ||
           (sealed[0] == '1' && sealed[1] == '\0') || (sealed[0] == '0' && sealed[1] == '\0')))
      {
         int64_t epoch = aimee_pg_column_int64(st, 0);
         int is_sealed = sealed[0] == 't' || sealed[0] == '1';
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
         {
            *epoch_out = epoch;
            *sealed_out = is_sealed;
            rc = 0;
         }
      }
   }
   aimee_pg_finalize(st);
   if (rc != 0)
      (void)aimee_pg_exec(db2_conn(), "ROLLBACK", err, sizeof(err));
   return rc;
}

int db2_vault_control_startup_end(int commit)
{
   if (db2_tenant_require_pg() != 0)
      return DB2_VAULT_KEY_USE_ERROR;
   char err[USE_ERR] = "";
   if (commit && aimee_pg_exec(db2_conn(), "COMMIT", err, sizeof(err)) == 0)
      return 0;
   (void)aimee_pg_exec(db2_conn(), "ROLLBACK", err, sizeof(err));
   return commit ? DB2_VAULT_KEY_USE_ERROR : 0;
}

int db2_vault_key_use_candidate(const char *actor, int64_t team_id, const char *key_id,
                                const char *principal, const char *agent, const char *cred,
                                int64_t version, db2_vault_key_use_envelope_t *out)
{
   if (db2_tenant_require_pg() != 0 || !valid_text(actor, 575) || team_id < 1 ||
       !valid_text(key_id, 600) || !valid_text(principal, 600) || !agent || strlen(agent) > 255 ||
       !cred || strlen(cred) > 255 || version < 1 || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   char err[USE_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT * FROM org_vault_key_use_candidate(?1,?2,?3,?4,?5,?6,?7)", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_bind_text(st, "?3", key_id);
   aimee_pg_bind_text(st, "?4", principal);
   aimee_pg_bind_text(st, "?5", agent);
   aimee_pg_bind_text(st, "?6", cred);
   aimee_pg_bind_int64(st, "?7", version);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int rc = step == AIMEE_PG_ROW
                ? (read_envelope(st, 0, version, out) == 0 ? 0 : DB2_VAULT_KEY_USE_INTEGRITY)
                : (step == AIMEE_PG_DONE ? DB2_VAULT_KEY_USE_MISSING : classified_error(err));
   aimee_pg_finalize(st);
   return rc;
}

int db2_vault_key_use_admit(const char *actor, int64_t team_id, const char *authenticated_origin,
                            const char *use_id, const char *key_id, const char *principal,
                            const char *agent, const char *cred, int64_t version,
                            const char *request_digest, const char *provider, const char *model,
                            const char *operation, const uint8_t *hwm_attestation,
                            size_t hwm_attestation_len, db2_vault_key_use_envelope_t *out)
{
   if (db2_tenant_require_pg() != 0 || !valid_text(actor, 575) || team_id < 1 ||
       !valid_text(authenticated_origin, 575) || !valid_text(use_id, 200) ||
       !valid_text(key_id, 600) || !valid_text(principal, 600) || !agent || strlen(agent) > 255 ||
       !cred || strlen(cred) > 255 || version < 1 || !valid_text(request_digest, 64) ||
       strlen(request_digest) != 64 || !valid_text(provider, 64) || !valid_text(model, 255) ||
       !valid_text(operation, 64) || !hwm_attestation || hwm_attestation_len == 0 ||
       hwm_attestation_len > DB2_VAULT_KEY_USE_ATTEST_MAX || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   char err[USE_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "SELECT * FROM org_vault_key_use_admit(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_bind_text(st, "?3", authenticated_origin);
   aimee_pg_bind_text(st, "?4", use_id);
   aimee_pg_bind_text(st, "?5", key_id);
   aimee_pg_bind_text(st, "?6", principal);
   aimee_pg_bind_text(st, "?7", agent);
   aimee_pg_bind_text(st, "?8", cred);
   aimee_pg_bind_int64(st, "?9", version);
   aimee_pg_bind_text(st, "?10", request_digest);
   aimee_pg_bind_text(st, "?11", provider);
   aimee_pg_bind_text(st, "?12", model);
   aimee_pg_bind_text(st, "?13", operation);
   aimee_pg_bind_blob(st, "?14", hwm_attestation, (int)hwm_attestation_len);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return classified_error(err);
   }
   const char *b = aimee_pg_column_text(st, 0);
   if (!b)
   {
      aimee_pg_finalize(st);
      return DB2_VAULT_KEY_USE_INTEGRITY;
   }
   int newly_admitted = b[0] == 't' || b[0] == '1';
   int rc = 0;
   if (aimee_pg_column_is_null(st, 1) || aimee_pg_column_int64(st, 1) < 1)
      rc = DB2_VAULT_KEY_USE_INTEGRITY;
   else
      out->seal_epoch = aimee_pg_column_int64(st, 1);
   if (newly_admitted)
   {
      int64_t seal_epoch = out->seal_epoch;
      if (rc == 0 && read_envelope(st, 2, version, out) == 0)
         out->seal_epoch = seal_epoch;
      else
         rc = DB2_VAULT_KEY_USE_INTEGRITY;
   }
   else
      for (int col = 2; col <= 6; col++)
         if (!aimee_pg_column_is_null(st, col))
            rc = DB2_VAULT_KEY_USE_INTEGRITY;
   aimee_pg_finalize(st);
   return rc == 0 ? newly_admitted : rc;
}
