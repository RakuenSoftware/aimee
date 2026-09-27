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
   if (db2_tenant_require_pg() != 0 || !actor || !actor[0] || !key_id || !key_id[0] || !principal ||
       !principal[0] || !agent || !cred || from_version < 1 || from_version == INT64_MAX)
      return -1;
   void *conn = db2_conn();
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(conn, "SELECT org_vault_rotation_start(?1,?2,?3,?4,?5,?6,?7,?8)",
                               err, sizeof(err))
            : NULL;
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
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(conn, "SELECT org_vault_rotation_stage(?1,?2,?3,?4,?5,?6)", err,
                               sizeof(err))
            : NULL;
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
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(conn, "SELECT org_vault_rotation_transition(?1,?2,?3,?4,?5)", errbuf,
                               sizeof(errbuf))
            : NULL;
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

int db2_vault_rotation_finalize(const char *actor, int64_t rotation_id, const uint8_t *attestation,
                                size_t attestation_len)
{
   if (db2_tenant_require_pg() != 0 || !actor || !actor[0] || rotation_id < 1 ||
       !valid_blob(attestation, attestation_len))
      return -1;
   void *conn = db2_conn();
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(conn, "SELECT org_vault_rotation_finalize(?1,?2,?3)", err,
                               sizeof(err))
            : NULL;
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
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(conn, "SELECT * FROM org_vault_rotation_get(?1)", err, sizeof(err))
            : NULL;
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
   copy_text(out->old_vendor_ref, sizeof(out->old_vendor_ref), aimee_pg_column_text(st, 12));
   copy_text(out->new_vendor_ref, sizeof(out->new_vendor_ref), aimee_pg_column_text(st, 13));
   copy_text(out->revoke_receipt, sizeof(out->revoke_receipt), aimee_pg_column_text(st, 14));
   copy_text(out->failure_phase, sizeof(out->failure_phase), aimee_pg_column_text(st, 15));
   copy_text(out->claim_owner, sizeof(out->claim_owner), aimee_pg_column_text(st, 16));
   out->claim_token = aimee_pg_column_int64(st, 17);
   copy_text(out->claim_until, sizeof(out->claim_until), aimee_pg_column_text(st, 18));
   aimee_pg_finalize(st);
   return 0;
}

int db2_vault_rotation_claim(const char *actor, int64_t rotation_id, const char *expected,
                             const char *owner, int ttl_seconds, int64_t *token)
{
   if (db2_tenant_require_pg() != 0 || !actor || !*actor || rotation_id < 1 || !expected ||
       !*expected || !owner || !*owner || ttl_seconds < 5 || ttl_seconds > 300 || !token)
      return -1;
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT org_vault_rotation_claim(?1,?2,?3,?4,?5)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", expected);
   aimee_pg_bind_text(st, "?4", owner);
   aimee_pg_bind_int64(st, "?5", ttl_seconds);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW)
      *token = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW && *token > 0 ? 0 : -1;
}

static int claim_boolean(const char *sql, const char *actor, int64_t rotation_id, const char *owner,
                         int64_t token, int ttl_seconds)
{
   if (db2_tenant_require_pg() != 0 || !actor || !*actor || rotation_id < 1 || !owner || !*owner ||
       token < 1)
      return -1;
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", owner);
   aimee_pg_bind_int64(st, "?4", token);
   if (ttl_seconds)
      aimee_pg_bind_int64(st, "?5", ttl_seconds);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   const char *value = step == AIMEE_PG_ROW ? aimee_pg_column_text(st, 0) : NULL;
   int ok = value && (value[0] == 't' || value[0] == '1');
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

int db2_vault_rotation_heartbeat(const char *actor, int64_t rotation_id, const char *owner,
                                 int64_t token, int ttl_seconds)
{
   if (ttl_seconds < 5 || ttl_seconds > 300)
      return -1;
   return claim_boolean("SELECT org_vault_rotation_heartbeat(?1,?2,?3,?4,?5)", actor, rotation_id,
                        owner, token, ttl_seconds);
}

int db2_vault_rotation_release(const char *actor, int64_t rotation_id, const char *owner,
                               int64_t token)
{
   return claim_boolean("SELECT org_vault_rotation_release(?1,?2,?3,?4)", actor, rotation_id, owner,
                        token, 0);
}

int db2_vault_rotation_checkpoint_old_ref(const char *actor, int64_t rotation_id, const char *owner,
                                          int64_t token, const char *old_vendor_ref)
{
   if (db2_tenant_require_pg() != 0 || !actor || !*actor || rotation_id < 1 || !owner || !*owner ||
       token < 1 || !old_vendor_ref || !*old_vendor_ref ||
       strlen(old_vendor_ref) > DB2_VAULT_ROTATION_REF_MAX)
      return -1;
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(), "SELECT org_vault_rotation_checkpoint_old_ref(?1,?2,?3,?4,?5)",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", owner);
   aimee_pg_bind_int64(st, "?4", token);
   aimee_pg_bind_text(st, "?5", old_vendor_ref);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}

int db2_vault_rotation_stage_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                     int64_t token, const char *new_vendor_ref,
                                     const db2_vault_rotation_envelope_t *e)
{
   if (db2_tenant_require_pg() != 0 || !actor || !*actor || rotation_id < 1 || !owner || !*owner ||
       token < 1 || !e || !new_vendor_ref || !*new_vendor_ref ||
       strlen(new_vendor_ref) > DB2_VAULT_ROTATION_REF_MAX || !e->ciphertext_len ||
       e->ciphertext_len > sizeof(e->ciphertext))
      return -1;
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT org_vault_rotation_stage_claimed(?1,?2,?3,?4,?5,?6,?7,?8,?9)", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", owner);
   aimee_pg_bind_int64(st, "?4", token);
   aimee_pg_bind_text(st, "?5", new_vendor_ref);
   aimee_pg_bind_blob(st, "?6", e->wrapped_dek, (int)sizeof(e->wrapped_dek));
   aimee_pg_bind_blob(st, "?7", e->nonce, (int)sizeof(e->nonce));
   aimee_pg_bind_blob(st, "?8", e->ciphertext, (int)e->ciphertext_len);
   aimee_pg_bind_blob(st, "?9", e->tag, (int)sizeof(e->tag));
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}

