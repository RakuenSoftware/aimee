/* server_state.c: server handlers for memory, index, rules, working memory, dashboard, workspace */
#include "server_state_internal.h"
#include "aimee.h"
#include "server.h"
#include "dashboard.h"
#include "render.h"               /* decision_to_json + db2_decision_log_list */
#include "audit_ledger.h"         /* audit_ledger_read — server-incurred tool-action audit */
#include "audit_worm.h"           /* audit_worm_verify/checkpoint — WORM audit store */
#include "server_http_identity.h" /* server_http_identity_query — audit pagination params */
#include "lsp.h"
#include "platform_path.h"
#include "workspace.h"
#include "workspace_mirror.h"
#include "workspace_provider.h"
#include "workspace_handle.h"
#include "workspace_runner_registry.h"
#include "forge_credentials.h"
#include "db1.h"
#include "kb_client.h"
#include "compute_pool.h"
#include "cJSON.h"
#include "json_fluent.h"
#include "dogfood.h"
#include "commands.h"
#include "platform_path.h"
#include "server_http.h"  /* session_primary_set/get/clear */
#include "agent_config.h" /* agent_load_config / agent_find */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Send a response object and delete it. Returns send rc. */
int send_and_free(server_conn_t *conn, cJSON *resp)
{
   return server_send_ok(conn, resp);
}

/* Write a JSON response and newline directly to a raw fd.
 * Used by kb proxy threads that run off the event loop. */
static void kb_proxy_write_response(int fd, cJSON *resp)
{
   char *json = cJSON_PrintUnformatted(resp);
   if (!json)
      return;
   size_t len = strlen(json);
   size_t off = 0;
   while (off < len)
   {
      ssize_t n = write(fd, json + off, len - off);
      if (n <= 0)
      {
         if (n < 0 && errno == EINTR)
            continue;
         break;
      }
      off += (size_t)n;
   }
   free(json);
   (void)write(fd, "\n", 1);
}

/* Generic context for kb proxy threads. */
typedef struct
{
   int conn_fd;
   cJSON *req;
} kb_proxy_ctx_t;

static kb_proxy_ctx_t *kb_proxy_ctx_new(server_conn_t *conn, cJSON *req)
{
   kb_proxy_ctx_t *c = malloc(sizeof(*c));
   if (!c)
      return NULL;
   c->conn_fd = conn->fd;
   c->req = cJSON_Duplicate(req, 1);
   if (!c->req)
   {
      free(c);
      return NULL;
   }
   return c;
}

static void kb_proxy_ctx_free(kb_proxy_ctx_t *c)
{
   if (!c)
      return;
   cJSON_Delete(c->req);
   free(c);
}

static void kb_proxy_spawn(server_conn_t *conn, cJSON *req, void *(*thread_fn)(void *))
{
   kb_proxy_ctx_t *c = kb_proxy_ctx_new(conn, req);
   if (!c)
      return;
   pthread_t tid;
   if (pthread_create(&tid, NULL, thread_fn, c) != 0)
   {
      kb_proxy_ctx_free(c);
      return;
   }
   pthread_detach(tid);
}

/* --- Memory handlers --- */

int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jkw = cJSON_GetObjectItemCaseSensitive(req, "keywords");
   int limit = jo_int(req, "limit", 10);

   if (!cJSON_IsArray(jkw) || cJSON_GetArraySize(jkw) == 0)
      return server_send_error(conn, "missing or empty keywords array", NULL);

   int count = cJSON_GetArraySize(jkw);
   if (count > 16)
      count = 16;

   char *clusters[16];
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_GetArrayItem(jkw, i);
      clusters[i] = cJSON_IsString(item) ? item->valuestring : "";
   }

   /* Build query string for fact search */
   char query_buf[2048];
   int qpos = 0;
   for (int i = 0; i < count; i++)
   {
      /* snprintf returns the would-be length, so a long keyword can run qpos
       * past the buffer; the next sizeof(query_buf) - qpos would then wrap to a
       * huge size_t and write out of bounds. Stop appending once full. */
      if (qpos >= (int)sizeof(query_buf) - 1)
         break;
      if (i > 0)
         qpos += snprintf(query_buf + qpos, sizeof(query_buf) - qpos, " ");
      qpos += snprintf(query_buf + qpos, sizeof(query_buf) - qpos, "%s", clusters[i]);
   }

   /* Search stored facts; graph-code fusion is always on for recall. */
   memory_t facts[32];
   int fact_count = kb_client_memory_find_facts_ex(query_buf, limit, facts, 32, "on");
   if (fact_count < 0)
      return server_send_error(conn,
                               "knowledge service search index unavailable; server-side "
                               "maintenance is required",
                               NULL);

   /* Search conversation windows */
   search_result_t results[32];
   int found = kb_client_memory_search(clusters, count, limit, results, 32);

   cJSON *farr = cJSON_CreateArray();
   for (int i = 0; i < fact_count; i++)
      cJSON_AddItemToArray(farr, memory_to_json(&facts[i]));

   cJSON *warr = cJSON_CreateArray();
   for (int i = 0; i < found; i++)
   {
      cJSON *r = cJSON_CreateObject();
      jo_add_str(r, "session_id", results[i].session_id);
      jo_add_i64(r, "seq", results[i].seq);
      jo_add_str(r, "summary", results[i].summary);
      jo_add_num(r, "score", results[i].score);
      cJSON_AddItemToArray(warr, r);
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "facts", farr);
   cJSON_AddItemToObject(resp, "windows", warr);
   return send_and_free(conn, resp);
}

int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *key, *content;
   if (jo_need_str(req, "key", &key) < 0 || jo_need_str(req, "content", &content) < 0)
      return server_send_error(conn, "missing key or content", NULL);

   const char *tier = jo_str(req, "tier", TIER_L0);
   const char *kind = jo_str(req, "kind", KIND_FACT);
   double confidence = jo_num(req, "confidence", 1.0);
   const char *sid = jo_str(req, "session_id", "");

   memory_t out;
   int rc = kb_client_memory_insert(tier, kind, key, content, confidence, sid, &out);

   cJSON *resp;
   if (rc == 0)
   {
      resp = jo_ok();
      jo_add_i64(resp, "id", out.id);
   }
   else
   {
      resp = jo_err("failed to store memory");
   }
   return send_and_free(conn, resp);
}

int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *tier = jo_str(req, "tier", NULL);
   const char *kind = jo_str(req, "kind", NULL);
   int limit = jo_int(req, "limit", 20);

   memory_t results[64];
   int count = kb_client_memory_list(tier, kind, limit, results, 64);
   if (count < 0)
      return server_send_error(conn,
                               "knowledge service unavailable; the memory store is unreachable "
                               "(server-side maintenance is required)",
                               NULL);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(arr, memory_to_json(&results[i]));

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "memories", arr);
   return send_and_free(conn, resp);
}

int handle_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = kb_client_memory_stats_json();
   if (!json)
      return server_send_error(conn,
                               "knowledge service unavailable; the memory store is unreachable "
                               "(server-side maintenance is required)",
                               NULL);
   cJSON *stats = cJSON_Parse(json);
   free(json);
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "stats", stats ? stats : cJSON_CreateObject());
   return send_and_free(conn, resp);
}

int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(jid))
      return server_send_error(conn, "missing id", NULL);

   memory_t m;
   int rc = kb_client_memory_get((int64_t)jid->valuedouble, &m);

   cJSON *resp;
   if (rc == 0)
   {
      resp = jo_ok();
      cJSON *mj = memory_to_json(&m);
      if (mj)
      {
         /* Merge memory fields into resp */
         cJSON *child = mj->child;
         while (child)
         {
            cJSON *next = child->next;
            cJSON_DetachItemViaPointer(mj, child);
            cJSON_AddItemToObject(resp, child->string, child);
            child = next;
         }
         cJSON_Delete(mj);
      }
   }
   else
   {
      resp = jo_err("memory not found");
   }
   return send_and_free(conn, resp);
}

int handle_memory_read(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *context = kb_client_memory_assemble_context(NULL);
   cJSON *resp = jo_ok();
   jo_add_str(resp, "context", context ? context : "");
   free(context);
   return send_and_free(conn, resp);
}

/* --- Index handlers --- */

