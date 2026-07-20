#include "org_vault_rotation.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ROT_ERR 256

static int valid_blob(const void *p, size_t n)
{
   return p && n > 0 && n <= INT_MAX;
}

static void copy_text(char *dst, size_t cap, const char *src)
{
   snprintf(dst, cap, "%s", src ? src : "");
}

int db2_vault_rotation_start(const char *actor, const char *key_id, const char *principal,
                             int has_team, int64_t team_id, const char *agent, const char *cred,
                             int64_t from_version, int compromise, int64_t *out_id)
{
   if (db2_tenant_require_pg() != 0 || !actor || !actor[0] || !key_id || !key_id[0] ||
       !principal || !principal[0] || !agent || !cred || from_version < 1 ||
       from_version == INT64_MAX)
      return -1;
   void *conn = db2_conn();
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = conn ? aimee_pg_prepare(
       conn, "SELECT org_vault_rotation_start(?1,?2,?3,?4,?5,?6,?7,?8)", err, sizeof(err)) : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_text(st, "?2", key_id);
   aimee_pg_bind_text(st, "?3", principal);
   if (has_team)
      aimee_pg_bind_int64(st, "?4", team_id);
   else
      aimee_pg_bind_null(st, "?4");
   aimee_pg_bind_text(st, "?5", agent);
   aimee_pg_bind_text(st, "?6", cred);
   aimee_pg_bind_int64(st, "?7", from_version);
   aimee_pg_bind_text(st, "?8", compromise ? "true" : "false");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW && out_id)
      *out_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}

int db2_vault_rotation_stage(const char *actor, int64_t rotation_id, const uint8_t *wrapped_dek,
                             size_t wrapped_dek_len, const uint8_t *nonce, size_t nonce_len,
                             const uint8_t *ciphertext, size_t ciphertext_len, const uint8_t *tag,
                             size_t tag_len, int64_t *out_version)
{
   if (db2_tenant_require_pg() != 0 || !actor || !actor[0] || rotation_id < 1 ||
       !valid_blob(wrapped_dek, wrapped_dek_len) || !valid_blob(nonce, nonce_len) ||
       !valid_blob(ciphertext, ciphertext_len) || !valid_blob(tag, tag_len))
      return -1;
   void *conn = db2_conn();
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = conn ? aimee_pg_prepare(
       conn, "SELECT org_vault_rotation_stage(?1,?2,?3,?4,?5,?6)", err, sizeof(err)) : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_blob(st, "?3", wrapped_dek, (int)wrapped_dek_len);
   aimee_pg_bind_blob(st, "?4", nonce, (int)nonce_len);
   aimee_pg_bind_blob(st, "?5", ciphertext, (int)ciphertext_len);
   aimee_pg_bind_blob(st, "?6", tag, (int)tag_len);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW && out_version)
      *out_version = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}

int db2_vault_rotation_transition(const char *actor, int64_t rotation_id, const char *expected,
                                  const char *next, const char *error)
{
   if (db2_tenant_require_pg() != 0 || !actor || !actor[0] || rotation_id < 1 || !expected ||
       !expected[0] || !next || !next[0])
      return -1;
   void *conn = db2_conn();
   char errbuf[ROT_ERR] = "";
   aimee_pg_stmt_t *st = conn ? aimee_pg_prepare(
       conn, "SELECT org_vault_rotation_transition(?1,?2,?3,?4,?5)", errbuf, sizeof(errbuf)) : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", expected);
   aimee_pg_bind_text(st, "?4", next);
   aimee_pg_bind_text(st, "?5", error ? error : "");
   aimee_pg_step_t step = aimee_pg_step(st, errbuf, sizeof(errbuf));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}

int db2_vault_rotation_finalize(const char *actor, int64_t rotation_id,
                                const uint8_t *attestation, size_t attestation_len)
{
   if (db2_tenant_require_pg() != 0 || !actor || !actor[0] || rotation_id < 1 ||
       !valid_blob(attestation, attestation_len))
      return -1;
   void *conn = db2_conn();
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = conn ? aimee_pg_prepare(
       conn, "SELECT org_vault_rotation_finalize(?1,?2,?3)", err, sizeof(err)) : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_blob(st, "?3", attestation, (int)attestation_len);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}

int db2_vault_rotation_get(int64_t rotation_id, db2_vault_rotation_row_t *out)
{
   if (db2_tenant_require_pg() != 0 || rotation_id < 1 || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = conn ? aimee_pg_prepare(
       conn, "SELECT * FROM org_vault_rotation_get(?1)", err, sizeof(err)) : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", rotation_id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   out->id = aimee_pg_column_int64(st, 0);
   copy_text(out->key_id, sizeof(out->key_id), aimee_pg_column_text(st, 1));
   copy_text(out->principal, sizeof(out->principal), aimee_pg_column_text(st, 2));
   out->has_team = !aimee_pg_column_is_null(st, 3);
   if (out->has_team)
      out->team_id = aimee_pg_column_int64(st, 3);
   copy_text(out->agent, sizeof(out->agent), aimee_pg_column_text(st, 4));
   copy_text(out->cred, sizeof(out->cred), aimee_pg_column_text(st, 5));
   out->from_version = aimee_pg_column_int64(st, 6);
   out->to_version = aimee_pg_column_int64(st, 7);
   copy_text(out->state, sizeof(out->state), aimee_pg_column_text(st, 8));
   const char *b = aimee_pg_column_text(st, 9);
   out->compromise = b && (b[0] == 't' || b[0] == '1');
   if (!aimee_pg_column_is_null(st, 10))
   {
      const void *blob = aimee_pg_column_blob(st, 10);
      int n = aimee_pg_column_bytes(st, 10);
      if (!blob || n <= 0 || (size_t)n > sizeof(out->hwm_attestation))
      {
         aimee_pg_finalize(st);
         memset(out, 0, sizeof(*out));
         return -1;
      }
      memcpy(out->hwm_attestation, blob, (size_t)n);
      out->hwm_attestation_len = (size_t)n;
   }
   copy_text(out->last_error, sizeof(out->last_error), aimee_pg_column_text(st, 11));
   aimee_pg_finalize(st);
   return 0;
}