static int copy_blob_column(aimee_pg_stmt_t *st, int column, void *out, size_t expected)
{
   const void *blob = aimee_pg_column_blob(st, column);
   int n = aimee_pg_column_bytes(st, column);
   if (!blob || n < 0 || (size_t)n != expected)
      return -1;
   memcpy(out, blob, expected);
   return 0;
}

int db2_vault_rotation_probe_admit(const char *actor, int64_t rotation_id, const char *owner,
                                   int64_t token, const char *operation_key,
                                   db2_vault_rotation_envelope_t *e)
{
   if (db2_tenant_require_pg() != 0 || !actor || !*actor || rotation_id < 1 || !owner || !*owner ||
       token < 1 || !e || !operation_key || !*operation_key || strlen(operation_key) > 200)
      return -1;
   memset(e, 0, sizeof(*e));
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(), "SELECT * FROM org_vault_rotation_probe_admit(?1,?2,?3,?4,?5)",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", owner);
   aimee_pg_bind_int64(st, "?4", token);
   aimee_pg_bind_text(st, "?5", operation_key);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   e->version = aimee_pg_column_int64(st, 0);
   if (copy_blob_column(st, 1, e->wrapped_dek, sizeof(e->wrapped_dek)) != 0 ||
       copy_blob_column(st, 2, e->nonce, sizeof(e->nonce)) != 0)
      goto fail;
   {
      const void *blob = aimee_pg_column_blob(st, 3);
      int n = aimee_pg_column_bytes(st, 3);
      if (!blob || n < 0 || (size_t)n > sizeof(e->ciphertext))
         goto fail;
      memcpy(e->ciphertext, blob, (size_t)n);
      e->ciphertext_len = (size_t)n;
   }
   if (copy_blob_column(st, 4, e->tag, sizeof(e->tag)) != 0)
      goto fail;
   aimee_pg_finalize(st);
   return 0;
fail:
   aimee_pg_finalize(st);
   memset(e, 0, sizeof(*e));
   return -1;
}

static int claimed_action(const char *sql, const char *actor, int64_t rotation_id,
                          const char *owner, int64_t token, const char *a, const char *b,
                          const char *c)
{
   if (db2_tenant_require_pg() != 0 || !sql || !actor || !*actor || rotation_id < 1 || !owner ||
       !*owner || token < 1 || !a || !*a || !b || !*b)
      return -1;
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", owner);
   aimee_pg_bind_int64(st, "?4", token);
   aimee_pg_bind_text(st, "?5", a ? a : "");
   aimee_pg_bind_text(st, "?6", b ? b : "");
   aimee_pg_bind_text(st, "?7", c ? c : "");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}

int db2_vault_rotation_transition_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                          int64_t token, const char *expected, const char *next,
                                          const char *receipt)
{
   return claimed_action("SELECT org_vault_rotation_transition_claimed(?1,?2,?3,?4,?5,?6,?7)",
                         actor, rotation_id, owner, token, expected, next, receipt);
}

int db2_vault_rotation_fail_claimed(const char *actor, int64_t rotation_id, const char *owner,
                                    int64_t token, const char *expected, const char *phase,
                                    const char *error)
{
   return claimed_action("SELECT org_vault_rotation_fail_claimed(?1,?2,?3,?4,?5,?6,?7)", actor,
                         rotation_id, owner, token, expected, phase, error);
}

int db2_vault_rotation_remediate(const char *actor, int64_t rotation_id, const char *owner,
                                 int64_t token, int64_t anchor_version, const char *evidence)
{
   if (db2_tenant_require_pg() != 0 || !actor || !*actor || rotation_id < 1 || !owner || !*owner ||
       token < 1 || anchor_version < 1 || !evidence || !*evidence ||
       strlen(evidence) > DB2_VAULT_ROTATION_REF_MAX)
      return -1;
   char err[ROT_ERR] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT org_vault_rotation_remediate(?1,?2,?3,?4,?5,?6)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", actor);
   aimee_pg_bind_int64(st, "?2", rotation_id);
   aimee_pg_bind_text(st, "?3", owner);
   aimee_pg_bind_int64(st, "?4", token);
   aimee_pg_bind_int64(st, "?5", anchor_version);
   aimee_pg_bind_text(st, "?6", evidence ? evidence : "");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return step == AIMEE_PG_ROW ? 0 : -1;
}
