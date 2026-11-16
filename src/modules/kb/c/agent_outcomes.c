/* kb_store/agent_outcomes.c: per-delegation outcome audit — Postgres via libpq. */

#include "agent_outcomes.h"
#include "kb_store_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

int kb_store_agent_outcome_record(const char *agent_name, const char *role,
                                  const char *outcome_kind, const char *reason, int turns_used,
                                  int tools_called, int64_t tokens_used,
                                  const char *tool_error_pattern)
{
   void *conn = kb_store_conn();
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
