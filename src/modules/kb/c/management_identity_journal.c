/* management_identity_journal.c — Postgres side of the login seam. See
 * management_identity_journal.h for the two structural properties this upholds:
 * no subject parameter, and filing an intent authorizes nothing. */

#include "management_identity_journal.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"
#include "management_intent_fields.h"

#include <string.h>

const char *db2_identity_auth_mode_str(db2_identity_auth_mode_t mode)
{
   switch (mode)
   {
   case DB2_IDENTITY_AUTH_MODE_OIDC:
      return "oidc";
   case DB2_IDENTITY_AUTH_MODE_PAM:
      return "pam";
   }
   return NULL;
}

static int operation_valid(const db2_identity_intent_operation_t *o)
{
   return o && db2_intent_fixed_hex(o->correlation_id, sizeof(o->correlation_id), 64) &&
          db2_intent_fixed_hex(o->jti, sizeof(o->jti), 64) &&
          /* The token jti is a token-charset string of at least 8 chars, matching
           * the intent CHECK. We always generate 64 hex, but a record that came
           * from elsewhere still has to satisfy the schema. */
          db2_intent_fixed_text(o->token_jti, sizeof(o->token_jti), DB2_IDENTITY_TOKEN_JTI_MAX,
                                1) &&
          strnlen(o->token_jti, sizeof(o->token_jti)) >= 8 && o->team_id > 0 &&
          db2_identity_auth_mode_str(o->auth_mode) &&
          db2_intent_fixed_text(o->target_server_id, sizeof(o->target_server_id),
                                DB2_IDENTITY_SERVER_MAX, 1) &&
          db2_intent_fixed_text(o->token_issuer, sizeof(o->token_issuer),
                                DB2_IDENTITY_TOKEN_ISSUER_MAX, 0) &&
          db2_intent_fixed_text(o->kid, sizeof(o->kid), DB2_IDENTITY_KID_MAX, 1) &&
          o->ttl_seconds >= 1 && o->ttl_seconds <= DB2_IDENTITY_TTL_MAX_SECONDS &&
          db2_intent_fixed_hex(o->installation_id, sizeof(o->installation_id), 32);
}

static int parse_auth_mode(const char *s, db2_identity_auth_mode_t *out)
{
   if (s && !strcmp(s, "oidc"))
      *out = DB2_IDENTITY_AUTH_MODE_OIDC;
   else if (s && !strcmp(s, "pam"))
      *out = DB2_IDENTITY_AUTH_MODE_PAM;
   else
      return -1;
   return 0;
}

/* Column order is kb_management_identity_intent_start's RETURNS TABLE. The
 * count check is what makes a schema change a loud decode failure instead of a
 * silent shift of every field by one. */
static int decode_intent(aimee_pg_stmt_t *st, db2_identity_intent_t *o)
{
   if (aimee_pg_column_count(st) != 19 || db2_intent_col_bool(st, 0, &o->replayed) ||
       db2_intent_copy_hex_col(st, 1, o->correlation_id, 64) ||
       db2_intent_copy_hex_col(st, 2, o->jti, 64) ||
       db2_intent_copy_col(st, 3, o->token_jti, sizeof(o->token_jti), DB2_IDENTITY_TOKEN_JTI_MAX,
                           1) ||
       strnlen(o->token_jti, sizeof(o->token_jti)) < 8 || db2_intent_col_i64(st, 4, &o->team_id) ||
       o->team_id < 1 ||
       db2_intent_copy_col(st, 5, o->subject, sizeof(o->subject), DB2_IDENTITY_SUBJECT_MAX, 0) ||
       /* The recorded subject must be a canonical identity key. It came from
        * aimee.principal, so a value that is not one means the tenant scope was
        * set from something that never went through a verifier. */
       !db2_intent_canonical_actor(o->subject, sizeof(o->subject)) ||
       parse_auth_mode(aimee_pg_column_text(st, 6), &o->auth_mode) ||
       db2_intent_copy_col(st, 7, o->target_server_id, sizeof(o->target_server_id),
                           DB2_IDENTITY_SERVER_MAX, 1) ||
       db2_intent_copy_col(st, 8, o->token_issuer, sizeof(o->token_issuer),
                           DB2_IDENTITY_TOKEN_ISSUER_MAX, 0) ||
       db2_intent_copy_col(st, 9, o->audience, sizeof(o->audience), DB2_IDENTITY_SERVER_MAX, 1) ||
       db2_intent_copy_col(st, 10, o->kid, sizeof(o->kid), DB2_IDENTITY_KID_MAX, 1) ||
       db2_intent_col_i64(st, 11, &o->issued_at) || db2_intent_col_i64(st, 12, &o->expires_at) ||
       o->issued_at < 1 || o->expires_at <= o->issued_at ||
       o->expires_at - o->issued_at > DB2_IDENTITY_TTL_MAX_SECONDS ||
       db2_intent_copy_hex_col(st, 13, o->installation_id, 32) ||
       db2_intent_col_i64(st, 14, &o->installation_generation) || o->installation_generation < 1 ||
       db2_intent_col_i64(st, 15, &o->installation_enrollment_id) ||
       o->installation_enrollment_id < 1 || db2_intent_col_i64(st, 16, &o->target_enrollment_id) ||
       o->target_enrollment_id < 1 || db2_intent_col_i64(st, 17, &o->revocation_generation) ||
       o->revocation_generation < 1 || db2_intent_col_i64(st, 18, &o->created_at_epoch) ||
       o->created_at_epoch < 1)
      return -1;
   /* The audience IS the target server; §4 makes the server's aud check
    * meaningful only if they can never diverge. */
   return strcmp(o->audience, o->target_server_id) ? -1 : 0;
}

