/* db1/token_audit.c: per-machine LLM token usage audit log. */

#include "token_audit.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Spend readers report REALIZED spend only: estimated (no provider usage),
 * avoided (dedup-skipped), and partial (aborted mid-stream) rows are carried for
 * reporting but are not billable, so they must never inflate spend totals or
 * per-dimension breakdowns. Legacy rows have usage_kind defaulting to 'realized'
 * (and the insert helper binds 'realized' for an empty value), so an empty/NULL
 * value is treated as realized. The full breakdown (db1_token_audit_spend_breakdown)
 * deliberately does NOT use this — it reports every kind separately. */
#define TA_REALIZED "(usage_kind = 'realized' OR usage_kind = '' OR usage_kind IS NULL)"

void db1_token_audit_ensure_idem_index(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return;
   /* Partial unique index: only rows with a real request_id are constrained, so
    * the many internal rows (empty request_id) are unaffected and existing rows
    * (all empty request_id before this column existed) never collide. The
    * request_id/attempt columns are added by db1_reconcile_columns at init; this
    * runs after, lazily, on the first insert. */
   sqlite3_exec(db,
                "CREATE UNIQUE INDEX IF NOT EXISTS idx_token_audit_idem"
                " ON token_audit(source, request_id, attempt) WHERE request_id != ''",
                NULL, NULL, NULL);
}

