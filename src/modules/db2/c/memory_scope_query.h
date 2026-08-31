#ifndef AIMEE_DB2_MEMORY_SCOPE_QUERY_H
#define AIMEE_DB2_MEMORY_SCOPE_QUERY_H

#include "db_postgres.h"
#include <stdint.h>

/* Canonical local-first rank for a memory id.  Ownership lives on the memory
 * row itself; memory_scopes remains a compatibility/multi-tag projection.
 * Keep this SQL equivalent to memory_scope_visibility_rank(): active project,
 * active workspace, shared/global, then explicit-all others.
 *
 * The request context is thread-local because KB worker threads may serve
 * different repositories concurrently.  Ordered readers bind the four named
 * reserved high-numbered parameters with db2_memory_scope_bind_current() and put this expression
 * before their relevance/freshness ordering and LIMIT. */
#define DB2_MEMORY_SCOPE_RANK_SQL(memory_id_sql)                                                   \
   "CASE WHEN ?105 <> '' AND ?106 <> '' AND EXISTS (SELECT 1 FROM memories ams "                   \
   " WHERE ams.id = " memory_id_sql " AND ams.scope_type=?105 AND ams.scope_value=?106) THEN 4 "   \
   "WHEN ?104 <> '' AND EXISTS (SELECT 1 FROM memories ams "                                       \
   " WHERE ams.id = " memory_id_sql                                                                \
   " AND ams.scope_type='project' AND ams.scope_value=?104) THEN 3 "                               \
   "WHEN ?103 <> '' AND EXISTS (SELECT 1 FROM memories ams "                                       \
   " WHERE ams.id = " memory_id_sql                                                                \
   " AND ams.scope_type='workspace' AND ams.scope_value=?103) THEN 2 "                             \
   "WHEN EXISTS (SELECT 1 FROM memories ams WHERE ams.id = " memory_id_sql                         \
   " AND ((ams.scope_type='global' AND ams.scope_value='_global') "                                \
   " OR (ams.scope_type='workspace' AND ams.scope_value='_shared'))) THEN 1 ELSE 0 END"

#define DB2_MEMORY_SCOPE_FILTER_SQL(memory_id_sql)                                                 \
   " AND (?101 = 0 OR ?102 = 1 OR (" DB2_MEMORY_SCOPE_RANK_SQL(memory_id_sql) ") > 0)"

/* Normal recall is deliberately stricter than scope-only history/review
 * queries.  Lifecycle visibility is not feature-gated: rejected, archived,
 * pending, fulfilled, and superseded rows never enter an answer candidate set. */
#define DB2_MEMORY_RECALL_FILTER_SQL(memory_id_sql)                                                \
   " AND EXISTS (SELECT 1 FROM memories aml WHERE aml.id = " memory_id_sql                         \
   " AND aml.lifecycle_state='active' AND "                                                        \
   "aml.activation_suppressed=0)" DB2_MEMORY_SCOPE_FILTER_SQL(memory_id_sql)

typedef struct
{
   int active;
   int include_all;
   char workspace[512];
   char project[512];
   char scope_type[64];
   char scope_value[512];
} db2_memory_scope_context_t;

void db2_memory_scope_context_set(const char *workspace, const char *project, int include_all);
void db2_memory_scope_context_set_exact(const char *workspace, const char *project,
                                        const char *scope_type, const char *scope_value,
                                        int include_all);
void db2_memory_scope_context_restore(const db2_memory_scope_context_t *context);
void db2_memory_scope_context_clear(void);
void db2_memory_scope_context_get(db2_memory_scope_context_t *out);
int db2_memory_scope_context_rank(int64_t memory_id);

/* Rank every id in one statement instead of one per id. out_ranks must hold n
 * ints; an id with no row keeps rank 0. Returns the number of positions
 * ranked. Prefer this wherever a whole candidate set is being ranked. */
int db2_memory_scope_context_rank_batch(const int64_t *ids, int n, int *out_ranks);
int db2_memory_scope_context_allows(int64_t memory_id);
void db2_memory_scope_bind_current(aimee_pg_stmt_t *st);

#endif
