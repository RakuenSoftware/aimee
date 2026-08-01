/* db2/org_egress.c: typed libpq access to P2b-a SECURITY DEFINER operations. */
#include "org_egress.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <string.h>

static int egress_error(const aimee_pg_stmt_t *st)
{
   const char *state = aimee_pg_sqlstate(st);
   if (state && strcmp(state, "42501") == 0)
      return DB2_EGRESS_ERR_DENIED;
   if (state && strcmp(state, "23505") == 0)
      return DB2_EGRESS_ERR_CONFLICT;
   return -1;
}

static int bind_common_guard(void)
{
   return db2_tenant_require_pg();
}

int db2_org_egress_admit(const char *authority_id, const char *fingerprint, const char *issuer,
                         const char *serial, const char *origin, const char *request_id,
                         int64_t team, int has_project, int64_t project, const char *model_id,
                         const char *digest, int64_t lease_secs, db2_org_egress_admission_t *out)
{
   int g = bind_common_guard();
   if (g)
      return g;
   if (!authority_id || strlen(authority_id) != 32 || !fingerprint || strlen(fingerprint) != 64 ||
       !issuer || !issuer[0] || !serial || !serial[0] || !origin || !origin[0] || !request_id ||
       strlen(request_id) != 36 || team < 1 || !model_id || !model_id[0] || !digest ||
       strlen(digest) != 64 || lease_secs < 1 || lease_secs > 300 || !out)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT outcome,dispatch_id,dispatch_state,reserved_max,billable_model,"
       "pricing_version,key_id,vault_principal,vault_agent,vault_cred,max_input,max_output "
       "FROM org_egress_admit(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", authority_id);
   aimee_pg_bind_text(st, "?2", fingerprint);
   aimee_pg_bind_text(st, "?3", issuer);
   aimee_pg_bind_text(st, "?4", serial);
   aimee_pg_bind_text(st, "?5", origin);
   aimee_pg_bind_text(st, "?6", request_id);
   aimee_pg_bind_int64(st, "?7", team);
   if (has_project)
      aimee_pg_bind_int64(st, "?8", project);
   else
      aimee_pg_bind_null(st, "?8");
   aimee_pg_bind_text(st, "?9", model_id);
   aimee_pg_bind_text(st, "?10", digest);
   aimee_pg_bind_int64(st, "?11", lease_secs);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      int rc = egress_error(st);
      aimee_pg_finalize(st);
      return rc;
   }
   memset(out, 0, sizeof(*out));
   const char *verdict = aimee_pg_column_text(st, 0);
   if (verdict && strcmp(verdict, "admitted") == 0)
      out->outcome = DB2_EGRESS_ADMITTED;
   else if (verdict && strcmp(verdict, "replay") == 0)
      out->outcome = DB2_EGRESS_REPLAY;
   else if (verdict && strcmp(verdict, "rate_refused") == 0)
      out->outcome = DB2_EGRESS_RATE_REFUSED;
   else if (verdict && strcmp(verdict, "budget_refused") == 0)
      out->outcome = DB2_EGRESS_BUDGET_REFUSED;
   else
   {
      aimee_pg_finalize(st);
      return -1;
   }
   if (!aimee_pg_column_is_null(st, 1))
      out->dispatch_id = aimee_pg_column_int64(st, 1);
   db2_copy_col_text(out->state, sizeof(out->state), st, 2);
   db2_copy_col_text(out->reserved_max_usd, sizeof(out->reserved_max_usd), st, 3);
   db2_copy_col_text(out->billable_model, sizeof(out->billable_model), st, 4);
   out->pricing_version = aimee_pg_column_int64(st, 5);
   db2_copy_col_text(out->key_id, sizeof(out->key_id), st, 6);
   db2_copy_col_text(out->vault_principal, sizeof(out->vault_principal), st, 7);
   db2_copy_col_text(out->vault_agent, sizeof(out->vault_agent), st, 8);
   db2_copy_col_text(out->vault_cred, sizeof(out->vault_cred), st, 9);
   out->max_input_tokens = aimee_pg_column_int64(st, 10);
   out->max_output_tokens = aimee_pg_column_int64(st, 11);
   aimee_pg_finalize(st);
   return 0;
}

int db2_org_egress_begin(const char *authority_id, const char *request_id, const char *owner_token,
                         const char *instance_id, int64_t ttl_secs, int64_t *out_id,
                         int64_t *out_generation)
{
   int g = bind_common_guard();
   if (g)
      return g;
   if (!authority_id || strlen(authority_id) != 32 || !request_id || strlen(request_id) != 36 ||
       !owner_token || strlen(owner_token) != 32 || !instance_id || !instance_id[0] ||
       ttl_secs < 1 || ttl_secs > 300)
      return -1;
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st = conn ? aimee_pg_prepare(conn,
                                                 "SELECT dispatch_id,owner_generation FROM "
                                                 "org_egress_dispatch_begin(?1,?2,?3,?4,?5)",
                                                 err, sizeof(err))
                              : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", authority_id);
   aimee_pg_bind_text(st, "?2", request_id);
   aimee_pg_bind_text(st, "?3", owner_token);
   aimee_pg_bind_text(st, "?4", instance_id);
   aimee_pg_bind_int64(st, "?5", ttl_secs);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int rc = step == AIMEE_PG_ROW ? 0 : (step == AIMEE_PG_DONE ? 1 : egress_error(st));
   if (step == AIMEE_PG_ROW)
   {
      if (out_id)
         *out_id = aimee_pg_column_int64(st, 0);
      if (out_generation)
         *out_generation = aimee_pg_column_int64(st, 1);
   }
   aimee_pg_finalize(st);
   return rc;
}

