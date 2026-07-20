#include "org_vault_key_use.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <string.h>

#define USE_ERR 256

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
   int rc = step == AIMEE_PG_ROW ? read_envelope(st, 0, version, out)
                                 : (step == AIMEE_PG_DONE ? -2 : -1);
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
      return -1;
   }
   const char *b = aimee_pg_column_text(st, 0);
   int newly_admitted = b && (b[0] == 't' || b[0] == '1');
   int rc = newly_admitted ? read_envelope(st, 1, version, out) : 0;
   aimee_pg_finalize(st);
   return rc == 0 ? newly_admitted : -1;
}
