/* Per-request memory scope carried by the KB connection thread.
 *
 * Temporary connection boundary while the remaining native DB2 callers migrate.
 * It carries context to their Go module requests; ranking and visibility
 * decisions belong to the Go memory owner.
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

void db2_memory_scope_context_clear(void)
{
   memset(&current_scope, 0, sizeof(current_scope));
}

void db2_memory_scope_context_get(db2_memory_scope_context_t *out)
{
   if (out)
      *out = current_scope;
}