static int bool_call_4(const char *sql, int64_t id, const char *token, int64_t generation,
                       int64_t arg4, int *out_ok)
{
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st = conn ? aimee_pg_prepare(conn, sql, err, sizeof(err)) : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_bind_text(st, "?2", token);
   aimee_pg_bind_int64(st, "?3", generation);
   aimee_pg_bind_int64(st, "?4", arg4);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   const char *v = step == AIMEE_PG_ROW ? aimee_pg_column_text(st, 0) : NULL;
   int ok = v && (v[0] == 't' || v[0] == '1');
   int rc = step == AIMEE_PG_ROW ? 0 : egress_error(st);
   aimee_pg_finalize(st);
   if (rc)
      return rc;
   if (out_ok)
      *out_ok = ok;
   return 0;
}

int db2_org_egress_heartbeat(int64_t id, const char *owner_token, int64_t generation,
                             int64_t ttl_secs, int *out_ok)
{
   int g = bind_common_guard();
   if (g)
      return g;
   if (id < 1 || !owner_token || strlen(owner_token) != 32 || generation < 1 || ttl_secs < 1 ||
       ttl_secs > 300)
      return -1;
   return bool_call_4("SELECT org_egress_dispatch_heartbeat(?1,?2,?3,?4)", id, owner_token,
                      generation, ttl_secs, out_ok);
}

int db2_org_egress_owner_guard(int64_t id, const char *owner_token, int64_t generation, int *out_ok)
{
   int g = bind_common_guard();
   if (g)
      return g;
   if (id < 1 || !owner_token || strlen(owner_token) != 32 || generation < 1)
      return -1;
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(conn, "SELECT org_egress_dispatch_owner_guard(?1,?2,?3)", err,
                               sizeof(err))
            : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_bind_text(st, "?2", owner_token);
   aimee_pg_bind_int64(st, "?3", generation);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   const char *v = step == AIMEE_PG_ROW ? aimee_pg_column_text(st, 0) : NULL;
   int ok = v && (v[0] == 't' || v[0] == '1');
   int rc = step == AIMEE_PG_ROW ? 0 : egress_error(st);
   aimee_pg_finalize(st);
   if (rc)
      return rc;
   if (out_ok)
      *out_ok = ok;
   return 0;
}

int db2_org_egress_settle(int64_t id, const char *owner_token, int64_t generation,
                          const char *state, int http_status, int64_t prompt_tokens,
                          int64_t completion_tokens, int64_t cache_read_tokens,
                          int64_t cache_write_tokens, const char *outcome_class,
                          const char *settlement_basis, int *out_ok)
{
   int g = bind_common_guard();
   if (g)
      return g;
   if (id < 1 || !owner_token || strlen(owner_token) != 32 || generation < 1 || !state ||
       http_status < 0 || http_status > 599 || prompt_tokens < 0 || completion_tokens < 0 ||
       cache_read_tokens < 0 || cache_write_tokens < 0 || !outcome_class || !settlement_basis ||
       (strcmp(settlement_basis, "actual") != 0 && strcmp(settlement_basis, "zero") != 0 &&
        strcmp(settlement_basis, "reservation") != 0))
      return -1;
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(
                  conn, "SELECT org_egress_dispatch_settle(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
                  err, sizeof(err))
            : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", id);
   aimee_pg_bind_text(st, "?2", owner_token);
   aimee_pg_bind_int64(st, "?3", generation);
   aimee_pg_bind_text(st, "?4", state);
   aimee_pg_bind_int64(st, "?5", http_status);
   aimee_pg_bind_int64(st, "?6", prompt_tokens);
   aimee_pg_bind_int64(st, "?7", completion_tokens);
   aimee_pg_bind_int64(st, "?8", cache_read_tokens);
   aimee_pg_bind_int64(st, "?9", cache_write_tokens);
   aimee_pg_bind_text(st, "?10", outcome_class);
   aimee_pg_bind_text(st, "?11", settlement_basis);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   const char *v = step == AIMEE_PG_ROW ? aimee_pg_column_text(st, 0) : NULL;
   int ok = v && (v[0] == 't' || v[0] == '1');
   int rc = step == AIMEE_PG_ROW ? 0 : egress_error(st);
   aimee_pg_finalize(st);
   if (rc)
      return rc;
   if (out_ok)
      *out_ok = ok;
   return 0;
}

int db2_org_egress_recover(int limit, int64_t *out_count)
{
   int g = bind_common_guard();
   if (g)
      return g;
   if (limit < 1 || limit > 100)
      return -1;
   void *conn = db2_conn();
   char err[256] = "";
   aimee_pg_stmt_t *st =
       conn ? aimee_pg_prepare(conn, "SELECT org_egress_recover(?1)", err, sizeof(err)) : NULL;
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", limit);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW && out_count)
      *out_count = aimee_pg_column_int64(st, 0);
   int rc = step == AIMEE_PG_ROW ? 0 : egress_error(st);
   aimee_pg_finalize(st);
   return rc;
}
