#ifndef AIMEE_DB2_MEMORY_SCOPE_QUERY_H
#define AIMEE_DB2_MEMORY_SCOPE_QUERY_H

#include "db_postgres.h"
#include <stdint.h>
#include <stdio.h>
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

/* The vocabulary a point's `visibility` label is written in.
 *
 * memory_visibility_labels() (modules/db2/c/schema.sql) writes it; this builds
 * the caller's half of it. Named here, beside the rank they both have to agree
 * with, because the whole hazard is two hand-written copies of one predicate. */
#define DB2_MEMORY_VISIBILITY_PROJECT_PREFIX   "project:"
#define DB2_MEMORY_VISIBILITY_WORKSPACE_PREFIX "workspace:"
#define DB2_MEMORY_VISIBILITY_GLOBAL           "global"
#define DB2_MEMORY_VISIBILITY_SHARED           "workspace:_shared"
#define DB2_MEMORY_VISIBILITY_UNTAGGED         "untagged"

#define DB2_MEMORY_VISIBILITY_VALUE_MAX  256
#define DB2_MEMORY_VISIBILITY_MAX_VALUES 5

/* The visibility values a caller in this scope is allowed to see.
 *
 * A point carries every scope it belongs to as separate values of one
 * `visibility` label, so DB2_MEMORY_SCOPE_RANK_SQL > 0 becomes exactly "these
 * two sets intersect" -- one IN, no OR. That equivalence is what
 * scripts/db2_replay_env.sh checks, against the rank itself, over a real
 * database: agreeing with the rank is the entire requirement here.
 *
 * Returns how many values were written; 0 when no scope filter applies at all
 * -- an inactive scope, or include_all, both of which the rank filter reads as
 * "admit everything". Zero is unambiguous, because an active scope always
 * yields at least the three unconditional values. Returns -1 if a value does
 * not FIT, which is refused rather than truncated: a workspace cut short is a
 * different workspace, and a search narrowed behind the caller's back returns a
 * short answer indistinguishable from a complete one.
 *
 * `storage` holds the built values and must outlive `values`, which points into
 * it. The request builder allocates nothing and neither does this. */
static inline int
db2_memory_visibility_filter_values(const db2_memory_scope_context_t *ctx,
                                    char storage[][DB2_MEMORY_VISIBILITY_VALUE_MAX],
                                    const char *values[], size_t capacity)
{
   if (!ctx || !storage || !values)
      return -1;
   if (!ctx->active || ctx->include_all)
      return 0;

   const char *prefixes[DB2_MEMORY_VISIBILITY_MAX_VALUES];
   const char *suffixes[DB2_MEMORY_VISIBILITY_MAX_VALUES];
   size_t wanted = 0;
   if (ctx->project[0])
   {
      prefixes[wanted] = DB2_MEMORY_VISIBILITY_PROJECT_PREFIX;
      suffixes[wanted++] = ctx->project;
   }
   if (ctx->workspace[0])
   {
      prefixes[wanted] = DB2_MEMORY_VISIBILITY_WORKSPACE_PREFIX;
      suffixes[wanted++] = ctx->workspace;
   }
   /* Rank 1 in three pieces: the global row, the '_shared' workspace row, and
    * the absence of any scope row -- which the writer turns into a value
    * because a provider cannot ask about a row that is not there. */
   prefixes[wanted] = DB2_MEMORY_VISIBILITY_GLOBAL;
   suffixes[wanted++] = "";
   prefixes[wanted] = DB2_MEMORY_VISIBILITY_SHARED;
   suffixes[wanted++] = "";
   prefixes[wanted] = DB2_MEMORY_VISIBILITY_UNTAGGED;
   suffixes[wanted++] = "";

   size_t count = 0;
   for (size_t i = 0; i < wanted; ++i)
   {
      char built[DB2_MEMORY_VISIBILITY_VALUE_MAX];
      int written = snprintf(built, sizeof(built), "%s%s", prefixes[i], suffixes[i]);
      if (written < 0 || (size_t)written >= sizeof(built))
         return -1;
      /* An active workspace literally named '_shared' would build the constant
       * a second time. Harmless to a set-membership test, but two encodings of
       * one question are worth not emitting. */
      int already = 0;
      for (size_t j = 0; j < count; ++j)
         if (strcmp(storage[j], built) == 0)
            already = 1;
      if (already)
         continue;
      if (count >= capacity)
         return -1;
      memcpy(storage[count], built, (size_t)written + 1);
      values[count] = storage[count];
      count++;
   }
   return (int)count;
}

#endif
