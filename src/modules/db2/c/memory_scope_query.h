#ifndef AIMEE_DB2_MEMORY_SCOPE_QUERY_H
#define AIMEE_DB2_MEMORY_SCOPE_QUERY_H

#include "db_postgres.h"
#include <stdint.h>

/* Canonical local-first rank for a memory id.  Keep this SQL equivalent to
 * memory_scope_visibility_rank(): active project, active workspace,
 * shared/global (including legacy untagged rows), then explicit-all others.
 *
 * The request context is thread-local because KB worker threads may serve
 * different repositories concurrently.  Ordered readers bind the four named
 * reserved high-numbered parameters with db2_memory_scope_bind_current() and put this expression
 * before their relevance/freshness ordering and LIMIT. */
#define DB2_MEMORY_SCOPE_RANK_SQL(memory_id_sql)                                              \
   "CASE WHEN ?101 = 0 THEN 0 "                                                              \
   "WHEN ?104 <> '' AND EXISTS (SELECT 1 FROM memory_scopes asp "                            \
   " WHERE asp.memory_id = " memory_id_sql                                                   \
   " AND asp.scope_type = 'project' AND asp.scope_value = ?104) THEN 3 "                      \
   "WHEN ?103 <> '' AND (EXISTS (SELECT 1 FROM memory_scopes asw "                           \
   " WHERE asw.memory_id = " memory_id_sql                                                   \
   " AND asw.scope_type = 'workspace' AND asw.scope_value = ?103) "                           \
   "OR EXISTS (SELECT 1 FROM memory_workspaces aw "                                          \
   " WHERE aw.memory_id = " memory_id_sql " AND aw.workspace = ?103)) THEN 2 "               \
   "WHEN EXISTS (SELECT 1 FROM memory_scopes asg WHERE asg.memory_id = " memory_id_sql        \
   " AND asg.scope_type = 'global' AND asg.scope_value = '_global') "                        \
   "OR EXISTS (SELECT 1 FROM memory_scopes ass WHERE ass.memory_id = " memory_id_sql          \
   " AND ass.scope_type = 'workspace' AND ass.scope_value = '_shared') "                     \
   "OR (NOT EXISTS (SELECT 1 FROM memory_scopes asc0 WHERE asc0.memory_id = " memory_id_sql   \
   " AND asc0.scope_type IN ('global','workspace','project')) "                              \
   "AND NOT EXISTS (SELECT 1 FROM memory_workspaces aw0 WHERE aw0.memory_id = " memory_id_sql \
   ")) THEN 1 ELSE 0 END"

#define DB2_MEMORY_SCOPE_FILTER_SQL(memory_id_sql)                                            \
   " AND (?101 = 0 OR ?102 = 1 OR ("                                                         \
       DB2_MEMORY_SCOPE_RANK_SQL(memory_id_sql) ") > 0)"

typedef struct
{
   int active;
   int include_all;
   char workspace[512];
   char project[512];
} db2_memory_scope_context_t;

void db2_memory_scope_context_set(const char *workspace, const char *project, int include_all);
void db2_memory_scope_context_clear(void);
void db2_memory_scope_context_get(db2_memory_scope_context_t *out);
int db2_memory_scope_context_rank(int64_t memory_id);
void db2_memory_scope_bind_current(aimee_pg_stmt_t *st);

#endif
