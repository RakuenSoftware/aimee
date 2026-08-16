/* db2/org_telemetry.c: P9a telemetry export + content-free ingest — Postgres via
 * libpq. See org_telemetry.h. Mirrors db2/org_rate.c: one prepared call into a
 * SECURITY DEFINER function, the definer's admin-gate RAISE mapped to a sentinel
 * by message text (libpq surfaces the RAISE message, not the SQLSTATE). The
 * ingest returns a STRUCTURED outcome ('stored'|'deduped'|'dropped'), never
 * parsed error text. Tenant-scoped: requires the RLS-enforcing Postgres backend. */

#include "org_telemetry.h"

#include "db2_internal.h"
#include "db2_tenant.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

/* Map an admin-gated definer RAISE to a sentinel. The definer RAISEs
 * "<fn>: admin only" (42501); libpq surfaces the message text. */
static int telemetry_step_err(const char *err)
{
   if (!err)
      return -1;
   if (strstr(err, "admin only") || strstr(err, "not authorized"))
      return DB2_TELEMETRY_ERR_DENIED;
   return -1;
}

int db2_telemetry_ingest(const char *source_event_id, const char *origin_cn, int has_team,
                         int64_t team, const char *event_schema, const char *metric_name,
                         const char *metric_kind, const char *value_text, int64_t ts,
                         char *out_result, int cap)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!source_event_id || !origin_cn || !event_schema || !metric_name || !metric_kind ||
       !value_text || !out_result || cap <= 0)
      return -1;
   out_result[0] = '\0';
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT org_telemetry_ingest(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source_event_id);
   aimee_pg_bind_text(st, "?2", origin_cn);
   if (has_team)
      aimee_pg_bind_int64(st, "?3", team);
   else
      aimee_pg_bind_null(st, "?3");
   aimee_pg_bind_text(st, "?4", event_schema);
   aimee_pg_bind_text(st, "?5", metric_name);
   aimee_pg_bind_text(st, "?6", metric_kind);
   /* NUMERIC arg: bind decimal text so libpq's unknown-type param coerces to numeric. */
   aimee_pg_bind_text(st, "?7", value_text);
   aimee_pg_bind_int64(st, "?8", ts);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   if (step == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      snprintf(out_result, (size_t)cap, "%s", c ? c : "");
   }
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_ROW)
      return telemetry_step_err(err);
   return 0;
}

int db2_telemetry_allow(const char *event_schema, const char *metric_names_array, int enabled)
{
   int g = db2_tenant_require_pg();
   if (g)
      return g;
   if (!event_schema || !event_schema[0] || !metric_names_array)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT org_telemetry_allow(?1, ?2::text[], ?3)", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", event_schema);
   /* The array literal ('{a,b}') coerces to text[] via the ::text[] cast above. */
   aimee_pg_bind_text(st, "?2", metric_names_array);
   aimee_pg_bind_int64(st, "?3", enabled ? 1 : 0);
   aimee_pg_step_t step = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (step == AIMEE_PG_ERR)
      return telemetry_step_err(err);
   return 0;
}

int db2_telemetry_allow_show(db2_telemetry_allow_row_t *out, int max)
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
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT event_schema, metric_names::text, enabled, "
                                          "updated_at FROM org_telemetry_allow_show()",
                                          err, sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   aimee_pg_step_t step = AIMEE_PG_DONE;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      if (n >= max)
         break;
      db2_telemetry_allow_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      const char *c;
      c = aimee_pg_column_text(st, 0);
      snprintf(r->event_schema, sizeof(r->event_schema), "%s", c ? c : "");
      c = aimee_pg_column_text(st, 1);
      snprintf(r->metric_names, sizeof(r->metric_names), "%s", c ? c : "");
      c = aimee_pg_column_is_null(st, 2) ? "" : aimee_pg_column_text(st, 2);
      r->enabled = (c && (c[0] == 't' || c[0] == 'T' || c[0] == '1')) ? 1 : 0;
      c = aimee_pg_column_text(st, 3);
      snprintf(r->updated_at, sizeof(r->updated_at), "%s", c ? c : "");
   }
   int failed = (step == AIMEE_PG_ERR);
   aimee_pg_finalize(st);
   if (failed)
      return telemetry_step_err(err);
   return n;
}

int db2_metrics_snapshot(org_metric_row_t *out, int max)
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
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT metric, team_id, period, model, value::text FROM org_metrics_snapshot()", err,
       sizeof(err));
   if (!st)
      return -1;
   int n = 0;
   int overflow = 0;
   aimee_pg_step_t step = AIMEE_PG_DONE;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      if (n >= max)
      {
         /* More series exist than the buffer holds: refuse rather than emit a
          * silently-incomplete /metrics (a monitoring correctness hazard). */
         overflow = 1;
         break;
      }
      org_metric_row_t *r = &out[n++];
      memset(r, 0, sizeof(*r));
      const char *c = aimee_pg_column_text(st, 0);
      snprintf(r->metric, sizeof(r->metric), "%s", c ? c : "");
      r->team = aimee_pg_column_is_null(st, 1) ? -1 : (long long)aimee_pg_column_int64(st, 1);
      c = aimee_pg_column_is_null(st, 2) ? "" : aimee_pg_column_text(st, 2);
      snprintf(r->period, sizeof(r->period), "%s", c ? c : "");
      c = aimee_pg_column_is_null(st, 3) ? "" : aimee_pg_column_text(st, 3);
      snprintf(r->model, sizeof(r->model), "%s", c ? c : "");
      c = aimee_pg_column_is_null(st, 4) ? "" : aimee_pg_column_text(st, 4);
      snprintf(r->value, sizeof(r->value), "%s", c ? c : "0");
   }
   int failed = (step == AIMEE_PG_ERR);
   aimee_pg_finalize(st);
   if (failed)
      return telemetry_step_err(err);
   if (overflow)
      return DB2_TELEMETRY_ERR_TOOBIG;
   return n;
}
