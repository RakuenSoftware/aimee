/* kb_service_graph.h: aimee-kb dispatch handlers for the graph.* RPC family
 * (code projection sync and graph explain) that runs against DB2. */
#ifndef DEC_KB_SERVICE_GRAPH_H
#define DEC_KB_SERVICE_GRAPH_H 1

#include "cJSON.h"

/* graph.sync_code: project the code index into entity_edges under a fresh
 * generation, publishing it atomically.  Request: {project}.
 * Response: {status, project, generation_id, edge_count, node_count}. */
int kb_handle_graph_sync_code(int fd, cJSON *req);

/* graph.explain: describe the entity_edges incident to a canonical node or
 * raw entity.  Request: {entity, [limit]}.
 * Response: {status, entity, edges:[{source,relation,target,weight,
 *            structural_weight,utility_score,edge_origin}], node:{...}}. */
int kb_handle_graph_explain(int fd, cJSON *req);

/* code.audit: graph-derived code-health checks over entity_edges +
 * code_embeddings.  Request: {[project], [limit]}.
 * Response: {status, dead_exports:[key], cycles:["a -> b -> a"],
 *            clones:[["sym  path", …]]}. */
int kb_handle_code_audit(int fd, cJSON *req);

#endif /* DEC_KB_SERVICE_GRAPH_H */
