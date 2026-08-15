/* kb_service_graph.c: aimee-kb dispatch handlers for the graph.* RPC family.
 * Code projection sync and graph explain run against DB2 here, on the KB side;
 * server/CLI reach them only via kb_client RPC. */

#include "kb_service_graph.h"

#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "modules/db2/c/db2.h"
#include "modules/db2/c/code_projection.h"
#include "modules/db2/c/entity_edges.h"
#include "modules/db2/c/entity_nodes.h"
#include "modules/db2/c/kb_service_backend.h" /* db2_kb_service_code_audit_json */
#include "kb_graph_analytics.h"               /* kb_graph_communities */

#include <stdlib.h>
#include <string.h>

/* Defined in kb_service.c. */
int kb_send_error(int fd, const char *msg);
int kb_send_response(int fd, cJSON *resp);

const char *kb_graph_edge_provenance(const char *edge_origin, int structural_weight)
{
   /* §3: derive the trust tag from the edge's source + structural-trust weight,
    * with NO new column. `structural` = deterministic AST/index facts (the only
    * tier a safety path may rely on, per §7); `inferred` = curator/semantic-
    * derived; `ambiguous` = raw session co-occurrence with no structural or
    * semantic grounding. */
   if (structural_weight > 0 || (edge_origin && strcmp(edge_origin, "code_projection") == 0))
      return "structural";
   if (edge_origin && strcmp(edge_origin, "session") == 0)
      return "ambiguous";
   return "inferred";
}

/* Upper bound on edges pulled into the in-memory community computation. Matches
 * the analytics read-out cap; a larger projection is truncated (deterministically,
 * by the source,target ORDER BY of the edge read). */
#define KB_GRAPH_COMMUNITY_MAX_EDGES 20000

/* Compute + persist deterministic community membership for a just-published
 * generation (graph-feedback S-community). Best-effort: a failure here does not
 * unpublish the projection — communities are a derived analytic. Returns the
 * number of nodes assigned, or -1 on error. */
static int kb_graph_persist_communities(int64_t gen_id, const char *project)
{
   if (gen_id <= 0)
      return -1;

   code_projection_edge_t *cpe = malloc((size_t)KB_GRAPH_COMMUNITY_MAX_EDGES * sizeof(*cpe));
   if (!cpe)
      return -1;
   int ne = db2_code_projection_list_edges_for_gen(gen_id, cpe, KB_GRAPH_COMMUNITY_MAX_EDGES);
   if (ne < 0)
   {
      free(cpe);
      return -1;
   }
   if (ne == 0)
   {
      /* No edges -> clear any stale membership for this generation. */
      free(cpe);
      return db2_code_projection_communities_replace(gen_id, project, NULL, 0);
   }

   kb_graph_edge_t *ge = malloc((size_t)ne * sizeof(*ge));
   if (!ge)
   {
      free(cpe);
      return -1;
   }
   for (int i = 0; i < ne; i++)
   {
      snprintf(ge[i].source, sizeof(ge[i].source), "%s", cpe[i].source);
      snprintf(ge[i].target, sizeof(ge[i].target), "%s", cpe[i].target);
      ge[i].weight = cpe[i].structural_weight;
   }
   free(cpe);

   /* Distinct nodes are bounded by 2 per edge. */
   long long max_nodes = (long long)ne * 2;
   kb_graph_community_t *gc = malloc((size_t)max_nodes * sizeof(*gc));
   if (!gc)
   {
      free(ge);
      return -1;
   }
   int nc = kb_graph_communities(ge, ne, gc, (int)max_nodes);
   free(ge);
   if (nc < 0)
   {
      free(gc);
      return -1;
   }

   code_projection_community_t *rows = nc > 0 ? malloc((size_t)nc * sizeof(*rows)) : NULL;
   if (nc > 0 && !rows)
   {
      free(gc);
      return -1;
   }
   for (int i = 0; i < nc; i++)
   {
      snprintf(rows[i].node_id, sizeof(rows[i].node_id), "%s", gc[i].node);
      snprintf(rows[i].community_id, sizeof(rows[i].community_id), "%s", gc[i].community);
   }
   free(gc);

   int rc = db2_code_projection_communities_replace(gen_id, project, rows, nc);
   free(rows);
   return rc == 0 ? nc : -1;
}

int64_t kb_graph_build_project_if_changed(const char *project, int *rebuilt)
{
   if (rebuilt)
      *rebuilt = 0;
   if (!project || !*project || !db2_is_initialized())
      return -1;
   /* Content-addressed idempotency: skip a project whose code is unchanged since
    * its last published generation, so the drain is cheap-when-nothing-changed. */
   char fp[64] = "", visible_fp[64] = "";
   if (db2_code_projection_project_fingerprint(project, fp, sizeof(fp)) != 0)
      return -1;
   db2_code_projection_visible_source_hash(project, visible_fp, sizeof(visible_fp));
   if (fp[0] && visible_fp[0] && strcmp(fp, visible_fp) == 0)
      return 0; /* unchanged -> no work */

   int64_t gen = db2_code_projection_generation_create(project);
   if (gen <= 0)
      return -1;
   db2_code_projection_generation_set_source_hash(gen, fp);
   int64_t edges = db2_code_projection_sync_project(project, gen);
   if (edges < 0)
   {
      db2_code_projection_generation_abort(gen, "sync failed");
      return -1;
   }
   if (db2_code_projection_generation_publish(gen, project) != 0)
   {
      db2_code_projection_generation_abort(gen, "publish failed");
      return -1;
   }
   /* Derived analytic: deterministic community membership for this generation.
    * Best-effort — a failure does not unpublish the projection. */
   kb_graph_persist_communities(gen, project);
   if (rebuilt)
      *rebuilt = 1;
   return edges; /* >= 0 (a changed-but-empty project publishes 0 edges + its hash) */
}

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
   /* Derived analytic (best-effort, see kb_graph_build_project_if_changed). */
   kb_graph_persist_communities(gen, project);

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
      cJSON_AddStringToObject(
          e, "provenance",
          kb_graph_edge_provenance(edges[i].edge_origin, (int)edges[i].structural_weight));
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
