/* db2/agent_outcomes.c: per-delegation outcome audit — Postgres via libpq. */

#include "agent_outcomes.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

int db2_agent_outcome_record(const char *agent_name, const char *role, const char *outcome_kind,
                             const char *reason, int turns_used, int tools_called,
                             int64_t tokens_used, const char *tool_error_pattern)
{
   void *conn = db2_conn();
   if (!conn || !outcome_kind)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO agent_outcomes "
                        "(agent_name, role, outcome, reason, turns_used, tools_called, "
                        "tokens_used, tool_error_pattern) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", agent_name ? agent_name : "");
   aimee_pg_bind_text(st, "?2", role ? role : "");
   aimee_pg_bind_text(st, "?3", outcome_kind);
   aimee_pg_bind_text(st, "?4", reason ? reason : "");
   aimee_pg_bind_int(st, "?5", turns_used);
   aimee_pg_bind_int(st, "?6", tools_called);
   aimee_pg_bind_int64(st, "?7", tokens_used);
   aimee_pg_bind_text(st, "?8", tool_error_pattern ? tool_error_pattern : "");

   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_agent_outcome_recent_failures(int window_days, db2_agent_outcome_failure_t *out, int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0 || window_days <= 0)
      return 0;

   char window[32];
   snprintf(window, sizeof(window), "-%d days", window_days);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT DISTINCT role, reason FROM agent_outcomes "
                                          "WHERE outcome IN ('failure', 'error') "
                                          "AND created_at > pg_now_text(?1) "
                                          "ORDER BY created_at DESC LIMIT ?2",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", window);
   aimee_pg_bind_int(st, "?2", max);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      const char *r = aimee_pg_column_text(st, 0);
      const char *rs = aimee_pg_column_text(st, 1);
      snprintf(out[count].role, sizeof(out[count].role), "%s", r ? r : "");
      snprintf(out[count].reason, sizeof(out[count].reason), "%s", rs ? rs : "");
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_agent_outcome_repeated_error_patterns(int min_count, db2_agent_outcome_pattern_count_t *out,
                                              int max)
{
   void *conn = db2_conn();
   if (!conn || !out || max <= 0 || min_count <= 0)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT tool_error_pattern, COUNT(*) AS cnt FROM agent_outcomes "
                        "WHERE tool_error_pattern != '' AND tool_error_pattern IS NOT NULL "
                        "GROUP BY tool_error_pattern HAVING COUNT(*) >= ?1",
                        err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", min_count);

   int count = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW && count < max)
   {
      const char *p = aimee_pg_column_text(st, 0);
      snprintf(out[count].pattern, sizeof(out[count].pattern), "%s", p ? p : "");
      out[count].count = aimee_pg_column_int(st, 1);
      count++;
   }
   aimee_pg_finalize(st);
   return count;
}
