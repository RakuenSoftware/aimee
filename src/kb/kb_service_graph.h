/* kb_service_graph.h: aimee-kb dispatch handlers for the graph.* RPC family
 * (code projection sync and graph explain) that runs against KB_STORE. */
#ifndef DEC_KB_SERVICE_GRAPH_H
#define DEC_KB_SERVICE_GRAPH_H 1

#include "cJSON.h"
#include <stdint.h>

/* Build (publish a fresh generation of) the code projection graph for `project`,
 * but ONLY when its code changed since the last published generation
 * (content-addressed via the project fingerprint), so the curator drain stays
 * cheap when nothing changed. *rebuilt is set to 1 when a generation was
 * published, 0 when skipped. Returns edge count (>=0) on build, 0 on skip, -1 on
 * error. KB-side (in-process KB_STORE). */
int64_t kb_graph_build_project_if_changed(const char *project, int *rebuilt);

/* §3 provenance tag for a graph edge, derived (no column) from its origin +
 * structural-trust weight: "structural" (deterministic AST/index fact),
 * "inferred" (curator/semantic), or "ambiguous" (raw session co-occurrence). */
const char *kb_graph_edge_provenance(const char *edge_origin, int structural_weight);

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
