/* guardrail_events.c: DB1 storage for semantic guardrail event records. */
#include "db1_internal.h"
#include "guardrail_events.h"
#include <stdio.h>
#include <string.h>

int db1_guardrail_event_insert(const guardrail_event_t *e)
{
   sqlite3 *db = db1_conn();
   if (!db || !e)
      return -1;

   /* Wrap in db1's transaction gate (g_txn_gate) rather than issuing a bare
    * autocommit INSERT. The shared connection has ONE transaction context: an
    * unguarded autocommit write can run inside another thread's open explicit
    * transaction (BEGIN IMMEDIATE ... COMMIT) and be committed or rolled back
    * with it. Taking the gate serializes this insert against those transactions,
    * which also makes it correct to call from the audit-bus consumer thread. */
   if (db1_txn_begin(db, "BEGIN IMMEDIATE") != 0)
      return -1;

   const char *sql =
       "INSERT INTO guardrail_events"
       " (session_id, tool_name, overall_risk, action_risk, diff_risk, drift_risk,"
       "  antipattern_similarity, recommendation, labels, final_action, explanation, dry_run)"
       " VALUES (?,?,?,?,?,?,?,?,?,?,?,?)";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1;
   }

   sqlite3_bind_text(stmt, 1, e->session_id, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 2, e->tool_name, -1, SQLITE_STATIC);
   sqlite3_bind_double(stmt, 3, e->overall_risk);
   sqlite3_bind_double(stmt, 4, e->action_risk);
   sqlite3_bind_double(stmt, 5, e->diff_risk);
   sqlite3_bind_double(stmt, 6, e->drift_risk);
   sqlite3_bind_double(stmt, 7, e->antipattern_similarity);
   sqlite3_bind_text(stmt, 8, e->recommendation, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 9, e->labels, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 10, e->final_action, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 11, e->explanation, -1, SQLITE_STATIC);
   sqlite3_bind_int(stmt, 12, e->dry_run);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   if (rc != SQLITE_DONE)
   {
      db1_txn_end(db, "ROLLBACK");
      return -1; /* real failure, not a silent success */
   }
   return db1_txn_end(db, "COMMIT"); /* 0 on commit, -1 if the commit failed */
}

int db1_guardrail_event_counts_7d(guardrail_event_counts_t *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   const char *sql = "SELECT"
                     " COALESCE(SUM(CASE WHEN dry_run = 1 THEN 1 ELSE 0 END), 0),"
                     " COALESCE(SUM(CASE WHEN dry_run = 0 AND final_action = 'warn'"
                     "                   THEN 1 ELSE 0 END), 0),"
                     " COALESCE(SUM(CASE WHEN dry_run = 0 AND final_action = 'prompt'"
                     "                   THEN 1 ELSE 0 END), 0),"
                     " COALESCE(SUM(CASE WHEN dry_run = 0 AND final_action = 'block'"
                     "                   THEN 1 ELSE 0 END), 0)"
                     " FROM guardrail_events"
                     " WHERE recorded_at >= datetime('now', '-7 days')";

   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      out->dry_run = sqlite3_column_int(stmt, 0);
      out->warn = sqlite3_column_int(stmt, 1);
      out->prompt = sqlite3_column_int(stmt, 2);
      out->block = sqlite3_column_int(stmt, 3);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_guardrail_event_session_advisory_count(const char *session_id, int *out)
{
   sqlite3 *db = db1_conn();
   if (!db || !session_id || !out)
      return -1;
   *out = 0;

   const char *sql = "SELECT COUNT(*) FROM guardrail_events"
                     " WHERE session_id = ? AND dry_run = 0"
                     " AND final_action IN ('warn', 'prompt')";
   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      *out = sqlite3_column_int(stmt, 0);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int db1_guardrail_event_list(int limit, int only_advisory, guardrail_event_row_t *rows, int *count)
{
   sqlite3 *db = db1_conn();
   if (!db || !rows || !count || limit <= 0)
      return -1;
   *count = 0;

   const char *sql_advisory = "SELECT id, recorded_at, session_id, tool_name, overall_risk, labels,"
                              " final_action, explanation, dry_run"
                              " FROM guardrail_events"
                              " WHERE final_action IN ('warn', 'prompt', 'block')"
                              " ORDER BY recorded_at DESC LIMIT ?";
   const char *sql_all = "SELECT id, recorded_at, session_id, tool_name, overall_risk, labels,"
                         " final_action, explanation, dry_run"
                         " FROM guardrail_events"
                         " ORDER BY recorded_at DESC LIMIT ?";

   sqlite3_stmt *stmt;
   if (sqlite3_prepare_v2(db, only_advisory ? sql_advisory : sql_all, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, limit);

   int n = 0;
   while (n < limit && sqlite3_step(stmt) == SQLITE_ROW)
   {
      guardrail_event_row_t *r = &rows[n++];
      r->id = sqlite3_column_int(stmt, 0);
      const char *ts = (const char *)sqlite3_column_text(stmt, 1);
      if (ts)
         snprintf(r->recorded_at, sizeof(r->recorded_at), "%s", ts);
      const char *sid = (const char *)sqlite3_column_text(stmt, 2);
      if (sid)
         snprintf(r->session_id, sizeof(r->session_id), "%s", sid);
      const char *tool = (const char *)sqlite3_column_text(stmt, 3);
      if (tool)
         snprintf(r->tool_name, sizeof(r->tool_name), "%s", tool);
      r->overall_risk = sqlite3_column_double(stmt, 4);
      const char *lbl = (const char *)sqlite3_column_text(stmt, 5);
      if (lbl)
         snprintf(r->labels, sizeof(r->labels), "%s", lbl);
      const char *act = (const char *)sqlite3_column_text(stmt, 6);
      if (act)
         snprintf(r->final_action, sizeof(r->final_action), "%s", act);
      const char *expl = (const char *)sqlite3_column_text(stmt, 7);
      if (expl)
         snprintf(r->explanation, sizeof(r->explanation), "%s", expl);
      r->dry_run = sqlite3_column_int(stmt, 8);
   }
   sqlite3_finalize(stmt);
   *count = n;
   return 0;
}
