/* kb_service_graph.c: aimee-kb dispatch handlers for the graph.* RPC family.
 * Code projection sync and graph explain run against DB2 here, on the KB side;
 * server/CLI reach them only via kb_client RPC. */

#include "kb_service_graph.h"

#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "db2.h"
#include "db2/code_projection.h"
#include "db2/entity_edges.h"
#include "db2/entity_nodes.h"
#include "db2/kb_service_backend.h" /* db2_kb_service_code_audit_json */

#include <stdlib.h>
#include <string.h>

/* Defined in kb_service.c. */
int kb_send_error(int fd, const char *msg);
int kb_send_response(int fd, cJSON *resp);

int kb_handle_graph_sync_code(int fd, cJSON *req)
{
   cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(req, "project");
   if (!cJSON_IsString(proj_j) || !proj_j->valuestring[0])
      return kb_send_error(fd, "missing project");
   const char *project = proj_j->valuestring;

   if (!db2_is_initialized())
      return kb_send_error(fd, "failed to open knowledge service store");

   int64_t gen = db2_code_projection_generation_create(project);
   if (gen <= 0)
      return kb_send_error(fd, "failed to create projection generation");

   int64_t edges = db2_code_projection_sync_project(project, gen);
   if (edges < 0)
   {
      db2_code_projection_generation_abort(gen, "sync failed");
      return kb_send_error(fd, "code projection sync failed");
   }

   if (db2_code_projection_generation_publish(gen, project) != 0)
   {
      db2_code_projection_generation_abort(gen, "publish failed");
      return kb_send_error(fd, "failed to publish projection generation");
   }

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddNumberToObject(resp, "generation_id", (double)gen);
   cJSON_AddNumberToObject(resp, "edge_count", (double)edges);
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}

int kb_handle_graph_explain(int fd, cJSON *req)
{
   cJSON *ent_j = cJSON_GetObjectItemCaseSensitive(req, "entity");
   if (!cJSON_IsString(ent_j) || !ent_j->valuestring[0])
      return kb_send_error(fd, "missing entity");
   const char *entity = ent_j->valuestring;
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 40;
   if (limit <= 0 || limit > 200)
      limit = 40;

   if (!db2_is_initialized())
      return kb_send_error(fd, "failed to open knowledge service store");

   db2_entity_edge_explain_t edges[200];
   int n = db2_entity_edge_explain_by_entity(entity, edges, limit);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "entity", entity);
   /* Mark scoring values provisional per proposal. */
   cJSON_AddTrueToObject(resp, "provisional_weights");

   /* Canonical node metadata when present. */
   db2_entity_node_t node;
   if (db2_entity_node_get(entity, &node) == 0)
   {
      cJSON *nj = cJSON_CreateObject();
      cJSON_AddStringToObject(nj, "node_key", node.node_key);
      cJSON_AddNumberToObject(nj, "node_kind", node.node_kind);
      cJSON_AddStringToObject(nj, "project", node.project);
      cJSON_AddStringToObject(nj, "display_name", node.display_name);
      cJSON_AddStringToObject(nj, "file_path", node.file_path);
      cJSON_AddStringToObject(nj, "symbol", node.symbol);
      cJSON_AddStringToObject(nj, "node_origin", node.node_origin);
      cJSON_AddItemToObject(resp, "node", nj);
   }

   cJSON *arr = cJSON_AddArrayToObject(resp, "edges");
   for (int i = 0; i < n; i++)
   {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "source", edges[i].source);
      cJSON_AddStringToObject(e, "relation", edges[i].relation);
      cJSON_AddStringToObject(e, "target", edges[i].target);
      cJSON_AddNumberToObject(e, "weight", edges[i].weight);
      cJSON_AddNumberToObject(e, "structural_weight", edges[i].structural_weight);
      cJSON_AddNumberToObject(e, "utility_score", edges[i].utility_score);
      cJSON_AddStringToObject(e, "edge_origin", edges[i].edge_origin);
      cJSON_AddItemToArray(arr, e);
   }
   cJSON_AddNumberToObject(resp, "edge_count", n);
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}

/* code.audit: graph-derived code-health checks (dead exports, import cycles,
 * clones). Request {[project], [limit]}; response is the assembled bundle. */
int kb_handle_code_audit(int fd, cJSON *req)
{
   cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(req, "project");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *project = cJSON_IsString(proj_j) ? proj_j->valuestring : "";
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 50;

   if (!db2_is_initialized())
      return kb_send_error(fd, "failed to open knowledge service store");

   cJSON *resp = db2_kb_service_code_audit_json(project, limit);
   if (!resp)
      return kb_send_error(fd, "failed to build code audit");
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}