int handle_index_scan(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *name = jo_str(req, "name", NULL);
   const char *root = jo_str(req, "root", NULL);
   if ((name && name[0] && (!root || !root[0])) || (root && root[0] && (!name || !name[0])))
      return server_send_error(conn, "index.scan requires both name and root, or neither", NULL);

   /* Synchronous (inline kb scan + send_and_free), like handle_index_ingest:
    * over /v1 this runs in the async op-run worker (HTTP already returned the
    * run handle) and over NDJSON in the per-connection worker, so blocking here
    * is fine. A kb_proxy_spawn detached-thread reply is NOT captured by the
    * op-run's loopback_rpc (it reads the socketpair synchronously), which is why
    * a remote /v1 index.scan previously failed with "rpc produced no response". */
   int force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "force")) ? 1 : 0;
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   int kb_rc = kb_client_index_scan(name, root, force, &res);
   cJSON *resp = (cJSON *)kb_client_index_scan_format_response(kb_rc, &res);
   return send_and_free(conn, resp);
}

int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *identifier;
   if (jo_need_str(req, "identifier", &identifier) < 0 || !identifier[0])
      return server_send_error(conn, "missing identifier", NULL);

   term_hit_t hits[128];
   int count = kb_client_index_find(identifier, hits, 128);

   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; i < count; i++)
   {
      cJSON *h = cJSON_CreateObject();
      jo_add_str(h, "project", hits[i].project);
      jo_add_str(h, "file_path", hits[i].file_path);
      cJSON_AddNumberToObject(h, "line", hits[i].line);
      jo_add_str(h, "kind", hits[i].kind);
      cJSON_AddItemToArray(arr, h);
   }
   return send_and_free(conn, resp);
}

int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   project_info_t projects[128];
   int count = kb_client_index_list(projects, 128);

   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "projects");
   for (int i = 0; i < count; i++)
   {
      cJSON *p = cJSON_CreateObject();
      jo_add_str(p, "name", projects[i].name);
      jo_add_str(p, "root", projects[i].root);
      jo_add_str(p, "scanned_at", projects[i].scanned_at);
      cJSON_AddItemToArray(arr, p);
   }
   return send_and_free(conn, resp);
}

int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *project, *file_path;
   if (jo_need_str(req, "project", &project) < 0 || jo_need_str(req, "file_path", &file_path) < 0)
      return server_send_error(conn, "missing project or file_path", NULL);

   blast_radius_t br;
   int rc = kb_client_index_blast_radius(project, file_path, &br);

   cJSON *resp;
   if (rc == 0)
   {
      resp = jo_ok();
      jo_add_str(resp, "file", br.file);

      cJSON *deps = cJSON_CreateArray();
      for (int i = 0; i < br.dependency_count; i++)
         cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependencies[i]));
      cJSON_AddItemToObject(resp, "dependencies", deps);

      cJSON *depts = cJSON_CreateArray();
      for (int i = 0; i < br.dependent_count; i++)
         cJSON_AddItemToArray(depts, cJSON_CreateString(br.dependents[i]));
      cJSON_AddItemToObject(resp, "dependents", depts);
   }
   else
   {
      resp = jo_err("blast radius lookup failed (knowledge service unavailable)");
   }
   return send_and_free(conn, resp);
}

int handle_index_structure(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project, *file_path;
   if (jo_need_str(req, "project", &project) < 0 || jo_need_str(req, "file_path", &file_path) < 0)
      return server_send_error(conn, "missing project or file_path", NULL);
   definition_t defs[256];
   int count = kb_client_index_structure(project, file_path, defs, 256);
   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "definitions");
   for (int i = 0; i < count; i++)
   {
      cJSON *d = cJSON_CreateObject();
      jo_add_str(d, "name", defs[i].name);
      jo_add_str(d, "kind", defs[i].kind);
      cJSON_AddNumberToObject(d, "line", defs[i].line);
      /* Span-propagation parity (#33): emit line_end like the KB/MCP routes do. */
      if (defs[i].line_end)
         cJSON_AddNumberToObject(d, "line_end", defs[i].line_end);
      cJSON_AddItemToArray(arr, d);
   }
   return send_and_free(conn, resp);
}

int handle_index_find_callers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *symbol;
   if (jo_need_str(req, "symbol", &symbol) < 0 || !symbol[0])
      return server_send_error(conn, "missing symbol", NULL);
   const char *project = jo_str(req, "project", NULL);

   caller_hit_t hits[128];
   int count = kb_client_index_find_callers(project, symbol, hits, 128);

   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "hits");
   for (int i = 0; i < count; i++)
   {
      cJSON *h = cJSON_CreateObject();
      jo_add_str(h, "project", hits[i].project);
      jo_add_str(h, "file_path", hits[i].file_path);
      jo_add_str(h, "caller", hits[i].caller);
      cJSON_AddNumberToObject(h, "line", hits[i].line);
      cJSON_AddItemToArray(arr, h);
   }
   return send_and_free(conn, resp);
}

/* --- Graph code-projection handlers --- */

/* graph.sync_code runs the code-graph projection (DB2-heavy) off the request
 * lane via kb_proxy_spawn, the same non-blocking pattern as index.scan, so it
 * never ties up the ephemeral request handler. */
static void *graph_sync_code_thread(void *arg)
{
   kb_proxy_ctx_t *c = arg;
   const char *project = jo_str(c->req, "project", NULL);

   kb_client_graph_sync_result_t res;
   memset(&res, 0, sizeof(res));
   int rc = kb_client_graph_sync_code(project ? project : "", &res);

   cJSON *resp = cJSON_CreateObject();
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "project", res.project);
      cJSON_AddNumberToObject(resp, "generation_id", (double)res.generation_id);
      cJSON_AddNumberToObject(resp, "edge_count", (double)res.edge_count);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "graph sync-code failed");
   }
   kb_proxy_write_response(c->conn_fd, resp);
   cJSON_Delete(resp);
   kb_proxy_ctx_free(c);
   return NULL;
}

int handle_graph_sync_code(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *project = jo_str(req, "project", NULL);
   if (!project || !project[0])
      return server_send_error(conn, "graph.sync_code requires a project", NULL);
   kb_proxy_spawn(conn, req, graph_sync_code_thread);
   return 0;
}

int handle_graph_explain(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *entity = jo_str(req, "entity", NULL);
   if (!entity || !entity[0])
      return server_send_error(conn, "graph.explain requires an entity", NULL);
   int limit = jo_int(req, "limit", 40);

   char *json = kb_client_graph_explain_json(entity, limit);
   if (!json)
      return server_send_error(conn, "graph explain failed (kb unreachable)", NULL);
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return server_send_error(conn, "graph explain: malformed kb response", NULL);
   return send_and_free(conn, resp);
}

int handle_blast_radius_preview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jpaths = cJSON_GetObjectItemCaseSensitive(req, "paths");
   const char *project;
   if (jo_need_str(req, "project", &project) < 0 || !cJSON_IsArray(jpaths))
      return server_send_error(conn, "missing project or paths array", NULL);

   int count = cJSON_GetArraySize(jpaths);
   if (count <= 0 || count > 100)
      return server_send_error(conn, "paths must contain 1-100 file paths", NULL);

   char *paths[100];
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_GetArrayItem(jpaths, i);
      paths[i] = cJSON_IsString(item) ? item->valuestring : "";
   }

   char *json = kb_client_index_blast_radius_preview_json(project, paths, count);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);

   if (!resp)
      return server_send_error(conn, "knowledge service did not return blast-radius preview", NULL);
   return send_and_free(conn, resp);
}

/* --- KB handlers --- */

/* Auditable-correctness /v1/audit/trace handler (kept in a textual include to
 * stay under the per-file line cap). */

/* CSS migration assistant /v1/css/signals handler — a thin forward to the KB
 * (textual include for the line cap). */

int handle_kb_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *query = jo_str(req, "query", "");
   const char *project = jo_str(req, "project", NULL);
   int max_results = jo_int(req, "max_results", 10);
   const char *fusion_mode = jo_str(req, "fusion_mode", NULL);
   const char *embed_cmd = jo_str(req, "embedding_command", NULL);

   char *json =
       kb_client_search_json_ex(project, query, embed_cmd, max_results, "json", fusion_mode);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service search failed", NULL);
   return send_and_free(conn, resp);
}

int handle_curator_implements(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *topic = jo_str(req, "topic", "");
   if (!topic[0])
      return server_send_error(conn, "curator implements requires a topic", NULL);
   char *json = kb_client_curator_implements_json(topic);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service /v1/implements failed", NULL);
   return send_and_free(conn, resp);
}

