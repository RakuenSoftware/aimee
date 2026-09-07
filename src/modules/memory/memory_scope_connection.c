/* Per-request memory scope carried by the KB connection thread.
 *
 * This is intentionally C: it binds request context onto the legacy PostgreSQL
 * connection and contains no ranking or visibility policy. Rank decisions are
 * delegated to the Go memory module through memory_scope_visibility_rank().
 */
#include "aimee.h"
#include "memory_scope_query.h"

#include <stdio.h>
#include <string.h>

static _Thread_local db2_memory_scope_context_t current_scope;

void db2_memory_scope_context_set_exact(const char *workspace, const char *project,
                                        const char *scope_type, const char *scope_value,
                                        int include_all)
{
   memset(&current_scope, 0, sizeof(current_scope));
   current_scope.active = 1;
   current_scope.include_all = include_all != 0;
   snprintf(current_scope.workspace, sizeof(current_scope.workspace), "%s",
            workspace ? workspace : "");
   snprintf(current_scope.project, sizeof(current_scope.project), "%s", project ? project : "");
   snprintf(current_scope.scope_type, sizeof(current_scope.scope_type), "%s",
            scope_type ? scope_type : "");
   snprintf(current_scope.scope_value, sizeof(current_scope.scope_value), "%s",
            scope_value ? scope_value : "");
}

void db2_memory_scope_context_set(const char *workspace, const char *project, int include_all)
{
   db2_memory_scope_context_set_exact(workspace, project, "", "", include_all);
}

void db2_memory_scope_context_restore(const db2_memory_scope_context_t *context)
{
   if (context)
      current_scope = *context;
   else
      memset(&current_scope, 0, sizeof(current_scope));
}

void db2_memory_scope_context_clear(void)
{
   memset(&current_scope, 0, sizeof(current_scope));
}

void db2_memory_scope_context_get(db2_memory_scope_context_t *out)
{
   if (out)
      *out = current_scope;
}

int db2_memory_scope_context_rank(int64_t memory_id)
{
   if (!current_scope.active)
      return 1;
   if (current_scope.include_all)
      return 1;
   return memory_scope_visibility_rank(memory_id, current_scope.workspace, current_scope.project);
}

int db2_memory_scope_context_rank_batch(const int64_t *ids, int n, int *out_ranks)
{
   if (!ids || !out_ranks || n < 0)
      return -1;
   for (int i = 0; i < n; ++i)
      out_ranks[i] = db2_memory_scope_context_rank(ids[i]);
   return n;
}

int db2_memory_scope_context_allows(int64_t memory_id)
{
   return db2_memory_scope_context_rank(memory_id) > 0;
}

void db2_memory_scope_bind_current(aimee_pg_stmt_t *statement)
{
   if (!statement)
      return;
   aimee_pg_bind_int(statement, "?101", current_scope.active);
   aimee_pg_bind_int(statement, "?102", current_scope.include_all);
   aimee_pg_bind_text(statement, "?103", current_scope.workspace);
   aimee_pg_bind_text(statement, "?104", current_scope.project);
   aimee_pg_bind_text(statement, "?105", current_scope.scope_type);
   aimee_pg_bind_text(statement, "?106", current_scope.scope_value);
}
