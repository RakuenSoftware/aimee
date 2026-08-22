#ifndef AIMEE_DB2_MEMORY_SCOPE_QUERY_H
#define AIMEE_DB2_MEMORY_SCOPE_QUERY_H

#include "db_postgres.h"
#include <stdint.h>
#include <string.h>

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

/* Applies a request's scope for the length of a block and puts back whatever
 * was there before, on every exit including an early return.
 *
 * The adapter runs in the caller's process and shares this thread-local with
 * it, so a dispatch block that set the scope and returned would silently
 * rescope whatever the caller did next. The dispatch blocks have many return
 * paths each; a guard is one line where restoring by hand is one edit per
 * return and one missed edit away from a leak.
 *
 * scope_flags is the wire encoding: bit 0 active, bit 1 include_all. An
 * inactive scope clears rather than sets, because the scope filter reads
 * "inactive" as "admit everything" and inheriting the caller's workspace
 * would answer a different question than the one asked. */
typedef struct
{
   db2_memory_scope_context_t saved;
} db2_memory_scope_guard_t;

void db2_memory_scope_context_set(const char *workspace, const char *project, int include_all);
void db2_memory_scope_context_clear(void);
void db2_memory_scope_context_get(db2_memory_scope_context_t *out);

/* Inline over the three accessors above rather than a symbol of its own: the
 * guard holds no state, and the unit suites that link the adapter stub those
 * accessors without linking this translation unit. */
static inline db2_memory_scope_guard_t db2_memory_scope_guard_enter(unsigned int scope_flags,
                                                                    const char *workspace,
                                                                    const char *project)
{
   db2_memory_scope_guard_t guard;
   memset(&guard, 0, sizeof(guard));
   db2_memory_scope_context_get(&guard.saved);
   if (scope_flags & 1u)
      db2_memory_scope_context_set(workspace, project, (int)((scope_flags >> 1) & 1u));
   else
      db2_memory_scope_context_clear();
   return guard;
}

static inline void db2_memory_scope_guard_release(db2_memory_scope_guard_t *guard)
{
   if (!guard)
      return;
   /* Put back, never clear: the caller's scope is not this call's to discard. */
   if (guard->saved.active)
      db2_memory_scope_context_set(guard->saved.workspace, guard->saved.project,
                                   guard->saved.include_all);
   else
      db2_memory_scope_context_clear();
}

#define DB2_MEMORY_SCOPE_GUARD __attribute__((cleanup(db2_memory_scope_guard_release)))
int db2_memory_scope_context_rank(int64_t memory_id);
void db2_memory_scope_bind_current(aimee_pg_stmt_t *st);

#endif