int handle_curator_synthesize(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *topic = jo_str(req, "topic", "");
   if (!topic[0])
      return server_send_error(conn, "curator synthesize requires a topic", NULL);
   char *json = kb_client_curator_synthesize_json(topic);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service /v1/synthesize failed", NULL);
   return send_and_free(conn, resp);
}

int handle_curator_contradictions(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   int limit = jo_int(req, "limit", 20);
   char *json = kb_client_curator_contradictions_json(limit);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service /v1/contradictions failed", NULL);
   return send_and_free(conn, resp);
}

int handle_curator_invalidated(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   /* Inbound push from aimee-kb: a source doc's derived curator artifacts were
    * invalidated. The subscriber (this server) has now received the event. */
   const char *source_kind = jo_str(req, "source_kind", "");
   const char *source_id = jo_str(req, "source_id", "");
   int stale = jo_int(req, "artifacts_stale", 0);
   (void)source_kind;
   (void)source_id;
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "received", stale);
   return send_and_free(conn, resp);
}

/* --- kb proxy threads (build / update / ingest) ---
 * These call aimee-kb which can take minutes for large repos.  Running them
 * synchronously on the event-loop thread would block the server for the entire
 * duration, making every other aimee command appear frozen.  Each handler
 * spawns a detached thread that performs the kb RPC and writes the response
 * directly to the connection fd when it is ready. */

static void *kb_build_thread(void *arg)
{
   kb_proxy_ctx_t *c = arg;
   const char *path = jo_str(c->req, "path", "");
   const char *project = jo_str(c->req, "project", "");
   const char *embed_cmd = jo_str(c->req, "embedding_command", NULL);
   int force = jo_int(c->req, "force", 0);

   char *json = kb_client_build_json(path, project, embed_cmd, force);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      resp = cJSON_Parse("{\"status\":\"error\",\"message\":\"knowledge service build failed\"}");
   if (resp)
   {
      kb_proxy_write_response(c->conn_fd, resp);
      cJSON_Delete(resp);
   }
   kb_proxy_ctx_free(c);
   return NULL;
}

static void *kb_update_thread(void *arg)
{
   kb_proxy_ctx_t *c = arg;
   const char *path = jo_str(c->req, "path", "");
   const char *project = jo_str(c->req, "project", "");
   const char *embed_cmd = jo_str(c->req, "embedding_command", NULL);

   char *json = kb_client_update_json(path, project, embed_cmd);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      resp = cJSON_Parse("{\"status\":\"error\",\"message\":\"knowledge service update failed\"}");
   if (resp)
   {
      kb_proxy_write_response(c->conn_fd, resp);
      cJSON_Delete(resp);
   }
   kb_proxy_ctx_free(c);
   return NULL;
}

static void *kb_ingest_thread(void *arg)
{
   kb_proxy_ctx_t *c = arg;
   const char *workspace = jo_str(c->req, "workspace", NULL);
   const char *embed_cmd = jo_str(c->req, "embedding_command", NULL);
   int force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c->req, "force")) ? 1 : 0;

   char *json = kb_client_ingest_json(workspace, embed_cmd, force);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      resp = cJSON_Parse("{\"status\":\"error\",\"message\":\"knowledge service ingest failed\"}");
   if (resp)
   {
      /* aimee-kb wakes its own ingest workers on enqueue (kb_handle_ingest),
       * so the server just relays the response. */
      kb_proxy_write_response(c->conn_fd, resp);
      cJSON_Delete(resp);
   }
   kb_proxy_ctx_free(c);
   return NULL;
}

static void *kb_docs_push_thread(void *arg)
{
   kb_proxy_ctx_t *c = arg;
   const char *scope = jo_str(c->req, "scope", "global");
   cJSON *paths_j = cJSON_GetObjectItemCaseSensitive(c->req, "paths");

   const char *paths[128];
   int path_count = 0;
   if (cJSON_IsArray(paths_j))
   {
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, paths_j)
      {
         if (path_count >= (int)(sizeof(paths) / sizeof(paths[0])))
            break;
         if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
            paths[path_count++] = item->valuestring;
      }
   }

   cJSON *resp = NULL;
   if (path_count <= 0)
   {
      resp = cJSON_Parse("{\"status\":\"error\",\"message\":\"docs paths required\"}");
   }
   else
   {
      char *json = kb_client_docs_push_json(scope, paths, path_count);
      resp = json ? cJSON_Parse(json) : NULL;
      free(json);
      if (!resp)
         resp = cJSON_Parse("{\"status\":\"error\",\"message\":\"knowledge service docs push "
                            "failed\"}");
   }

   if (resp)
   {
      kb_proxy_write_response(c->conn_fd, resp);
      cJSON_Delete(resp);
   }
   kb_proxy_ctx_free(c);
   return NULL;
}

int handle_kb_build(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   kb_proxy_spawn(conn, req, kb_build_thread);
   return 0;
}

int handle_kb_update(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   kb_proxy_spawn(conn, req, kb_update_thread);
   return 0;
}

int handle_kb_ingest(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   kb_proxy_spawn(conn, req, kb_ingest_thread);
   return 0;
}

int handle_kb_docs_push(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   kb_proxy_spawn(conn, req, kb_docs_push_thread);
   return 0;
}

int handle_kb_ingest_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = kb_client_ingest_status_json();
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service ingest status failed", NULL);

   /* aimee-kb owns ingest and reports its own worker count in the response. */
   return send_and_free(conn, resp);
}

/* Forward decl (server_compute_impl.h drags in unrelated compute internals). */
void delegate_ondemand_add_workers_stats(cJSON *out);

int handle_workers(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)req;

   cJSON *resp = jo_ok();

   /* Server-side pools. Sessionless server work runs on the compute pool;
    * session-bound chat/tool/delegate work runs on per-session pools. KB
    * ingest now runs inside aimee-kb and is surfaced under the "kb" object. */
   if (ctx)
   {
      cJSON *request_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(request_obj, "role", "request");
      cJSON_AddNumberToObject(request_obj, "configured",
                              compute_pool_thread_count(&ctx->request_pool));
      char *request_slots = compute_pool_slots_json(&ctx->request_pool);
      if (request_slots)
      {
         cJSON *slots = cJSON_Parse(request_slots);
         free(request_slots);
         if (slots)
            cJSON_AddItemToObject(request_obj, "slots", slots);
      }
      cJSON_AddItemToObject(resp, "request", request_obj);

      cJSON *compute_obj = cJSON_CreateObject();
      cJSON_AddStringToObject(compute_obj, "role", "compute");
      cJSON_AddNumberToObject(compute_obj, "configured", compute_pool_thread_count(&ctx->pool));
      char *compute_slots = compute_pool_slots_json(&ctx->pool);
      if (compute_slots)
      {
         cJSON *slots = cJSON_Parse(compute_slots);
         free(compute_slots);
         if (slots)
            cJSON_AddItemToObject(compute_obj, "slots", slots);
      }
      cJSON_AddItemToObject(resp, "compute", compute_obj);

      /* Sessionless delegates run on on-demand threads gated by the per-model
       * limiter, not a fixed pool slot. Surface the live count, its peak, and
       * the cumulative total (confirms the limiter is the binding throttle). */
      delegate_ondemand_add_workers_stats(resp);

      char *session_json = server_session_pools_json(ctx);
      if (session_json)
      {
         cJSON *session_pools = cJSON_Parse(session_json);
         free(session_json);
         if (session_pools)
            cJSON_AddItemToObject(resp, "session_pools", session_pools);
      }
   }

   cJSON *async = server_compute_async_json(ctx);
   if (async)
      cJSON_AddItemToObject(resp, "async", async);

   /* Secondary pools (e.g. an in-flight verify run with its own ephemeral
    * pool). Reported as a heterogeneous array so future ephemeral pools
    * appear automatically without changing the JSON shape. */
   char *secondary_json = compute_pool_secondary_pools_json();
   if (secondary_json)
   {
      cJSON *secondary = cJSON_Parse(secondary_json);
      free(secondary_json);
      if (secondary)
         cJSON_AddItemToObject(resp, "secondary_pools", secondary);
   }

   /* aimee-kb connection workers */
   char *kb_json = kb_client_workers_json();
   if (kb_json)
   {
      cJSON *kb_resp = cJSON_Parse(kb_json);
      free(kb_json);
      if (kb_resp)
      {
         cJSON *kb_obj = cJSON_CreateObject();
         cJSON_AddStringToObject(kb_obj, "role", "connection");
         cJSON *configured = cJSON_GetObjectItemCaseSensitive(kb_resp, "configured");
         if (cJSON_IsNumber(configured))
            cJSON_AddNumberToObject(kb_obj, "configured", configured->valuedouble);
         cJSON *slots = cJSON_DetachItemFromObjectCaseSensitive(kb_resp, "slots");
         if (slots)
            cJSON_AddItemToObject(kb_obj, "slots", slots);
         /* Pass through the kb-side autonomous-task registry so `aimee
          * workers` can render curator/maintenance/etc. activity. */
         cJSON *background = cJSON_DetachItemFromObjectCaseSensitive(kb_resp, "background");
         if (background)
            cJSON_AddItemToObject(kb_obj, "background", background);
         cJSON *threads = cJSON_DetachItemFromObjectCaseSensitive(kb_resp, "threads");
         if (threads)
            cJSON_AddItemToObject(kb_obj, "threads", threads);
         cJSON_AddItemToObject(resp, "kb", kb_obj);
         cJSON_Delete(kb_resp);
      }
   }

   return send_and_free(conn, resp);
}

