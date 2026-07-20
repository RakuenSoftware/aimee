/* db2/model_catalog.c: P2a org model catalog + entitlement — Postgres via libpq.
 * See org_model_catalog.h. Mirrors the db2/team.c access pattern. Every mutation goes
 * through the audited SECURITY DEFINER functions (org_catalog_upsert/_remove,
 * org_model_entitle/_unentitle) rather than a raw INSERT, so the WORM audit append is
 * atomic with the mutation in one transaction. Reads go through org_catalog_entitled(),
 * which is actor-bound to aimee.principal (no confused-deputy). Tenant-scoped: requires
 * the RLS-enforcing Postgres backend. */

#include "org_model_catalog.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

int db2_model_catalog_list(db2_model_catalog_row_t *out, int max)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* Admin-only read (RLS p_catalog_admin_read). The operator CLI runs as an admin
    * principal, so this direct SELECT is admitted; the runtime role has no catalog
    * SELECT at all and must use org_catalog_entitled() instead. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT model_id, display_name, provider, wire, endpoint, enabled::int"
       " FROM org_model_catalog ORDER BY model_id",
       err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_model_catalog_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(r->model_id, sizeof(r->model_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 1);
      snprintf(r->display_name, sizeof(r->display_name), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 2);
      snprintf(r->provider, sizeof(r->provider), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(r->wire, sizeof(r->wire), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 4);
      snprintf(r->endpoint, sizeof(r->endpoint), "%s", c ? c : "");
      r->enabled = aimee_pg_column_int64(st, 5) ? 1 : 0;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_model_entitled_list(db2_model_entitled_row_t *out, int max)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   /* org_catalog_entitled() reads current_setting('aimee.principal') itself — no arg,
    * so a caller can never nominate another principal's memberships. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT model_id, display_name, provider, wire, endpoint FROM org_catalog_entitled()",
       err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_model_entitled_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(r->model_id, sizeof(r->model_id), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 1);
      snprintf(r->display_name, sizeof(r->display_name), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 2);
      snprintf(r->provider, sizeof(r->provider), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 3);
      snprintf(r->wire, sizeof(r->wire), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 4);
      snprintf(r->endpoint, sizeof(r->endpoint), "%s", c ? c : "");
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_model_catalog_upsert(const char *model_id, const char *display_name, const char *provider,
                             const char *wire, const char *endpoint, int enabled, int64_t *out_id)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!model_id || !model_id[0] || !provider || !provider[0] || !wire || !wire[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT org_catalog_upsert(?1, ?2, ?3, ?4, ?5, ?6)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", model_id);
   aimee_pg_bind_text(st, "?2", display_name ? display_name : "");
   aimee_pg_bind_text(st, "?3", provider);
   aimee_pg_bind_text(st, "?4", wire);
   aimee_pg_bind_text(st, "?5", endpoint ? endpoint : "");
   /* The 6th arg is BOOLEAN; bind a text literal ('true'/'false') so libpq's
    * unknown-type param coerces to boolean (there is no implicit integer->boolean
    * cast in a function-argument position). */
   aimee_pg_bind_text(st, "?6", enabled ? "true" : "false");
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t id = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;
   if (out_id)
      *out_id = id;
   return 0;
}

int db2_model_catalog_remove(const char *model_id, int64_t *out_removed)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!model_id || !model_id[0])
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_catalog_remove(?1)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", model_id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t removed = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;
   if (out_removed)
      *out_removed = removed;
   return 0;
}

static int model_ent_op(const char *sql, const char *model_id, int64_t team_id, int64_t *out)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!model_id || !model_id[0] || team_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", model_id);
   aimee_pg_bind_int64(st, "?2", team_id);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   int64_t v = (step == AIMEE_PG_ROW) ? aimee_pg_column_int64(st, 0) : 0;
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return -1;
   if (out)
      *out = v;
   return 0;
}

int db2_model_entitle(const char *model_id, int64_t team_id, int64_t *out_id)
{
   return model_ent_op("SELECT org_model_entitle(?1, ?2)", model_id, team_id, out_id);
}

int db2_model_unentitle(const char *model_id, int64_t team_id, int64_t *out_removed)
{
   return model_ent_op("SELECT org_model_unentitle(?1, ?2)", model_id, team_id, out_removed);
}
