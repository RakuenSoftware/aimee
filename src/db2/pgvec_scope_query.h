#ifndef AIMEE_DB2_PGVEC_SCOPE_QUERY_H
#define AIMEE_DB2_PGVEC_SCOPE_QUERY_H

#include "memory_scope_query.h"

/* memory_embeddings stores both memory points and offset memory-unit points.
 * Resolve either record type to its owning memory before applying the same
 * canonical scope tables used by lexical/SQL readers. This deliberately does
 * not trust the embedding row's denormalized scope columns: old rows can carry
 * only a legacy memory_workspaces tag, and scope tags can change after embed. */
#define PGVEC_MEMORY_OWNER_ID_SQL                                                                \
   "(CASE WHEN e.record_type = 'unit' THEN (SELECT mu.memory_id FROM memory_units mu "           \
   "WHERE mu.id = e.point_id - 1000000000000) ELSE e.point_id END)"

#define PGVEC_MEMORY_SCOPE_FILTER_SQL DB2_MEMORY_SCOPE_FILTER_SQL(PGVEC_MEMORY_OWNER_ID_SQL)
#define PGVEC_MEMORY_SCOPE_RANK_SQL   DB2_MEMORY_SCOPE_RANK_SQL(PGVEC_MEMORY_OWNER_ID_SQL)

#endif