int handle_kb_status(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *project = jo_str(req, "project", NULL);

   char *json =
       project && project[0] ? kb_client_project_status_json(project) : kb_client_status_json();
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service status failed", NULL);
   return send_and_free(conn, resp);
}

/* optimize.export: surface the kb bandit/optimization export (decision-point
 * registry + per-point arm baselines + closed-decision log) over a first-class
 * /v1 route so the thin client's `aimee optimize` can reach it. */
int handle_optimize_export(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json = kb_client_bandit_export_json();
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "bandit optimization export failed", NULL);
   return send_and_free(conn, resp);
}

/* optimize.promote: persist the production-default arm for a decision point via
 * the kb DB2 bandit. Request: { decision_point, arm }. */
int handle_optimize_promote(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *dp = jo_str(req, "decision_point", NULL);
   const char *arm = jo_str(req, "arm", NULL);
   if (!dp || !dp[0] || !arm || !arm[0])
      return server_send_error(conn, "decision_point and arm are required", NULL);

   char *json = kb_client_bandit_promote_json(dp, arm);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "bandit promotion failed", NULL);
   return send_and_free(conn, resp);
}

/* calibration.readiness: surface the kb calibration-readiness report over a
 * first-class /v1 route (was kb-only; reachable now via `aimee kb calibrate`). */
int handle_calibration_readiness(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json = kb_client_calibrate_readiness_json();
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "calibration readiness check failed", NULL);
   return send_and_free(conn, resp);
}

/* demotion.check: surface the kb demotion dry-run report over a first-class /v1
 * route (was kb-only; reachable now via `aimee kb demote`). */
int handle_demotion_check(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json = kb_client_demote_check_json();
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "demotion check failed", NULL);
   return send_and_free(conn, resp);
}

/* optimize.replay_record: record off-policy replay attribution (output of
 * tools/bandit_replay.py) as a benchmark_trace artifact. Body: {decision_point,
 * result}. Reachable via `aimee optimize replay-record --point X --file F`. */
int handle_optimize_replay_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *dp = jo_str(req, "decision_point", NULL);
   if (!dp || !dp[0])
      return server_send_error(conn, "decision_point is required", NULL);

   cJSON *result = cJSON_GetObjectItemCaseSensitive(req, "result");
   if (!cJSON_IsObject(result))
      return server_send_error(conn, "result (object) is required", NULL);
   char *result_json = cJSON_PrintUnformatted(result);
   if (!result_json)
      return server_send_error(conn, "failed to serialize result", NULL);

   char *json = kb_client_bandit_replay_record_json(dp, result_json);
   free(result_json);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "replay-record failed", NULL);
   return send_and_free(conn, resp);
}

/* --- Rules handlers --- */

int handle_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json = kb_client_rules_list_json(64);
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service did not return rules", NULL);
   return send_and_free(conn, resp);
}

int handle_rules_generate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json = kb_client_rules_generate_json();
   cJSON *resp = json ? cJSON_Parse(json) : NULL;
   free(json);
   if (!resp)
      return server_send_error(conn, "knowledge service did not return rules markdown", NULL);
   return send_and_free(conn, resp);
}

int handle_rules_delete(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   int id = jo_int(req, "id", 0);
   if (id <= 0)
      return server_send_error(conn, "rules.delete requires id", NULL);

   if (kb_client_rules_delete(id) != 0)
      return server_send_error(conn, "knowledge service did not delete rule", NULL);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "id", id);
   return send_and_free(conn, resp);
}

/* --- Collab rules handlers --- */

int handle_collab_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = kb_client_collab_rules_list_json();
   cJSON *resp = jo_ok();
   cJSON *arr = json ? cJSON_Parse(json) : cJSON_CreateArray();
   free(json);
   cJSON_AddItemToObject(resp, "rules", arr ? arr : cJSON_CreateArray());
   return send_and_free(conn, resp);
}

int handle_collab_rules_list_active(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = kb_client_collab_rules_list_active_json();
   cJSON *resp = jo_ok();
   cJSON *obj = json ? cJSON_Parse(json) : cJSON_CreateObject();
   free(json);
   cJSON_AddItemToObject(resp, "active", obj ? obj : cJSON_CreateObject());
   return send_and_free(conn, resp);
}

static int collab_rule_action(server_conn_t *conn, cJSON *req, int (*fn)(int))
{
   int id = jo_int(req, "id", 0);
   if (id <= 0)
      return server_send_error(conn, "id required", NULL);
   if (fn(id) != 0)
      return server_send_error(conn, "collab rule action failed", NULL);
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "id", id);
   return send_and_free(conn, resp);
}

int handle_collab_rules_approve(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return collab_rule_action(conn, req, kb_client_collab_rules_approve);
}

int handle_collab_rules_reject(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return collab_rule_action(conn, req, kb_client_collab_rules_reject);
}

int handle_collab_rules_retire(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   return collab_rule_action(conn, req, kb_client_collab_rules_retire);
}

/* --- Working memory handlers --- */

int handle_wm_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid, *key, *val;
   if (jo_need_str(req, "session_id", &sid) < 0 || jo_need_str(req, "key", &key) < 0 ||
       jo_need_str(req, "value", &val) < 0)
      return server_send_error(conn, "missing session_id, key, or value", NULL);

   const char *category = jo_str(req, "category", "general");
   int ttl = jo_int(req, "ttl", 0);

   int rc = db1_wm_set(sid, key, val, category, ttl);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   return send_and_free(conn, resp);
}

int handle_wm_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid, *key;
   if (jo_need_str(req, "session_id", &sid) < 0 || jo_need_str(req, "key", &key) < 0)
      return server_send_error(conn, "missing session_id or key", NULL);

   wm_entry_t entry;
   int rc = db1_wm_get(sid, key, &entry);

   cJSON *resp;
   if (rc == 0)
   {
      resp = jo_ok();
      jo_add_str(resp, "key", entry.key);
      jo_add_str(resp, "value", entry.value);
      jo_add_str(resp, "category", entry.category);
      jo_add_str(resp, "updated_at", entry.updated_at);
   }
   else
   {
      resp = jo_err("key not found or expired");
   }
   return send_and_free(conn, resp);
}

/* --- Per-session primary agent handlers (mirror the /v1/sessions/<id>/primary
 *     HTTP routes; both transports share the in-memory store). --- */

int handle_primary_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid, *agent;
   if (jo_need_str(req, "session_id", &sid) < 0 || jo_need_str(req, "agent", &agent) < 0)
      return server_send_error(conn, "missing session_id or agent", NULL);

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || !agent_find(&acfg, agent))
      return server_send_error(conn, "no such agent", NULL);

   session_primary_set(sid, agent);
   cJSON *resp = jo_ok();
   jo_add_str(resp, "agent", agent);
   return send_and_free(conn, resp);
}

int handle_primary_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid;
   if (jo_need_str(req, "session_id", &sid) < 0)
      return server_send_error(conn, "missing session_id", NULL);

   char agent[MAX_AGENT_NAME] = "";
   session_primary_get(sid, agent, sizeof(agent));
   cJSON *resp = jo_ok();
   jo_add_str(resp, "agent", agent);
   return send_and_free(conn, resp);
}

int handle_primary_clear(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid;
   if (jo_need_str(req, "session_id", &sid) < 0)
      return server_send_error(conn, "missing session_id", NULL);

   session_primary_clear(sid);
   cJSON *resp = jo_ok();
   jo_add_str(resp, "agent", "");
   return send_and_free(conn, resp);
}

