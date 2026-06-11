/* db1/agent_log.c: per-machine agent/tool turn audit log. */

#include "agent_log.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int db1_agent_log_insert(const db1_agent_log_insert_row_t *row)
{
   if (!row || !row->agent_name || !row->role)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO agent_log (agent_name, role, prompt_tokens, completion_tokens,"
       " latency_ms, success, error, turns, tool_calls, confidence, session_id, created_at)"
       " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, row->agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, row->role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, row->prompt_tokens);
   sqlite3_bind_int(stmt, 4, row->completion_tokens);
   sqlite3_bind_int(stmt, 5, row->latency_ms);
   sqlite3_bind_int(stmt, 6, row->success);
   if (row->error && row->error[0])
      sqlite3_bind_text(stmt, 7, row->error, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 7);
   sqlite3_bind_int(stmt, 8, row->turns);
   sqlite3_bind_int(stmt, 9, row->tool_calls);
   sqlite3_bind_int(stmt, 10, row->confidence);
   sqlite3_bind_text(stmt, 11, row->session_id ? row->session_id : "", -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return (rc == SQLITE_DONE) ? 0 : -1;
}

static void fill_display(db1_agent_log_display_t *d, sqlite3_stmt *s)
{
   d->id = sqlite3_column_int64(s, 0);
   db1_copy_col_text(d->agent_name, sizeof(d->agent_name), s, 1);
   db1_copy_col_text(d->role, sizeof(d->role), s, 2);
   d->prompt_tokens = sqlite3_column_int(s, 3);
   d->completion_tokens = sqlite3_column_int(s, 4);
   d->latency_ms = sqlite3_column_int(s, 5);
   d->success = sqlite3_column_int(s, 6);
   d->turns = sqlite3_column_int(s, 7);
   d->tool_calls = sqlite3_column_int(s, 8);
   d->confidence = sqlite3_column_int(s, 9);
   db1_copy_col_text(d->session_id, sizeof(d->session_id), s, 10);
   db1_copy_col_text(d->created_at, sizeof(d->created_at), s, 11);
   db1_copy_col_text(d->error, sizeof(d->error), s, 12);
}

static const char *DISPLAY_COLUMNS =
    "id, agent_name, role, prompt_tokens, completion_tokens, latency_ms, success,"
    " turns, tool_calls, confidence, COALESCE(session_id, ''), created_at, COALESCE(error, '')";

int db1_agent_log_list_recent(db1_agent_log_display_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char sql[512];
   snprintf(sql, sizeof(sql), "SELECT %s FROM agent_log ORDER BY id DESC LIMIT ?", DISPLAY_COLUMNS);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      fill_display(&out[n++], stmt);
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_list_by_session(const char *session_id, db1_agent_log_display_t *out, int max)
{
   if (!session_id || !out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char sql[512];
   snprintf(sql, sizeof(sql),
            "SELECT %s FROM agent_log WHERE session_id = ? ORDER BY id ASC LIMIT ?",
            DISPLAY_COLUMNS);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      fill_display(&out[n++], stmt);
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_search_session_ids_by_role(const char *pattern,
                                             char (*out_ids)[DB1_AL_SESSION_LEN], int max)
{
   if (!pattern || !out_ids || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT DISTINCT session_id FROM agent_log"
                            " WHERE session_id IS NOT NULL AND session_id != '' AND role LIKE ?"
                            " ORDER BY id DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      const unsigned char *v = sqlite3_column_text(stmt, 0);
      snprintf(out_ids[n++], DB1_AL_SESSION_LEN, "%s", v ? (const char *)v : "");
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_count_per_role(const char *since_or_null, db1_agent_log_role_count_t *out,
                                 int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *sql =
       since_or_null
           ? "SELECT role, COUNT(*) FROM agent_log WHERE created_at >= ?"
             " GROUP BY role ORDER BY COUNT(*) DESC LIMIT ?"
           : "SELECT role, COUNT(*) FROM agent_log GROUP BY role ORDER BY COUNT(*) DESC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   if (since_or_null)
      sqlite3_bind_text(stmt, param++, since_or_null, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, param, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 0);
      out[n].count = sqlite3_column_int(stmt, 1);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_stats(const char *since_or_null, db1_agent_log_stats_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *sql =
       since_or_null
           ? "SELECT COUNT(*), COALESCE(SUM(turns), 0), COALESCE(SUM(tool_calls), 0),"
             "       COALESCE(SUM(prompt_tokens), 0), COALESCE(SUM(completion_tokens), 0),"
             "       COALESCE(SUM(success), 0)"
             " FROM agent_log WHERE created_at >= ?"
           : "SELECT COUNT(*), COALESCE(SUM(turns), 0), COALESCE(SUM(tool_calls), 0),"
             "       COALESCE(SUM(prompt_tokens), 0), COALESCE(SUM(completion_tokens), 0),"
             "       COALESCE(SUM(success), 0)"
             " FROM agent_log";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   if (since_or_null)
      sqlite3_bind_text(stmt, 1, since_or_null, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->total = sqlite3_column_int(stmt, 0);
      out->turns = sqlite3_column_int64(stmt, 1);
      out->tool_calls = sqlite3_column_int64(stmt, 2);
      out->prompt_tokens = sqlite3_column_int64(stmt, 3);
      out->completion_tokens = sqlite3_column_int64(stmt, 4);
      out->successes = sqlite3_column_int(stmt, 5);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_agent_log_failures_since_seconds(int max_rows, int since_secs, db1_agent_log_failure_t *out)
{
   if (!out || max_rows <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char sql[384];
   snprintf(sql, sizeof(sql),
            "SELECT role, COALESCE(error, ''), created_at FROM agent_log"
            " WHERE success = 0 AND error IS NOT NULL"
            " AND created_at > datetime('now', '-%d seconds')"
            " ORDER BY id DESC LIMIT ?",
            since_secs);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max_rows);
   int n = 0;
   while (n < max_rows && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 0);
      db1_copy_col_text(out[n].error, sizeof(out[n].error), stmt, 1);
      db1_copy_col_text(out[n].created_at, sizeof(out[n].created_at), stmt, 2);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_list_recent_errors(int since_days, db1_agent_log_recent_error_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char sql[384];
   snprintf(sql, sizeof(sql),
            "SELECT error FROM agent_log"
            " WHERE success = 0 AND error IS NOT NULL AND error != ''"
            " AND created_at > datetime('now', '-%d days')"
            " ORDER BY id DESC LIMIT ?",
            since_days);
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].error, sizeof(out[n].error), stmt, 0);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

static void load_recent_error(sqlite3 *db, const char *role, const char *agent_name, int since_days,
                              char *out, size_t out_sz)
{
   if (!out || out_sz == 0)
      return;
   out[0] = '\0';
   if (!db || !role || !agent_name)
      return;

   char window[32];
   snprintf(window, sizeof(window), "-%d days", since_days);

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT error FROM agent_log"
                            " WHERE role = ? AND agent_name = ? AND success = 0"
                            " AND error IS NOT NULL AND error != ''"
                            " AND created_at > datetime('now', ?)"
                            " ORDER BY id DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return;
   sqlite3_bind_text(stmt, 1, role, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, window, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) == SQLITE_ROW)
      db1_copy_col_text(out, out_sz, stmt, 0);
   sqlite3_finalize(stmt);
}

int db1_agent_log_list_delegation_patterns(int since_days, int min_total,
                                           db1_agent_log_delegation_pattern_t *out, int max)
{
   if (!out || max <= 0 || min_total <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char window[32];
   snprintf(window, sizeof(window), "-%d days", since_days);

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT role, agent_name,"
                            " SUM(CASE WHEN success = 1 THEN 1 ELSE 0 END) AS wins,"
                            " SUM(CASE WHEN success = 0 THEN 1 ELSE 0 END) AS fails,"
                            " COUNT(*) AS total,"
                            " CAST(AVG(turns) AS INTEGER) AS avg_turns,"
                            " CAST(AVG(tool_calls) AS INTEGER) AS avg_tools"
                            " FROM agent_log"
                            " WHERE created_at > datetime('now', ?)"
                            " AND role != ''"
                            " GROUP BY role, agent_name"
                            " HAVING COUNT(*) >= ?"
                            " ORDER BY COUNT(*) DESC, role ASC, agent_name ASC"
                            " LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, window, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, min_total);
   sqlite3_bind_int(stmt, 3, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 0);
      db1_copy_col_text(out[n].agent_name, sizeof(out[n].agent_name), stmt, 1);
      out[n].wins = sqlite3_column_int(stmt, 2);
      out[n].fails = sqlite3_column_int(stmt, 3);
      out[n].total = sqlite3_column_int(stmt, 4);
      out[n].avg_turns = sqlite3_column_int(stmt, 5);
      out[n].avg_tools = sqlite3_column_int(stmt, 6);
      load_recent_error(db, out[n].role, out[n].agent_name, since_days, out[n].recent_error,
                        sizeof(out[n].recent_error));
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_list_failure_episode_seeds(int since_days, int min_fails,
                                             db1_agent_log_failure_episode_seed_t *out, int max)
{
   if (!out || max <= 0 || min_fails <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   char window[32];
   snprintf(window, sizeof(window), "-%d days", since_days);

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT role, agent_name, COUNT(*) AS fails,"
                            " GROUP_CONCAT(error, ' | ') AS errors"
                            " FROM agent_log"
                            " WHERE success = 0"
                            " AND created_at > datetime('now', ?)"
                            " AND error IS NOT NULL AND error != ''"
                            " GROUP BY role, agent_name"
                            " HAVING COUNT(*) >= ?"
                            " ORDER BY COUNT(*) DESC, role ASC, agent_name ASC"
                            " LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, window, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, min_fails);
   sqlite3_bind_int(stmt, 3, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 0);
      db1_copy_col_text(out[n].agent_name, sizeof(out[n].agent_name), stmt, 1);
      out[n].fails = sqlite3_column_int(stmt, 2);
      db1_copy_col_text(out[n].errors, sizeof(out[n].errors), stmt, 3);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_metrics_by_role(db1_agent_log_metric_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   /* Both agent_log and token_audit live in DB1 now, so the JOIN is
    * internal to this store. */
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT al.role, COUNT(*), SUM(CASE WHEN al.success THEN 1 ELSE 0 END),"
       "       AVG(al.latency_ms), SUM(al.prompt_tokens + al.completion_tokens),"
       "       COALESCE(SUM(ta.cache_write_tokens), 0),"
       "       COALESCE(SUM(ta.cache_read_tokens), 0),"
       "       COALESCE(SUM(ta.estimated_cost_usd), 0.0)"
       " FROM agent_log al"
       /* Only join internal agent rows: ingress-sourced rows have no agent_log
        * row and must not inflate agent stats (they match by agent name only). */
       " LEFT JOIN token_audit ta"
       "   ON ta.tool_name = al.agent_name AND ta.role = al.role"
       "  AND ta.source IN ('', 'agent')"
       " GROUP BY al.role ORDER BY COUNT(*) DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 0);
      out[n].total = sqlite3_column_int(stmt, 1);
      out[n].successes = sqlite3_column_int(stmt, 2);
      out[n].avg_latency_ms = sqlite3_column_int(stmt, 3);
      out[n].tokens = sqlite3_column_int64(stmt, 4);
      out[n].cache_write_tokens = sqlite3_column_int64(stmt, 5);
      out[n].cache_read_tokens = sqlite3_column_int64(stmt, 6);
      out[n].estimated_cost_usd = sqlite3_column_double(stmt, 7);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_agent_stats(const char *agent_name_or_null, db1_agent_log_agent_stats_t *out,
                              int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   const char *sql =
       agent_name_or_null
           ? "SELECT al.agent_name, COUNT(*), SUM(al.prompt_tokens), SUM(al.completion_tokens),"
             "       AVG(al.latency_ms), AVG(CASE WHEN al.success THEN 1.0 ELSE 0.0 END),"
             "       COALESCE(SUM(ta.cache_write_tokens), 0),"
             "       COALESCE(SUM(ta.cache_read_tokens), 0),"
             "       COALESCE(SUM(ta.estimated_cost_usd), 0.0)"
             " FROM agent_log al"
             /* Exclude ingress-sourced rows: they have no agent_log row and
              * would otherwise inflate agent stats via the agent-name match. */
             " LEFT JOIN token_audit ta"
             "   ON ta.tool_name = al.agent_name AND ta.source IN ('', 'agent')"
             " WHERE al.agent_name = ?"
             " GROUP BY al.agent_name LIMIT ?"
           : "SELECT al.agent_name, COUNT(*), SUM(al.prompt_tokens), SUM(al.completion_tokens),"
             "       AVG(al.latency_ms), AVG(CASE WHEN al.success THEN 1.0 ELSE 0.0 END),"
             "       COALESCE(SUM(ta.cache_write_tokens), 0),"
             "       COALESCE(SUM(ta.cache_read_tokens), 0),"
             "       COALESCE(SUM(ta.estimated_cost_usd), 0.0)"
             " FROM agent_log al"
             /* Exclude ingress-sourced rows: they have no agent_log row and
              * would otherwise inflate agent stats via the agent-name match. */
             " LEFT JOIN token_audit ta"
             "   ON ta.tool_name = al.agent_name AND ta.source IN ('', 'agent')"
             " GROUP BY al.agent_name ORDER BY COUNT(*) DESC LIMIT ?";
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int param = 1;
   if (agent_name_or_null)
      sqlite3_bind_text(stmt, param++, agent_name_or_null, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, param, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].agent_name, sizeof(out[n].agent_name), stmt, 0);
      out[n].total_calls = sqlite3_column_int(stmt, 1);
      out[n].total_prompt_tokens = sqlite3_column_int(stmt, 2);
      out[n].total_completion_tokens = sqlite3_column_int(stmt, 3);
      out[n].avg_latency_ms = sqlite3_column_int(stmt, 4);
      out[n].success_rate = sqlite3_column_double(stmt, 5);
      out[n].total_cache_write_tokens = sqlite3_column_int64(stmt, 6);
      out[n].total_cache_read_tokens = sqlite3_column_int64(stmt, 7);
      out[n].total_estimated_cost_usd = sqlite3_column_double(stmt, 8);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}

int db1_agent_log_hud_summary(db1_agent_log_hud_t *out, int recent_secs)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql_all = "SELECT COUNT(*),"
                                " COALESCE(SUM(CASE WHEN success = 1 THEN 1 ELSE 0 END), 0),"
                                " COALESCE(SUM(CASE WHEN success = 0 THEN 1 ELSE 0 END), 0),"
                                " COALESCE(SUM(prompt_tokens), 0),"
                                " COALESCE(SUM(completion_tokens), 0),"
                                " COALESCE(SUM(turns), 0),"
                                " COALESCE(SUM(tool_calls), 0),"
                                " COALESCE(AVG(latency_ms), 0)"
                                " FROM agent_log";
   if (sqlite3_prepare_v2(db, sql_all, -1, &stmt, NULL) == SQLITE_OK)
   {
      if (sqlite3_step(stmt) == SQLITE_ROW)
      {
         out->total_calls = sqlite3_column_int(stmt, 0);
         out->successful_calls = sqlite3_column_int(stmt, 1);
         out->failed_calls = sqlite3_column_int(stmt, 2);
         out->total_prompt_tokens = sqlite3_column_int64(stmt, 3);
         out->total_completion_tokens = sqlite3_column_int64(stmt, 4);
         out->total_turns = sqlite3_column_int(stmt, 5);
         out->total_tool_calls = sqlite3_column_int(stmt, 6);
         out->avg_latency_ms = sqlite3_column_double(stmt, 7);
      }
      sqlite3_finalize(stmt);
   }

   char recent_sql[256];
   snprintf(recent_sql, sizeof(recent_sql),
            "SELECT COUNT(*), COALESCE(SUM(CASE WHEN success = 1 THEN 1 ELSE 0 END), 0)"
            " FROM agent_log WHERE created_at >= datetime('now', '-%d seconds')",
            recent_secs);
   if (sqlite3_prepare_v2(db, recent_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      if (sqlite3_step(stmt) == SQLITE_ROW)
      {
         out->recent_calls = sqlite3_column_int(stmt, 0);
         out->recent_successes = sqlite3_column_int(stmt, 1);
      }
      sqlite3_finalize(stmt);
   }
   return 0;
}

int db1_agent_log_session_outcome(const char *session_id, int *successes_out, int *total_out)
{
   if (!session_id || !successes_out || !total_out)
      return -1;
   *successes_out = 0;
   *total_out = 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT COALESCE(SUM(success), 0), COUNT(*) FROM agent_log WHERE session_id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      *successes_out = sqlite3_column_int(stmt, 0);
      *total_out = sqlite3_column_int(stmt, 1);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_agent_log_prometheus(db1_agent_log_prometheus_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT agent_name, role,"
                            " COUNT(*),"
                            " SUM(CASE WHEN success THEN 1 ELSE 0 END),"
                            " SUM(prompt_tokens),"
                            " SUM(completion_tokens),"
                            " AVG(latency_ms),"
                            " SUM(tool_calls)"
                            " FROM agent_log GROUP BY agent_name, role LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
   {
      db1_copy_col_text(out[n].agent_name, sizeof(out[n].agent_name), stmt, 0);
      db1_copy_col_text(out[n].role, sizeof(out[n].role), stmt, 1);
      out[n].total = sqlite3_column_int(stmt, 2);
      out[n].successes = sqlite3_column_int(stmt, 3);
      out[n].prompt_tokens = sqlite3_column_int(stmt, 4);
      out[n].completion_tokens = sqlite3_column_int(stmt, 5);
      out[n].avg_latency_ms = sqlite3_column_int(stmt, 6);
      out[n].tool_calls = sqlite3_column_int(stmt, 7);
      n++;
   }
   sqlite3_finalize(stmt);
   return n;
}