int db1_token_audit_insert(const db1_token_audit_row_t *row)
{
   if (!row)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* Lazily ensure the idempotency index exists before the first OR-IGNORE
    * insert relies on it. CREATE INDEX IF NOT EXISTS is idempotent, so the
    * benign race on the best-effort flag is safe. Keeps the index creation
    * self-contained in token_audit.o (no db1_init -> token_audit link edge). */
   static int g_idem_index_ready = 0;
   if (!g_idem_index_ready)
   {
      db1_token_audit_ensure_idem_index();
      g_idem_index_ready = 1;
   }

   sqlite3_stmt *stmt = NULL;
   /* OR IGNORE so a duplicate (source, request_id, attempt) — a retried or
    * late-finalized provider call — is silently dropped rather than double-counted.
    * The partial unique index only constrains rows with a non-empty request_id;
    * internal rows (empty request_id) are never deduped. */
   static const char *sql =
       "INSERT OR IGNORE INTO token_audit"
       " (session_id, delegation_id, project_name, tool_name, role, model, source,"
       "  requested_model, stop_reason, usage_kind, agent_log_id,"
       "  request_id, attempt, principal,"
       "  prompt_tokens, completion_tokens, cache_write_tokens, cache_read_tokens,"
       "  estimated_cost_usd)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, row->session_id ? row->session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, row->delegation_id ? row->delegation_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, row->project_name ? row->project_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, row->tool_name ? row->tool_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, row->role ? row->role : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, row->model ? row->model : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 7, row->source ? row->source : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 8, row->requested_model ? row->requested_model : "", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 9, row->stop_reason ? row->stop_reason : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 10, row->usage_kind ? row->usage_kind : "realized", -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 11, row->agent_log_id);
   sqlite3_bind_text(stmt, 12, row->request_id ? row->request_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 13, row->attempt);
   sqlite3_bind_text(stmt, 14, row->principal ? row->principal : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 15, row->prompt_tokens);
   sqlite3_bind_int(stmt, 16, row->completion_tokens);
   sqlite3_bind_int(stmt, 17, row->cache_write_tokens);
   sqlite3_bind_int(stmt, 18, row->cache_read_tokens);
   sqlite3_bind_double(stmt, 19, row->estimated_cost_usd);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

double db1_token_audit_cost_for_delegation(const char *delegation_id)
{
   if (!delegation_id || !delegation_id[0])
      return 0.0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0.0;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT COALESCE(SUM(estimated_cost_usd), 0.0)"
                            " FROM token_audit WHERE delegation_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0.0;
   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   double total = 0.0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      total = sqlite3_column_double(stmt, 0);
   sqlite3_finalize(stmt);
   return total;
}

int db1_token_audit_totals(int since_hours, db1_token_audit_totals_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql_recent = "SELECT COUNT(*), COALESCE(SUM(prompt_tokens), 0),"
                                   " COALESCE(SUM(completion_tokens), 0),"
                                   " COALESCE(SUM(cache_write_tokens), 0),"
                                   " COALESCE(SUM(cache_read_tokens), 0),"
                                   " COALESCE(SUM(estimated_cost_usd), 0.0)"
                                   " FROM token_audit"
                                   " WHERE " TA_REALIZED " AND created_at >= datetime('now', ?)";
   static const char *sql_all = "SELECT COUNT(*), COALESCE(SUM(prompt_tokens), 0),"
                                " COALESCE(SUM(completion_tokens), 0),"
                                " COALESCE(SUM(cache_write_tokens), 0),"
                                " COALESCE(SUM(cache_read_tokens), 0),"
                                " COALESCE(SUM(estimated_cost_usd), 0.0)"
                                " FROM token_audit"
                                " WHERE " TA_REALIZED;
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, 1, since, -1, SQLITE_TRANSIENT);
   }

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->total_calls = sqlite3_column_int(stmt, 0);
      out->prompt_tokens = sqlite3_column_int64(stmt, 1);
      out->completion_tokens = sqlite3_column_int64(stmt, 2);
      out->cache_write_tokens = sqlite3_column_int64(stmt, 3);
      out->cache_read_tokens = sqlite3_column_int64(stmt, 4);
      out->estimated_cost_usd = sqlite3_column_double(stmt, 5);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_token_audit_spend_breakdown(int since_hours, db1_token_audit_spend_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql_recent =
       "SELECT usage_kind, COALESCE(SUM(estimated_cost_usd), 0.0) FROM token_audit"
       " WHERE created_at >= datetime('now', ?) GROUP BY usage_kind";
   static const char *sql_all =
       "SELECT usage_kind, COALESCE(SUM(estimated_cost_usd), 0.0) FROM token_audit"
       " GROUP BY usage_kind";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, 1, since, -1, SQLITE_TRANSIENT);
   }

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *kind = sqlite3_column_text(stmt, 0);
      double cost = sqlite3_column_double(stmt, 1);
      const char *k = kind ? (const char *)kind : "";
      /* Empty/legacy usage_kind defaults to realized (matches the column
       * default and the insert helper). */
      if (k[0] == '\0' || strcmp(k, "realized") == 0)
         out->realized_cost_usd += cost;
      else if (strcmp(k, "estimated") == 0)
         out->estimated_cost_usd += cost;
      else if (strcmp(k, "avoided") == 0)
         out->avoided_cost_usd += cost;
      else if (strcmp(k, "partial") == 0)
         out->partial_cost_usd += cost;
      else
         out->realized_cost_usd += cost; /* unknown kind -> conservative: spend */
   }
   sqlite3_finalize(stmt);

   /* Billable spend is realized only; estimated/avoided/partial are reported
    * separately and never folded into the total. */
   out->spend_cost_usd = out->realized_cost_usd;
   return 0;
}