int handle_wm_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid;
   if (jo_need_str(req, "session_id", &sid) < 0)
      return server_send_error(conn, "missing session_id", NULL);

   const char *category = jo_str(req, "category", NULL);

   wm_entry_t entries[WM_MAX_RESULTS];
   int count = db1_wm_list(sid, category, entries, WM_MAX_RESULTS);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *e = cJSON_CreateObject();
      jo_add_str(e, "key", entries[i].key);
      jo_add_str(e, "value", entries[i].value);
      jo_add_str(e, "category", entries[i].category);
      jo_add_str(e, "updated_at", entries[i].updated_at);
      cJSON_AddItemToArray(arr, e);
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "entries", arr);
   return send_and_free(conn, resp);
}

int handle_wm_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid;
   if (jo_need_str(req, "session_id", &sid) < 0)
      return server_send_error(conn, "missing session_id", NULL);

   char *context = db1_wm_assemble_context(sid);
   cJSON *resp = jo_ok_kv("context", context ? context : "");
   free(context);
   return send_and_free(conn, resp);
}

/* --- Attempt log handlers --- */

int handle_attempt_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid, *approach, *outcome;
   if (jo_need_str(req, "session_id", &sid) < 0 || jo_need_str(req, "approach", &approach) < 0 ||
       jo_need_str(req, "outcome", &outcome) < 0)
      return server_send_error(conn, "missing session_id, approach, or outcome", NULL);

   /* Build structured JSON value */
   cJSON *val = cJSON_CreateObject();
   jo_add_str(val, "task_context", jo_str(req, "task_context", ""));
   jo_add_str(val, "approach", approach);
   jo_add_str(val, "outcome", outcome);
   jo_add_str(val, "lesson", jo_str(req, "lesson", ""));

   char *json_val = cJSON_PrintUnformatted(val);
   cJSON_Delete(val);
   if (!json_val)
      return server_send_error(conn, "failed to serialize attempt", NULL);

   /* Generate a unique key: attempt:<counter> */
   char key[64];
   static _Atomic int attempt_counter = 0;
   int cnt = ++attempt_counter;
   snprintf(key, sizeof(key), "attempt:%d", cnt);

   /* Store with 'attempt' category, 4-hour TTL (session-scoped) */
   int rc = db1_wm_set(sid, key, json_val, "attempt", 14400);
   free(json_val);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc == 0)
      jo_add_str(resp, "key", key);
   return send_and_free(conn, resp);
}

int handle_attempt_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   const char *sid;
   if (jo_need_str(req, "session_id", &sid) < 0)
      return server_send_error(conn, "missing session_id", NULL);

   const char *filter = jo_str(req, "filter", NULL);

   wm_entry_t entries[WM_MAX_RESULTS];
   int count = db1_wm_list(sid, "attempt", entries, WM_MAX_RESULTS);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      /* Parse the structured value */
      cJSON *val = cJSON_Parse(entries[i].value);
      if (!val)
         continue;

      /* Apply keyword filter if provided */
      if (filter && filter[0])
      {
         const char *tc = jo_str(val, "task_context", "");
         const char *ap = jo_str(val, "approach", "");
         if (!strstr(tc, filter) && !strstr(ap, filter))
         {
            cJSON_Delete(val);
            continue;
         }
      }

      jo_add_str(val, "key", entries[i].key);
      jo_add_str(val, "recorded_at", entries[i].created_at);
      cJSON_AddItemToArray(arr, val);
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "attempts", arr);
   return send_and_free(conn, resp);
}

/* --- Dashboard handlers --- */

int handle_dashboard_metrics(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json_str = api_metrics();

   cJSON *resp = jo_ok();
   if (json_str)
   {
      cJSON *data = cJSON_Parse(json_str);
      if (data)
         cJSON_AddItemToObject(resp, "data", data);
      free(json_str);
   }
   json_str = api_vector_status();
   if (json_str)
   {
      cJSON *status = cJSON_Parse(json_str);
      if (status)
         cJSON_AddItemToObject(resp, "vector", status);
      free(json_str);
   }
   return send_and_free(conn, resp);
}

int handle_dashboard_delegations(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json_str = api_delegations();

   cJSON *resp = jo_ok();
   if (json_str)
   {
      cJSON *data = cJSON_Parse(json_str);
      if (data)
         cJSON_AddItemToObject(resp, "data", data);
      free(json_str);
   }
   return send_and_free(conn, resp);
}

/* --- Workspace handler --- */

int handle_workspace_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   config_t cfg;
   config_load(&cfg);
   char *context = workspace_build_context_from_config(&cfg);

   cJSON *resp = jo_ok_kv("context", context ? context : "");
   free(context);
   return send_and_free(conn, resp);
}

/* --- Workspace registry handlers (add / list / remove) ---
 * The thin client has no local config; `aimee workspace <sub>` routes here so
 * the workspace registry (config workspaces[]) and project indexing both stay
 * server-side, mirroring the legacy cmd_workspace implementation. */

int workspace_rpc_args(cJSON *req, char **argv, int max)
{
   cJSON *args = cJSON_GetObjectItemCaseSensitive(req, "args");
   if (!cJSON_IsArray(args))
      return 0;
   int n = cJSON_GetArraySize(args);
   if (n > max)
      n = max;
   for (int i = 0; i < n; i++)
   {
      cJSON *a = cJSON_GetArrayItem(args, i);
      argv[i] = (char *)(cJSON_IsString(a) ? a->valuestring : "");
   }
   return n;
}

/* handle_workspace_add lives in server_runner_endpoints.inc (below) to keep this
 * file under the 2000-line limit; it shares workspace_rpc_args + the jo_ok/
 * send_and_free helpers defined here. */

int handle_workspace_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   config_t cfg;
   config_load(&cfg);

   /* Listing workspaces is a read of the registry; the kb call only enriches
    * each entry with its already-indexed project names. Probe liveness first so
    * a down kb degrades to empty project lists instead of blocking this RPC on a
    * 15s aimee-kb autostart (which exceeds the client's default timeout). */
   project_info_t all_projects[256];
   int pcount = kb_client_is_live() ? kb_client_index_list(all_projects, 256) : 0;
   if (pcount < 0)
      pcount = 0;

   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "workspaces");
   for (int w = 0; w < cfg.workspace_count; w++)
   {
      cJSON *ws_obj = cJSON_CreateObject();
      jo_add_str(ws_obj, "path", cfg.workspaces[w]);
      jo_add_str(ws_obj, "provider",
                 cfg.workspace_providers[w][0] ? cfg.workspace_providers[w] : "shared");
      if (cfg.workspace_vcs_remote[w][0])
         jo_add_str(ws_obj, "remote", cfg.workspace_vcs_remote[w]);
      if (cfg.workspace_vcs_head[w][0])
         jo_add_str(ws_obj, "head", cfg.workspace_vcs_head[w]);
      cJSON *projs = cJSON_AddArrayToObject(ws_obj, "projects");
      size_t ws_len = strlen(cfg.workspaces[w]);
      for (int p = 0; p < pcount; p++)
         if (strncmp(all_projects[p].root, cfg.workspaces[w], ws_len) == 0 &&
             (all_projects[p].root[ws_len] == '/' || all_projects[p].root[ws_len] == '\0'))
            cJSON_AddItemToArray(projs, cJSON_CreateString(all_projects[p].name));
      cJSON_AddItemToArray(arr, ws_obj);
   }
   return send_and_free(conn, resp);
}

/* Capture the trimmed first line of a `git -C <root> ...` command through the
 * shared provider's exec primitive (empty when the command fails / root is not
 * a repo). The provider seam means a detached server runs this on its mirror. */
void ws_git_line(const char *const argv[], char *out, size_t outsz)
{
   out[0] = '\0';
   const workspace_provider_t *ws = workspace_provider_shared();
   char *cap = NULL;
   if (ws->exec(ws, argv, &cap, 4096) == 0 && cap)
   {
      size_t n = strcspn(cap, "\r\n");
      if (n >= outsz)
         n = outsz - 1;
      memcpy(out, cap, n);
      out[n] = '\0';
   }
   free(cap);
}

/* workspace.get + the detached runner reverse-channel handlers live in a
 * sibling .inc (kept out of this file's line budget); included here so they
 * share workspace_rpc_args, ws_git_line, and the jo_ok/send_and_free helpers
 * defined above. */