static db2_management_action_result_t first_row(aimee_pg_stmt_t *st)
{
   char err[256] = "";
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ERR)
      return db2_management_action_classify_sqlstate(aimee_pg_sqlstate(st));
   return step == AIMEE_PG_ROW ? DB2_MANAGEMENT_ACTION_OK : DB2_MANAGEMENT_ACTION_INTEGRITY;
}

static db2_management_action_result_t finish_row(aimee_pg_stmt_t *st, int valid)
{
   char err[256] = "";
   db2_management_action_result_t rc =
       valid ? DB2_MANAGEMENT_ACTION_OK : DB2_MANAGEMENT_ACTION_INTEGRITY;
   if (valid)
   {
      aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
      if (step == AIMEE_PG_ERR)
         rc = db2_management_action_classify_sqlstate(aimee_pg_sqlstate(st));
      else if (step != AIMEE_PG_DONE)
         rc = DB2_MANAGEMENT_ACTION_INTEGRITY;
   }
   aimee_pg_finalize(st);
   return rc;
}

db2_management_action_result_t
db2_identity_intent_operation_init(int64_t team_id, const char *target_server_id,
                                   db2_identity_auth_mode_t auth_mode, const char *token_issuer,
                                   const char *kid, int ttl_seconds, const char *installation_id,
                                   db2_identity_intent_operation_t *out)
{
   if (!out)
      return DB2_MANAGEMENT_ACTION_INVALID;
   memset(out, 0, sizeof(*out));
   if (team_id < 1 || !db2_identity_auth_mode_str(auth_mode) || ttl_seconds < 1 ||
       ttl_seconds > DB2_IDENTITY_TTL_MAX_SECONDS ||
       !db2_intent_input_text(target_server_id, DB2_IDENTITY_SERVER_MAX, 1) ||
       !db2_intent_input_text(token_issuer, DB2_IDENTITY_TOKEN_ISSUER_MAX, 0) ||
       !db2_intent_input_text(kid, DB2_IDENTITY_KID_MAX, 1) ||
       !db2_intent_input_hex(installation_id, 32))
      return DB2_MANAGEMENT_ACTION_INVALID;

   db2_identity_intent_operation_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   candidate.team_id = team_id;
   candidate.auth_mode = auth_mode;
   candidate.ttl_seconds = ttl_seconds;
   memcpy(candidate.target_server_id, target_server_id, strlen(target_server_id));
   memcpy(candidate.token_issuer, token_issuer, strlen(token_issuer));
   memcpy(candidate.kid, kid, strlen(kid));
   memcpy(candidate.installation_id, installation_id, 32);
   /* Three independent identifiers: the namespace pair that admits exactly one
    * private-key use, and the token's own jti, which is what the server spends
    * on replay. Deriving one from another would let a caller who saw a token
    * predict the handle that minted it. */
   if (db2_intent_generate_id(candidate.correlation_id) || db2_intent_generate_id(candidate.jti) ||
       db2_intent_generate_id(candidate.token_jti))
   {
      memset(&candidate, 0, sizeof(candidate));
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   *out = candidate;
   return DB2_MANAGEMENT_ACTION_OK;
}

db2_management_action_result_t db2_identity_login_context(const kb_principal_t *principal,
                                                          int64_t team_id, char installation_id[33],
                                                          char kid[DB2_IDENTITY_KID_MAX + 1])
{
   if (installation_id)
      installation_id[0] = '\0';
   if (kid)
      kid[0] = '\0';
   if (!principal || !installation_id || !kid || team_id < 1)
      return DB2_MANAGEMENT_ACTION_INVALID;

   /* The same tenant scope the writer opens, for the same reason: it refuses an
    * unauthenticated principal and sets aimee.principal/aimee.team, which is
    * where the SQL function reads the actor from. */
   int tx = db2_tenant_scope_begin(principal, team_id);
   if (tx != 0)
      return (tx == DB2_ERR_TENANT_DENIED || tx == DB2_ERR_TENANT_UNAUTHENTICATED)
                 ? DB2_MANAGEMENT_ACTION_DENIED
                 : DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(
       db2_conn(), "SELECT * FROM public.kb_management_identity_login_context(?1)", &kind, err,
       sizeof(err));
   if (!st)
   {
      db2_tenant_scope_rollback();
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   char inst[33] = "", wire[DB2_IDENTITY_KID_MAX + 1] = "";
   db2_management_action_result_t rc =
       aimee_pg_bind_int64(st, "?1", team_id) ? DB2_MANAGEMENT_ACTION_UNAVAILABLE : first_row(st);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      aimee_pg_finalize(st);
      db2_tenant_scope_rollback();
      return rc;
   }
   const char *c0 = aimee_pg_column_text(st, 0), *c1 = aimee_pg_column_text(st, 1);
   int ok = c0 && c1 && db2_intent_input_hex(c0, 32) &&
            db2_intent_input_text(c1, DB2_IDENTITY_KID_MAX, 1);
   if (ok)
   {
      snprintf(inst, sizeof(inst), "%s", c0);
      snprintf(wire, sizeof(wire), "%s", c1);
   }
   rc = finish_row(st, ok);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      db2_tenant_scope_rollback();
      return rc;
   }
   /* A read-only scope, so a failed commit costs nothing and is not ambiguous in
    * the way the writer's is — there is no row that may or may not exist. */
   if (db2_tenant_scope_commit() != 0)
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   snprintf(installation_id, 33, "%s", inst);
   snprintf(kid, DB2_IDENTITY_KID_MAX + 1, "%s", wire);
   return DB2_MANAGEMENT_ACTION_OK;
}