int db1_token_audit_by_role(int since_hours, db1_token_audit_role_summary_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql_recent = "SELECT COALESCE(role, ''), COUNT(*),"
                                   " COALESCE(SUM(prompt_tokens), 0),"
                                   " COALESCE(SUM(completion_tokens), 0),"
                                   " COALESCE(SUM(estimated_cost_usd), 0.0)"
                                   " FROM token_audit"
                                   " WHERE " TA_REALIZED " AND created_at >= datetime('now', ?)"
                                   " GROUP BY role"
                                   " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
                                   "          COALESCE(SUM(completion_tokens), 0) DESC"
                                   " LIMIT ?";
   static const char *sql_all = "SELECT COALESCE(role, ''), COUNT(*),"
                                " COALESCE(SUM(prompt_tokens), 0),"
                                " COALESCE(SUM(completion_tokens), 0),"
                                " COALESCE(SUM(estimated_cost_usd), 0.0)"
                                " FROM token_audit"
                                " WHERE " TA_REALIZED " GROUP BY role"
                                " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
                                "          COALESCE(SUM(completion_tokens), 0) DESC"
                                " LIMIT ?";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, param++, since, -1, SQLITE_TRANSIENT);
   }
   sqlite3_bind_int(stmt, param, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 0);
      out[n].calls = sqlite3_column_int(stmt, 1);
      out[n].prompt_tokens = sqlite3_column_int64(stmt, 2);
      out[n].completion_tokens = sqlite3_column_int64(stmt, 3);
      out[n].estimated_cost_usd = sqlite3_column_double(stmt, 4);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_token_audit_by_tool(int since_hours, db1_token_audit_tool_summary_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql_recent = "SELECT COALESCE(tool_name, ''), COUNT(*),"
                                   " COALESCE(SUM(prompt_tokens), 0),"
                                   " COALESCE(SUM(completion_tokens), 0),"
                                   " COALESCE(SUM(estimated_cost_usd), 0.0)"
                                   " FROM token_audit"
                                   " WHERE " TA_REALIZED " AND created_at >= datetime('now', ?)"
                                   " GROUP BY tool_name"
                                   " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
                                   "          COALESCE(SUM(completion_tokens), 0) DESC"
                                   " LIMIT ?";
   static const char *sql_all = "SELECT COALESCE(tool_name, ''), COUNT(*),"
                                " COALESCE(SUM(prompt_tokens), 0),"
                                " COALESCE(SUM(completion_tokens), 0),"
                                " COALESCE(SUM(estimated_cost_usd), 0.0)"
                                " FROM token_audit"
                                " WHERE " TA_REALIZED " GROUP BY tool_name"
                                " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
                                "          COALESCE(SUM(completion_tokens), 0) DESC"
                                " LIMIT ?";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, param++, since, -1, SQLITE_TRANSIENT);
   }
   sqlite3_bind_int(stmt, param, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].tool_name, sizeof(out[n].tool_name), stmt, 0);
      out[n].calls = sqlite3_column_int(stmt, 1);
      out[n].prompt_tokens = sqlite3_column_int64(stmt, 2);
      out[n].completion_tokens = sqlite3_column_int64(stmt, 3);
      out[n].estimated_cost_usd = sqlite3_column_double(stmt, 4);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_token_audit_by_model(int since_hours, db1_token_audit_model_summary_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* Empty-model rows (legacy entries written before the model was tracked)
    * are surfaced as "(unattributed)" rather than dropped via WHERE, so
    * historical spend does not silently vanish once newer rows carry a
    * model. GROUP BY 1 groups on the relabelled expression. */
   static const char *sql_recent =
       "SELECT CASE WHEN COALESCE(model, '') = '' THEN '(unattributed)' ELSE model END,"
       " COUNT(*),"
       " COALESCE(SUM(prompt_tokens), 0),"
       " COALESCE(SUM(completion_tokens), 0),"
       " COALESCE(SUM(estimated_cost_usd), 0.0)"
       " FROM token_audit"
       " WHERE " TA_REALIZED " AND created_at >= datetime('now', ?)"
       " GROUP BY 1"
       " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
       "          COALESCE(SUM(completion_tokens), 0) DESC"
       " LIMIT ?";
   static const char *sql_all =
       "SELECT CASE WHEN COALESCE(model, '') = '' THEN '(unattributed)' ELSE model END,"
       " COUNT(*),"
       " COALESCE(SUM(prompt_tokens), 0),"
       " COALESCE(SUM(completion_tokens), 0),"
       " COALESCE(SUM(estimated_cost_usd), 0.0)"
       " FROM token_audit"
       " WHERE " TA_REALIZED " GROUP BY 1"
       " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
       "          COALESCE(SUM(completion_tokens), 0) DESC"
       " LIMIT ?";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, param++, since, -1, SQLITE_TRANSIENT);
   }
   sqlite3_bind_int(stmt, param, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].model, sizeof(out[n].model), stmt, 0);
      out[n].calls = sqlite3_column_int(stmt, 1);
      out[n].prompt_tokens = sqlite3_column_int64(stmt, 2);
      out[n].completion_tokens = sqlite3_column_int64(stmt, 3);
      out[n].estimated_cost_usd = sqlite3_column_double(stmt, 4);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_token_audit_by_source(int since_hours, db1_token_audit_source_summary_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* Empty-source rows (legacy entries written before the source was tracked)
    * surface as "(unattributed)" rather than dropped, mirroring by_model. */
   static const char *sql_recent =
       "SELECT CASE WHEN COALESCE(source, '') = '' THEN '(unattributed)' ELSE source END,"
       " COUNT(*),"
       " COALESCE(SUM(prompt_tokens), 0),"
       " COALESCE(SUM(completion_tokens), 0),"
       " COALESCE(SUM(estimated_cost_usd), 0.0)"
       " FROM token_audit"
       " WHERE " TA_REALIZED " AND created_at >= datetime('now', ?)"
       " GROUP BY 1"
       " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
       "          COALESCE(SUM(completion_tokens), 0) DESC"
       " LIMIT ?";
   static const char *sql_all =
       "SELECT CASE WHEN COALESCE(source, '') = '' THEN '(unattributed)' ELSE source END,"
       " COUNT(*),"
       " COALESCE(SUM(prompt_tokens), 0),"
       " COALESCE(SUM(completion_tokens), 0),"
       " COALESCE(SUM(estimated_cost_usd), 0.0)"
       " FROM token_audit"
       " WHERE " TA_REALIZED " GROUP BY 1"
       " ORDER BY COALESCE(SUM(prompt_tokens), 0) +"
       "          COALESCE(SUM(completion_tokens), 0) DESC"
       " LIMIT ?";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, param++, since, -1, SQLITE_TRANSIENT);
   }
   sqlite3_bind_int(stmt, param, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].source, sizeof(out[n].source), stmt, 0);
      out[n].calls = sqlite3_column_int(stmt, 1);
      out[n].prompt_tokens = sqlite3_column_int64(stmt, 2);
      out[n].completion_tokens = sqlite3_column_int64(stmt, 3);
      out[n].estimated_cost_usd = sqlite3_column_double(stmt, 4);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_insights_by_platform(int since_hours, db1_insights_platform_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql_recent = "SELECT COALESCE(client_type,''), COUNT(*)"
                                   " FROM server_sessions"
                                   " WHERE created_at >= datetime('now', ?)"
                                   " GROUP BY client_type ORDER BY COUNT(*) DESC"
                                   " LIMIT ?";
   static const char *sql_all = "SELECT COALESCE(client_type,''), COUNT(*)"
                                " FROM server_sessions"
                                " GROUP BY client_type ORDER BY COUNT(*) DESC"
                                " LIMIT ?";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, param++, since, -1, SQLITE_TRANSIENT);
   }
   sqlite3_bind_int(stmt, param, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].platform, sizeof(out[n].platform), stmt, 0);
      out[n].session_count = sqlite3_column_int(stmt, 1);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_insights_top_sessions(int since_hours, db1_insights_top_session_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql_recent =
       "SELECT ta.session_id, COALESCE(ss.title,''), COALESCE(MAX(ta.model),''),"
       " COALESCE(SUM(ta.prompt_tokens),0), COALESCE(SUM(ta.completion_tokens),0),"
       " COALESCE(SUM(ta.estimated_cost_usd),0.0), COALESCE(MIN(ta.created_at),'')"
       " FROM token_audit ta"
       " LEFT JOIN server_sessions ss ON ss.id = ta.session_id"
       " WHERE (ta.usage_kind = 'realized' OR ta.usage_kind = '' OR ta.usage_kind IS NULL)"
       " AND ta.created_at >= datetime('now', ?)"
       " GROUP BY ta.session_id"
       " ORDER BY SUM(ta.estimated_cost_usd) DESC LIMIT ?";
   static const char *sql_all =
       "SELECT ta.session_id, COALESCE(ss.title,''), COALESCE(MAX(ta.model),''),"
       " COALESCE(SUM(ta.prompt_tokens),0), COALESCE(SUM(ta.completion_tokens),0),"
       " COALESCE(SUM(ta.estimated_cost_usd),0.0), COALESCE(MIN(ta.created_at),'')"
       " FROM token_audit ta"
       " LEFT JOIN server_sessions ss ON ss.id = ta.session_id"
       " WHERE (ta.usage_kind = 'realized' OR ta.usage_kind = '' OR ta.usage_kind IS NULL)"
       " GROUP BY ta.session_id"
       " ORDER BY SUM(ta.estimated_cost_usd) DESC LIMIT ?";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, param++, since, -1, SQLITE_TRANSIENT);
   }
   sqlite3_bind_int(stmt, param, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].session_id, sizeof(out[n].session_id), stmt, 0);
      db1_copy_col_text(out[n].title, sizeof(out[n].title), stmt, 1);
      db1_copy_col_text(out[n].model, sizeof(out[n].model), stmt, 2);
      out[n].prompt_tokens = sqlite3_column_int64(stmt, 3);
      out[n].completion_tokens = sqlite3_column_int64(stmt, 4);
      out[n].estimated_cost_usd = sqlite3_column_double(stmt, 5);
      db1_copy_col_text(out[n].created_at, sizeof(out[n].created_at), stmt, 6);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_insights_delegates_by_role(int since_hours, db1_insights_delegate_role_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql_recent = "SELECT COALESCE(role,''), COUNT(*),"
                                   " SUM(CASE WHEN status='completed' THEN 1 ELSE 0 END)"
                                   " FROM delegation_spawns"
                                   " WHERE created_at >= datetime('now', ?)"
                                   " GROUP BY role ORDER BY COUNT(*) DESC LIMIT ?";
   static const char *sql_all = "SELECT COALESCE(role,''), COUNT(*),"
                                " SUM(CASE WHEN status='completed' THEN 1 ELSE 0 END)"
                                " FROM delegation_spawns"
                                " GROUP BY role ORDER BY COUNT(*) DESC LIMIT ?";
   const char *sql = since_hours > 0 ? sql_recent : sql_all;
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   char since[32];
   if (since_hours > 0)
   {
      snprintf(since, sizeof(since), "-%d hours", since_hours);
      sqlite3_bind_text(stmt, param++, since, -1, SQLITE_TRANSIENT);
   }
   sqlite3_bind_int(stmt, param, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 0);
      out[n].total = sqlite3_column_int(stmt, 1);
      out[n].completed = sqlite3_column_int(stmt, 2);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_token_audit_list_dashboard(db1_token_audit_dashboard_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   static const char *sql =
       "SELECT COALESCE(tool_name, ''), COALESCE(role, ''),"
       " COALESCE(SUM(prompt_tokens), 0), COALESCE(SUM(completion_tokens), 0),"
       " COALESCE(SUM(cache_write_tokens), 0), COALESCE(SUM(cache_read_tokens), 0),"
       " COALESCE(SUM(estimated_cost_usd), 0.0), COUNT(*), COALESCE(MAX(created_at), '')"
       " FROM token_audit"
       " WHERE " TA_REALIZED " GROUP BY tool_name, role"
       " ORDER BY COALESCE(SUM(estimated_cost_usd), 0.0) DESC,"
       "          COALESCE(MAX(created_at), '') DESC"
       " LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);

   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].tool_name, sizeof(out[n].tool_name), stmt, 0);
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 1);
      out[n].prompt_tokens = sqlite3_column_int64(stmt, 2);
      out[n].completion_tokens = sqlite3_column_int64(stmt, 3);
      out[n].cache_write_tokens = sqlite3_column_int64(stmt, 4);
      out[n].cache_read_tokens = sqlite3_column_int64(stmt, 5);
      out[n].estimated_cost_usd = sqlite3_column_double(stmt, 6);
      out[n].call_count = sqlite3_column_int(stmt, 7);
      db1_copy_col_text(out[n].last_seen, sizeof(out[n].last_seen), stmt, 8);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