int handle_workspace_remove(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char *argv[8];
   int argc = workspace_rpc_args(req, argv, 8);
   if (argc < 1 || !argv[0][0])
      return server_send_error(conn, "usage: workspace remove <path>", NULL);

   char abs[MAX_PATH_LEN];
   const char *target = realpath(argv[0], abs) ? abs : argv[0];

   config_t cfg;
   config_load(&cfg);
   int found = -1;
   for (int i = 0; i < cfg.workspace_count; i++)
      if (strcmp(cfg.workspaces[i], target) == 0)
      {
         found = i;
         break;
      }
   if (found < 0)
      return server_send_error(conn, "workspace: not registered", NULL);

   /* Compact every parallel registry array in lockstep so the provider + VCS
    * metadata stay aligned with their path (previously only workspaces[] was
    * shifted, which misaligned workspace_providers[] for entries after the
    * removed one). */
   for (int i = found; i < cfg.workspace_count - 1; i++)
   {
      snprintf(cfg.workspaces[i], MAX_PATH_LEN, "%s", cfg.workspaces[i + 1]);
      snprintf(cfg.workspace_providers[i], sizeof(cfg.workspace_providers[i]), "%s",
               cfg.workspace_providers[i + 1]);
      snprintf(cfg.workspace_vcs_remote[i], sizeof(cfg.workspace_vcs_remote[i]), "%s",
               cfg.workspace_vcs_remote[i + 1]);
      snprintf(cfg.workspace_vcs_head[i], sizeof(cfg.workspace_vcs_head[i]), "%s",
               cfg.workspace_vcs_head[i + 1]);
   }
   cfg.workspace_count--;
   if (config_save(&cfg) != 0)
      return server_send_error(conn, "workspace: failed to save config", NULL);

   /* Closing a workspace revokes any brokered forge token for it (zeroed in
    * memory) — the "revoked on session close" half of the forge-credential
    * contract (workspace-resource-plane §4). No-op when none was installed. */
   forge_cred_revoke(target);

   cJSON *resp = jo_ok();
   jo_add_str(resp, "removed", target);
   return send_and_free(conn, resp);
}

/* --- Dogfood handlers --- */

int handle_dogfood_tag(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *record_id = jo_str(req, "record_id", NULL);
   if (!record_id || !record_id[0])
      return server_send_error(conn, "dogfood tag: missing record_id", NULL);

   dogfood_label_t label = {0};
   label.outcome = jo_str(req, "outcome", NULL);
   label.notes = jo_str(req, "notes", NULL);
   int richness = jo_int(req, "richness", 0);
   if (richness >= 1 && richness <= 5)
      label.context_richness = richness;
   cJSON *surprise_j = cJSON_GetObjectItemCaseSensitive(req, "surprise");
   if (cJSON_IsBool(surprise_j))
   {
      label.has_surprise = 1;
      label.surprise = cJSON_IsTrue(surprise_j) ? 1 : 0;
   }

   int rc = dogfood_label_record(NULL, record_id, &label);
   if (rc != 0)
      return server_send_error(conn, "dogfood tag: failed to write label", NULL);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "record_id", record_id);
   return send_and_free(conn, resp);
}

static cJSON *dogfood_server_detach_prospectives(char *envelope)
{
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);
   if (!resp)
      return NULL;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "prospectives");
   cJSON *out = NULL;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0 && cJSON_IsArray(arr))
      out = cJSON_DetachItemViaPointer(resp, arr);
   cJSON_Delete(resp);
   return out;
}

static int dogfood_server_complete_review_reminders(void)
{
   cJSON *armed =
       dogfood_server_detach_prospectives(kb_client_memory_prospective_list_json("armed", 64));
   if (!armed)
      return 0;

   int closed = 0;
   cJSON *r = NULL;
   cJSON_ArrayForEach(r, armed)
   {
      cJSON *trig = cJSON_GetObjectItemCaseSensitive(r, "trigger_text");
      cJSON *id_j = cJSON_GetObjectItemCaseSensitive(r, "id");
      if (!cJSON_IsString(trig) || !cJSON_IsNumber(id_j))
         continue;
      if (strncmp(trig->valuestring, "dogfood-review-", 15) != 0)
         continue;

      char *env = kb_client_memory_prospective_complete_json((int64_t)id_j->valuedouble);
      if (!env)
         continue;
      cJSON *resp = cJSON_Parse(env);
      free(env);
      if (resp)
      {
         cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
         if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
            closed++;
         cJSON_Delete(resp);
      }
   }
   cJSON_Delete(armed);
   return closed;
}

static void dogfood_server_review_deadline(const char *month, char *out, size_t cap)
{
   struct tm tm_buf;
   memset(&tm_buf, 0, sizeof(tm_buf));
   int y = 0;
   int m = 0;
   if (sscanf(month ? month : "", "%d-%d", &y, &m) != 2 || y < 1970 || m < 1 || m > 12)
   {
      if (cap > 0)
         out[0] = '\0';
      return;
   }

   tm_buf.tm_year = y - 1900;
   tm_buf.tm_mon = m;
   tm_buf.tm_mday = 15;
   time_t t = timegm(&tm_buf);
   struct tm out_tm;
   gmtime_r(&t, &out_tm);
   strftime(out, cap, "%Y-%m-%d %H:%M:%S", &out_tm);
}

static int dogfood_server_arm_review_reminder_if_needed(const char *month, int unlabelled)
{
   if (!month || !month[0])
      return 0;

   char trigger[96];
   snprintf(trigger, sizeof(trigger), "dogfood-review-%s", month);

   cJSON *armed =
       dogfood_server_detach_prospectives(kb_client_memory_prospective_list_json("armed", 64));
   if (armed)
   {
      cJSON *r = NULL;
      cJSON_ArrayForEach(r, armed)
      {
         cJSON *trig = cJSON_GetObjectItemCaseSensitive(r, "trigger_text");
         if (cJSON_IsString(trig) && strcmp(trig->valuestring, trigger) == 0)
         {
            cJSON_Delete(armed);
            return 0;
         }
      }
      cJSON_Delete(armed);
   }

   char action[256];
   snprintf(action, sizeof(action), "review %d record(s) for %s: aimee dogfood review --month %s",
            unlabelled, month, month);
   char valid_until[32];
   dogfood_server_review_deadline(month, valid_until, sizeof(valid_until));

   char *envelope =
       kb_client_memory_prospective_create_json(trigger, action, "", "", "", valid_until);
   if (!envelope)
      return 0;
   cJSON *resp = cJSON_Parse(envelope);
   free(envelope);
   int armed_ok = 0;
   if (resp)
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      armed_ok = cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0;
      cJSON_Delete(resp);
   }
   return armed_ok ? 1 : 0;
}

int handle_dogfood_review(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *month = jo_str(req, "month", NULL);
   const char *dir = jo_str(req, "dir", NULL);
   cJSON *report = dogfood_build_report_for_month(dir, month);
   if (!report)
      return server_send_error(conn, "dogfood review: could not read log or build report", NULL);

   int total = jo_int(report, "records_total", 0);
   int labelled = jo_int(report, "records_labelled", 0);
   int unlabelled = total > labelled ? total - labelled : 0;
   int closed = dogfood_server_complete_review_reminders();

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "records_total", total);
   cJSON_AddNumberToObject(resp, "records_labelled", labelled);
   cJSON_AddNumberToObject(resp, "records_unlabelled", unlabelled);
   cJSON_AddNumberToObject(resp, "reminders_completed", closed);
   cJSON_AddStringToObject(resp, "month", jo_str(report, "month", month ? month : ""));
   cJSON_Delete(report);
   return send_and_free(conn, resp);
}

int handle_dogfood_report(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *month = jo_str(req, "month", NULL);
   const char *dir = jo_str(req, "dir", NULL);
   cJSON *report = dogfood_build_report_for_month(dir, month);
   if (!report)
      return server_send_error(conn, "dogfood report: could not read log or build report", NULL);
   int total = jo_int(report, "records_total", 0);
   int labelled = jo_int(report, "records_labelled", 0);
   int unlabelled = total > labelled ? total - labelled : 0;
   int armed =
       dogfood_server_arm_review_reminder_if_needed(jo_str(report, "month", month), unlabelled);
   cJSON_AddBoolToObject(report, "review_reminder_armed", armed ? 1 : 0);
   return send_and_free(conn, report);
}

/* --- Identity handlers --- */