db2_management_action_result_t db2_identity_intent_start(const kb_principal_t *principal,
                                                         const db2_identity_intent_operation_t *op,
                                                         db2_identity_intent_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out || !operation_valid(op) || !principal)
      return DB2_MANAGEMENT_ACTION_INVALID;
   /* The scope carries the subject: db2_tenant_scope_begin refuses an
    * unauthenticated principal and sets aimee.principal/aimee.team, which is
    * where the SQL function reads the subject from. There is nowhere for a
    * caller to inject a different one. */
   int tx = db2_tenant_scope_begin(principal, op->team_id);
   if (tx != 0)
      return (tx == DB2_ERR_TENANT_DENIED || tx == DB2_ERR_TENANT_UNAUTHENTICATED)
                 ? DB2_MANAGEMENT_ACTION_DENIED
                 : DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   char err[256] = "";
   aimee_pg_prepare_error_t kind = AIMEE_PG_PREPARE_OK;
   aimee_pg_stmt_t *st = aimee_pg_prepare_ex(
       db2_conn(),
       "SELECT * FROM public.kb_management_identity_intent_start(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)",
       &kind, err, sizeof(err));
   if (!st)
   {
      db2_tenant_scope_rollback();
      return DB2_MANAGEMENT_ACTION_UNAVAILABLE;
   }
   int bound =
       aimee_pg_bind_text(st, "?1", op->correlation_id) || aimee_pg_bind_text(st, "?2", op->jti) ||
       aimee_pg_bind_text(st, "?3", op->token_jti) || aimee_pg_bind_int64(st, "?4", op->team_id) ||
       aimee_pg_bind_text(st, "?5", op->target_server_id) ||
       aimee_pg_bind_text(st, "?6", db2_identity_auth_mode_str(op->auth_mode)) ||
       aimee_pg_bind_text(st, "?7", op->token_issuer) || aimee_pg_bind_text(st, "?8", op->kid) ||
       aimee_pg_bind_int(st, "?9", op->ttl_seconds) ||
       aimee_pg_bind_text(st, "?10", op->installation_id);
   db2_identity_intent_t candidate;
   memset(&candidate, 0, sizeof(candidate));
   db2_management_action_result_t rc = bound ? DB2_MANAGEMENT_ACTION_UNAVAILABLE : first_row(st);
   if (bound)
      aimee_pg_finalize(st);
   else if (rc == DB2_MANAGEMENT_ACTION_OK)
      /* Every field the caller asked for is compared against what came back, so
       * a replay that returns someone else's row is an INTEGRITY failure rather
       * than a token minted under the wrong claims. */
      rc = finish_row(
          st, decode_intent(st, &candidate) == 0 &&
                  !strcmp(candidate.correlation_id, op->correlation_id) &&
                  !strcmp(candidate.jti, op->jti) && !strcmp(candidate.token_jti, op->token_jti) &&
                  candidate.team_id == op->team_id && candidate.auth_mode == op->auth_mode &&
                  !strcmp(candidate.target_server_id, op->target_server_id) &&
                  !strcmp(candidate.token_issuer, op->token_issuer) &&
                  !strcmp(candidate.kid, op->kid) &&
                  candidate.expires_at - candidate.issued_at == op->ttl_seconds &&
                  !strcmp(candidate.installation_id, op->installation_id));
   else
      aimee_pg_finalize(st);
   if (rc != DB2_MANAGEMENT_ACTION_OK)
   {
      db2_tenant_scope_rollback();
      return rc;
   }
   if (db2_tenant_scope_commit() != 0)
      return DB2_MANAGEMENT_ACTION_COMMIT_AMBIGUOUS;
   *out = candidate;
   return DB2_MANAGEMENT_ACTION_OK;
}