int handle_identity_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   config_t cfg;
   if (config_load(&cfg) != 0)
      return server_send_error(conn, "identity show: could not load config", NULL);
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddItemToObject(resp, "charter", identity_charter_json(&cfg));
   cJSON_AddItemToObject(resp, "local_operator", identity_local_operator_json());
   cJSON_AddItemToObject(resp, "working_profile", identity_working_profile_json());
   return send_and_free(conn, resp);
}

static cJSON *load_snapshot_json(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (n <= 0 || n > 4 * 1024 * 1024)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = (char *)malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)n, fp);
   fclose(fp);
   buf[rd] = '\0';
   cJSON *j = cJSON_Parse(buf);
   free(buf);
   return j;
}

int handle_identity_snapshot(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *out_dir_rel = jo_str(req, "out", "benchmarks/identity");
   const char *cwd = jo_str(req, "cwd", NULL);

   char out_dir[1024];
   if (cwd && cwd[0] && out_dir_rel[0] != '/')
      snprintf(out_dir, sizeof(out_dir), "%s/%s", cwd, out_dir_rel);
   else
      snprintf(out_dir, sizeof(out_dir), "%s", out_dir_rel);

   if (platform_mkdir_p(out_dir, 0755) != 0)
      return server_send_error(conn, "identity snapshot: could not create output directory", NULL);

   config_t db1_cfg;
   config_load(&db1_cfg);
   if (db1_init(db1_cfg.db1_path) != 0)
      return server_send_error(conn, "identity snapshot: could not initialize DB1", NULL);

   cJSON *snap = identity_snapshot_build();
   if (!snap)
      return server_send_error(conn, "identity snapshot: could not build snapshot", NULL);

   char stamp[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(stamp, sizeof(stamp), "%Y-%m-%d", &tm_buf);

   char path[1536];
   snprintf(path, sizeof(path), "%s/%s.json", out_dir, stamp);
   struct stat st;
   if (stat(path, &st) == 0)
   {
      char stamp_hour[32];
      strftime(stamp_hour, sizeof(stamp_hour), "%Y-%m-%dT%H%M", &tm_buf);
      snprintf(path, sizeof(path), "%s/%s.json", out_dir, stamp_hour);
   }

   char *rendered = cJSON_Print(snap);
   cJSON_Delete(snap);
   if (!rendered)
      return server_send_error(conn, "identity snapshot: could not render JSON", NULL);

   FILE *fp = fopen(path, "w");
   if (!fp)
   {
      free(rendered);
      return server_send_error(conn, "identity snapshot: could not open output file", NULL);
   }
   fputs(rendered, fp);
   fputc('\n', fp);
   fclose(fp);
   free(rendered);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "path", path);
   return send_and_free(conn, resp);
}

int handle_identity_diff(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *path_a = jo_str(req, "a", NULL);
   const char *path_b = jo_str(req, "b", NULL);
   const char *cwd = jo_str(req, "cwd", NULL);

   if (!path_a || !path_a[0] || !path_b || !path_b[0])
      return server_send_error(conn, "identity diff: missing a or b path", NULL);

   char abs_a[1536], abs_b[1536];
   if (cwd && cwd[0] && path_a[0] != '/')
      snprintf(abs_a, sizeof(abs_a), "%s/%s", cwd, path_a);
   else
      snprintf(abs_a, sizeof(abs_a), "%s", path_a);
   if (cwd && cwd[0] && path_b[0] != '/')
      snprintf(abs_b, sizeof(abs_b), "%s/%s", cwd, path_b);
   else
      snprintf(abs_b, sizeof(abs_b), "%s", path_b);

   double flip_threshold = 0.3;
   cJSON *ft = cJSON_GetObjectItemCaseSensitive(req, "flip_threshold");
   if (cJSON_IsNumber(ft))
      flip_threshold = ft->valuedouble;

   cJSON *snap_a = load_snapshot_json(abs_a);
   cJSON *snap_b = load_snapshot_json(abs_b);
   if (!snap_a || !snap_b)
   {
      cJSON_Delete(snap_a);
      cJSON_Delete(snap_b);
      return server_send_error(conn, "identity diff: could not load snapshot file(s)", NULL);
   }

   cJSON *report = identity_diff_report(snap_a, snap_b, flip_threshold);
   cJSON_Delete(snap_a);
   cJSON_Delete(snap_b);
   if (!report)
      return server_send_error(conn, "identity diff: could not build diff report", NULL);
   return send_and_free(conn, report);
}

/* --- Extended dashboard handlers (traces, plans, logs, plugins, onboard, memory-stats) --- */

static cJSON *parse_or_array(char *json)
{
   cJSON *v = json ? cJSON_Parse(json) : NULL;
   return v ? v : cJSON_CreateArray();
}

static cJSON *parse_or_object(char *json)
{
   cJSON *v = json ? cJSON_Parse(json) : NULL;
   return v ? v : cJSON_CreateObject();
}

/* Recent governance decision records (decision_log via KB client), newest first. */
static char *dashboard_decisions_json(void)
{
   db2_decision_log_row_t rows[50];
   int n = kb_client_decision_log_list(NULL, 50, rows, 50);
   if (n < 0)
      n = 0;
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, decision_to_json(&rows[i]));
   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json ? json : strdup("[]");
}

/* Parse an unsigned-long query param ("k=v&…") from the current request's query
 * string; returns `dflt` when absent or unparseable. Used for audit pagination —
 * the /v1 GET route carries ?limit=&offset=, captured via the identity query. */
static long dashboard_query_long(const char *key, long dflt)
{
   const char *q = server_http_identity_query();
   size_t klen = strlen(key);
   for (const char *p = q; p && *p;)
   {
      const char *amp = strchr(p, '&');
      if (strncmp(p, key, klen) == 0 && p[klen] == '=')
      {
         char *end = NULL;
         long v = strtol(p + klen + 1, &end, 10);
         if (end != p + klen + 1)
            return v;
         return dflt;
      }
      if (!amp)
         break;
      p = amp + 1;
   }
   return dflt;
}

/* A page of the tool-action audit, most-recent first: `limit` rows starting
 * `offset` rows back from newest. Sets *total to the full ledger row count so the
 * client can paginate. Bounded so the serialized page always fits SHTTP_RESP_MAX. */
static cJSON *dashboard_audit_page(int offset, int limit, int *total)
{
   cJSON *all = audit_ledger_read(NULL, NULL);
   cJSON *out = cJSON_CreateArray();
   if (!all)
   {
      *total = 0;
      return out;
   }
   int n = cJSON_GetArraySize(all);
   *total = n;
   for (int i = n - 1 - offset; i >= 0 && cJSON_GetArraySize(out) < limit; i--)
      cJSON_AddItemToArray(out, cJSON_DetachItemFromArray(all, i));
   cJSON_Delete(all);
   return out;
}

/* One ledger read → the guardrail verdict-mix SUMMARY (counts over EVERY action,
 * so the Dashboard pane is no longer capped at a sample). Returns
 * {total,allow,block,rewrite,approval_required,other}. */
static cJSON *dashboard_audit_summary(void)
{
   cJSON *o = cJSON_CreateObject();
   int total = 0, allow = 0, block = 0, rewrite = 0, approval = 0, other = 0;
   cJSON *all = audit_ledger_read(NULL, NULL);
   if (all)
   {
      cJSON *r = NULL;
      cJSON_ArrayForEach(r, all)
      {
         cJSON *v = cJSON_GetObjectItemCaseSensitive(r, "verdict");
         const char *s = (v && cJSON_IsString(v)) ? v->valuestring : "";
         total++;
         if (strcmp(s, "allow") == 0)
            allow++;
         else if (strcmp(s, "block") == 0)
            block++;
         else if (strcmp(s, "rewrite") == 0)
            rewrite++;
         else if (strcmp(s, "approval_required") == 0)
            approval++;
         else
            other++;
      }
      cJSON_Delete(all);
   }
   cJSON_AddNumberToObject(o, "total", total);
   cJSON_AddNumberToObject(o, "allow", allow);
   cJSON_AddNumberToObject(o, "block", block);
   cJSON_AddNumberToObject(o, "rewrite", rewrite);
   cJSON_AddNumberToObject(o, "approval_required", approval);
   cJSON_AddNumberToObject(o, "other", other);
   return o;
}

static void readiness_add_step(cJSON *steps, const char *step, const char *status,
                               const char *message)
{
   cJSON *s = cJSON_CreateObject();
   cJSON_AddStringToObject(s, "step", step);
   cJSON_AddStringToObject(s, "status", status);
   if (message && message[0])
      cJSON_AddStringToObject(s, "message", message);
   cJSON_AddItemToArray(steps, s);
}

static int json_array_len(const cJSON *v)
{
   return cJSON_IsArray(v) ? cJSON_GetArraySize(v) : 0;
}

/* Real readiness report (onboard-report shape) from the already-gathered payload. */
static cJSON *dashboard_readiness(const cJSON *resp)
{
   cJSON *report = cJSON_CreateObject();
   cJSON_AddStringToObject(report, "version", "1");
   cJSON *steps = cJSON_AddArrayToObject(report, "steps");
   cJSON *next = cJSON_AddArrayToObject(report, "next_actions");
   int ready = 1;

   const cJSON *mem = cJSON_GetObjectItem(resp, "memory_stats");
   const cJSON *tiers = mem ? cJSON_GetObjectItem(mem, "tier_kinds") : NULL;
   if (json_array_len(tiers) > 0)
      readiness_add_step(steps, "database", "ok", "memory store reachable");
   else
      readiness_add_step(steps, "database", "warn", "no memories recorded yet");

   int agents = json_array_len(cJSON_GetObjectItem(resp, "agents"));
   if (agents > 0)
   {
      char msg[64];
      snprintf(msg, sizeof(msg), "%d agent%s configured", agents, agents == 1 ? "" : "s");
      readiness_add_step(steps, "agents", "ok", msg);
   }
   else
   {
      ready = 0;
      readiness_add_step(steps, "agents", "error", "no agents configured");
      cJSON_AddItemToArray(next, cJSON_CreateString("Configure at least one delegate agent"));
   }

   if (json_array_len(cJSON_GetObjectItem(resp, "metrics")) > 0)
      readiness_add_step(steps, "delegations", "ok", "delegate activity recorded");
   else
      readiness_add_step(steps, "delegations", "warn", "no delegate activity yet");

   const cJSON *lsp = cJSON_GetObjectItem(resp, "lsp");
   int lsp_err = lsp ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(lsp, "errors")) : 0;
   readiness_add_step(steps, "lsp", lsp_err > 0 ? "warn" : "ok",
                      lsp_err > 0 ? "diagnostics present" : "no code diagnostics");

   cJSON_AddBoolToObject(report, "ready", ready);
   cJSON_AddNumberToObject(report, "elapsed_ms", 0);
   return report;
}

int handle_dashboard_traces(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = api_traces();
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", parse_or_array(json));
   free(json);
   return send_and_free(conn, resp);
}

int handle_dashboard_plans(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = api_plans();
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", parse_or_array(json));
   free(json);
   return send_and_free(conn, resp);
}

int handle_dashboard_logs(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = kb_client_dashboard_logs_json();
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", parse_or_array(json));
   free(json);
   return send_and_free(conn, resp);
}

int handle_dashboard_plugins(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = api_dashboard_plugins();
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", parse_or_object(json));
   free(json);
   return send_and_free(conn, resp);
}

int handle_dashboard_onboard(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = api_dashboard_onboard();
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", parse_or_object(json));
   free(json);
   return send_and_free(conn, resp);
}

int handle_dashboard_memory_stats(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char *json = kb_client_dashboard_memory_stats_json();
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", parse_or_object(json));
   free(json);
   return send_and_free(conn, resp);
}

/* --- Aggregate dashboard handler (all panels in one connection) --- */

int handle_dashboard_all(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   cJSON *resp = jo_ok();
   char *json;

#define ADD_ARRAY(key, fn)                                                                         \
   do                                                                                              \
   {                                                                                               \
      json = (fn);                                                                                 \
      cJSON_AddItemToObject(resp, (key), parse_or_array(json));                                    \
      free(json);                                                                                  \
   } while (0)

#define ADD_OBJECT(key, fn)                                                                        \
   do                                                                                              \
   {                                                                                               \
      json = (fn);                                                                                 \
      cJSON_AddItemToObject(resp, (key), parse_or_object(json));                                   \
      free(json);                                                                                  \
   } while (0)

   ADD_ARRAY("delegations", api_delegations());
   ADD_ARRAY("metrics", api_metrics());
   ADD_ARRAY("traces", api_traces());
   ADD_ARRAY("plans", api_plans());
   ADD_ARRAY("logs", kb_client_dashboard_logs_json());
   ADD_ARRAY("agents", server_agent_list_json());
   ADD_ARRAY("token_audit", api_token_audit());
   ADD_ARRAY("decisions", dashboard_decisions_json());
   ADD_OBJECT("memory_stats", kb_client_dashboard_memory_stats_json());

#undef ADD_ARRAY
#undef ADD_OBJECT

   int errors = 0, warnings = 0, active_servers = 0;
   lsp_manager_diag_summary(&errors, &warnings, &active_servers);
   cJSON *lsp = cJSON_CreateObject();
   cJSON_AddNumberToObject(lsp, "errors", errors);
   cJSON_AddNumberToObject(lsp, "warnings", warnings);
   cJSON_AddNumberToObject(lsp, "active_servers", active_servers);
   cJSON_AddItemToObject(resp, "lsp", lsp);

   cJSON_AddItemToObject(resp, "onboard", dashboard_readiness(resp));

   /* Verdict-mix counts over the WHOLE ledger (the Guardrail Actions pane), so it
    * is no longer limited to a capped sample. The full row list is served,
    * paginated, by dashboard.audit for the Logs page. */
   cJSON_AddItemToObject(resp, "audit_summary", dashboard_audit_summary());

   return send_and_free(conn, resp);
}

/* dashboard.audit: the server's tool-action audit ledger for the Logs page,
 * PAGINATED (?limit=&offset=, most-recent first) so an arbitrarily large ledger
 * never overflows the response buffer. Returns {data:[page], total, offset, limit}. */
int handle_dashboard_audit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   int limit = (int)dashboard_query_long("limit", 500);
   if (limit <= 0 || limit > 1000)
      limit = 500; /* 1000 rows ≈ 180 KB, safely under SHTTP_RESP_MAX */
   int offset = (int)dashboard_query_long("offset", 0);
   if (offset < 0)
      offset = 0;
   int total = 0;
   cJSON *page = dashboard_audit_page(offset, limit, &total);
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "data", page);
   cJSON_AddNumberToObject(resp, "total", total);
   cJSON_AddNumberToObject(resp, "offset", offset);
   cJSON_AddNumberToObject(resp, "limit", limit);
   return send_and_free(conn, resp);
}

/* GET /v1/audit/verify: verify the WORM audit store's hash-chain + checkpoint MACs
 * and report the amber uncheckpointed-tail signal. verify ∈ {green,amber,red}. */
int handle_audit_verify(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   char err[256] = "";
   long head = 0, ckpt = 0;
   int st = audit_worm_verify(err, sizeof err, &head, &ckpt);
   const char *status =
       st == AUDIT_WORM_VERIFY_GREEN ? "green" : (st == AUDIT_WORM_VERIFY_AMBER ? "amber" : "red");
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "verify", status);
   cJSON_AddNumberToObject(resp, "head_seq", head);
   cJSON_AddNumberToObject(resp, "last_checkpoint_seq", ckpt);
   cJSON_AddNumberToObject(resp, "unattested", head > ckpt ? head - ckpt : 0);
   if (st == AUDIT_WORM_VERIFY_RED)
      cJSON_AddStringToObject(resp, "detail", err[0] ? err : "integrity break");
   return send_and_free(conn, resp);
}

/* POST /v1/audit/checkpoint: append a checkpoint committing the current chain head
 * under the chain-key MAC, bounding the unattested tail. */
int handle_audit_checkpoint(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   int rc = audit_worm_checkpoint();
   cJSON *resp = jo_ok();
   cJSON_AddBoolToObject(resp, "checkpointed", rc == 0);
   if (rc != 0)
      cJSON_AddStringToObject(resp, "error", "checkpoint failed");
   return send_and_free(conn, resp);
}

/* --- LSP diagnostics summary --- */

int handle_lsp_diagnostics_summary(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;
   int errors = 0, warnings = 0, active_servers = 0;
   lsp_manager_diag_summary(&errors, &warnings, &active_servers);
   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "errors", errors);
   cJSON_AddNumberToObject(resp, "warnings", warnings);
   cJSON_AddNumberToObject(resp, "active_servers", active_servers);
   return send_and_free(conn, resp);
}
