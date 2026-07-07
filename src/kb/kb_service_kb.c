/* kb_service_kb.c: aimee-kb dispatch handlers for the kb.* RPC family
 * (build, update, ingest, repair, clear, search, status).  Split out of
 * kb_service.c so the file stays under the per-file line cap. */

#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "kb_curator_queue.h"
#include "kb_curator_provider.h"
#include "json_fluent.h"
#include "db2/canonical_index.h"
#include "db2/kb_maintenance.h"
#include "db2/kb_payload.h"
#include "db2/kb_service_backend.h"
#include "db2/kb_service_backend_export.h"
#include "db2/kb_runtime_state.h"
#include "db2/lifecycle.h"
#include "db2/vector_index_ops.h"
#include "db2/pgvec_kb_service.h"
#include "db2/kb_vectors.h"
#include "db2/vector_status.h"
#include "kb.h"
#include "kb_bandit.h"
#include "kb_http.h"
#include "kb_service.h"
#include "kb_service_kb.h"
#include "log.h"
#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg);
extern kb_service_ctx_t *g_kb_ctx;

char *kb_service_status_json(const char *project)
{
   db2_kb_service_project_status_t stats;
   if (db2_kb_service_collect_project_status(project, &stats) != 0)
      return strdup("{\"status\":\"error\",\"summary_status\":\"unavailable\","
                    "\"owner\":\"knowledge-service\",\"available\":0,"
                    "\"message\":\"failed to query knowledge status\"}");

   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return strdup("{}");

   cJSON_AddStringToObject(obj, "status", "ok");
   cJSON_AddStringToObject(obj, "summary_status", "ok");
   cJSON_AddStringToObject(obj, "owner", "knowledge-service");
   cJSON_AddBoolToObject(obj, "available", 1);
   cJSON_AddStringToObject(obj, "project", stats.project);
   cJSON_AddNumberToObject(obj, "files", stats.files);
   cJSON_AddNumberToObject(obj, "chunks", stats.chunks);
   cJSON_AddNumberToObject(obj, "tokens", stats.tokens);
   cJSON_AddNumberToObject(obj, "embeddings", stats.embeddings);

   cJSON *queue = cJSON_AddObjectToObject(obj, "queue");
   cJSON_AddNumberToObject(queue, "pending", stats.queue.pending);
   cJSON_AddNumberToObject(queue, "running", stats.queue.running);
   cJSON_AddNumberToObject(queue, "done", stats.queue.done);
   cJSON_AddNumberToObject(queue, "failed", stats.queue.failed);
   cJSON_AddNumberToObject(queue, "total", stats.queue.total);

   cJSON *vector_status = pgvec_vector_status_json();
   if (vector_status)
   {
      cJSON_AddItemToObject(obj, "vector", vector_status);
      cJSON *vector_status_value = cJSON_GetObjectItemCaseSensitive(vector_status, "status");
      if (cJSON_IsString(vector_status_value) &&
          strcmp(vector_status_value->valuestring, "ok") != 0)
         cJSON_ReplaceItemInObject(obj, "summary_status", cJSON_CreateString("degraded"));
   }

   /* §2c: a dim-change re-embed in flight -> `maintenance`; past the TTL (re-embed
    * stuck) -> `degraded`, so a never-clearing reset is observable not silent. */
   {
      int rtarget = 0;
      long rstarted = 0;
      if (db2_reembed_in_progress_get(&rtarget, &rstarted) == 1)
      {
         const long ttl = 24 * 3600;
         int stuck = (rstarted > 0 && (long)time(NULL) - rstarted > ttl);
         cJSON_ReplaceItemInObject(obj, "summary_status",
                                   cJSON_CreateString(stuck ? "degraded" : "maintenance"));
         cJSON *re = cJSON_AddObjectToObject(obj, "reembed");
         cJSON_AddNumberToObject(re, "target_dim", rtarget);
         cJSON_AddNumberToObject(re, "started_at", (double)rstarted);
         cJSON_AddBoolToObject(re, "stuck", stuck);
      }
   }

   db2_kb_ingest_queue_stats_t iqstats;
   memset(&iqstats, 0, sizeof(iqstats));
   if (db2_kb_ingest_queue_stats(&iqstats) == 0)
   {
      cJSON *iq = cJSON_AddObjectToObject(obj, "ingest_queue");
      cJSON_AddNumberToObject(iq, "pending", iqstats.pending);
      cJSON_AddNumberToObject(iq, "running", iqstats.running);
      cJSON_AddNumberToObject(iq, "done_last_24h", iqstats.done_last_24h);
      cJSON_AddNumberToObject(iq, "failed_last_24h", iqstats.failed_last_24h);
   }

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json ? json : strdup("{}");
}

int kb_handle_ingest(int fd, cJSON *req)
{
   cJSON *ws_j = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   cJSON *force_j = cJSON_GetObjectItemCaseSensitive(req, "force");
   int force = cJSON_IsTrue(force_j) ? 1 : 0;

   if (!db2_is_initialized())
      return kb_send_error(fd, "failed to open knowledge service store");

   config_t cfg;
   config_load(&cfg);

   int use_all =
       !cJSON_IsString(ws_j) || !ws_j->valuestring[0] || strcmp(ws_j->valuestring, "all") == 0;

   char(*projects)[MAX_PATH_LEN] = calloc(MAX_DISCOVERED_PROJECTS, MAX_PATH_LEN);
   if (!projects)
      return kb_send_error(fd, "out of memory");

   int total_queued = 0;

   if (!use_all)
   {
      int n = workspace_discover_projects(ws_j->valuestring, 3, projects, MAX_DISCOVERED_PROJECTS);
      for (int i = 0; i < n; i++)
      {
         char pname[256];
         char pws[256];
         workspace_repo_index_keys(projects[i], ws_j->valuestring, pname, sizeof(pname), pws,
                                   sizeof(pws));
         if (force)
         {
            db2_kb_service_clear_project(pname);
            pgvec_kb_vector_delete_project(pname);
            db2_kb_file_index_delete_project(pname);
         }
         db2_kb_ingest_queue_enqueue(pname, projects[i], pws, force);
         total_queued++;
      }
   }
   else
   {
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         int n =
             workspace_discover_projects(cfg.workspaces[w], 3, projects, MAX_DISCOVERED_PROJECTS);
         for (int i = 0; i < n; i++)
         {
            char pname[256];
            char pws[256];
            workspace_repo_index_keys(projects[i], cfg.workspaces[w], pname, sizeof(pname), pws,
                                      sizeof(pws));
            if (force)
            {
               db2_kb_service_clear_project(pname);
               pgvec_kb_vector_delete_project(pname);
               db2_kb_file_index_delete_project(pname);
            }
            db2_kb_ingest_queue_enqueue(pname, projects[i], pws, force);
            total_queued++;
         }
      }
   }

   free(projects);

   if (g_kb_ctx)
      kb_worker_notify(g_kb_ctx);

   char msg[256];
   if (total_queued == 0)
      snprintf(msg, sizeof(msg), "No projects found to ingest.");
   else
      snprintf(msg, sizeof(msg),
               "%s queued for %d project(s). Run `aimee kb ingest status` to monitor.",
               force ? "Force re-index" : "Incremental ingest", total_queued);

   cJSON *resp = jo_ok();
   cJSON_AddNumberToObject(resp, "projects_queued", total_queued);
   cJSON_AddStringToObject(resp, "message", msg);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

char *kb_service_ingest_status_json(void)
{
   if (!db2_is_initialized())
      return NULL;

   db2_kb_ingest_queue_stats_t qs;
   if (db2_kb_ingest_queue_stats(&qs) != 0)
      return NULL;

#define INGEST_RECENT_MAX 200
   db2_kb_ingest_recent_t recent[INGEST_RECENT_MAX];
   int n_recent = db2_kb_ingest_queue_recent(recent, INGEST_RECENT_MAX);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");

   cJSON *queue = cJSON_AddObjectToObject(resp, "queue");
   cJSON_AddNumberToObject(queue, "pending", qs.pending);
   cJSON_AddNumberToObject(queue, "running", qs.running);
   cJSON_AddNumberToObject(queue, "done_last_24h", qs.done_last_24h);
   cJSON_AddNumberToObject(queue, "failed_last_24h", qs.failed_last_24h);

   cJSON *workers_obj = cJSON_AddObjectToObject(resp, "workers");
   int configured = g_kb_ctx ? g_kb_ctx->ingest_count : 0;
   cJSON_AddNumberToObject(workers_obj, "configured", configured);
   cJSON_AddNumberToObject(workers_obj, "active", qs.running);

   cJSON *arr = cJSON_AddArrayToObject(resp, "recent");
   for (int i = 0; i < n_recent; i++)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "project", recent[i].project);
      cJSON_AddStringToObject(r, "status", recent[i].status);
      cJSON_AddStringToObject(r, "completed_at", recent[i].completed_at);
      cJSON_AddNumberToObject(r, "files_indexed", recent[i].files_indexed);
      cJSON_AddNumberToObject(r, "chunks_added", recent[i].chunks_added);
      if (recent[i].error_message[0])
         cJSON_AddStringToObject(r, "error", recent[i].error_message);
      cJSON_AddItemToArray(arr, r);
   }
#undef INGEST_RECENT_MAX

   char *json = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   return json;
}

/* Add a per-tier provider sub-object {configured, base_url, model} (no api_key). */
static void kb_health_add_curator_tier(cJSON *curator, const char *key, const config_t *cfg,
                                       kb_curator_stage_t stage)
{
   cJSON *t = cJSON_AddObjectToObject(curator, key);
   if (!t)
      return;
   provider_def_t def;
   int configured = kb_curator_provider_for_stage(cfg, stage, &def);
   cJSON_AddBoolToObject(t, "configured", configured);
   cJSON_AddStringToObject(t, "base_url", configured && def.base_url ? def.base_url : "");
   cJSON_AddStringToObject(t, "model", configured && def.model ? def.model : "");
}

/* Curator observability block for /v1/health (§4): which tiers have a provider
 * (Tier-A extract/index, Tier-B reason/judge) and the curator queue depth. */
static void kb_health_add_curator(cJSON *resp, const config_t *cfg)
{
   cJSON *curator = cJSON_AddObjectToObject(resp, "curator");
   if (!curator)
      return;
   kb_health_add_curator_tier(curator, "tier_a", cfg, KB_CURATOR_STAGE_EXTRACT_DOCS);
   kb_health_add_curator_tier(curator, "tier_b", cfg, KB_CURATOR_STAGE_JUDGE);

   kb_curator_queue_counts_t qc;
   kb_curator_queue_counts(&qc);
   cJSON *q = cJSON_AddObjectToObject(curator, "queue");
   if (q)
   {
      cJSON_AddNumberToObject(q, "extract_pending", qc.extract_pending);
      cJSON_AddNumberToObject(q, "extract_done", qc.extract_done);
      cJSON_AddNumberToObject(q, "code_unit_pending", qc.code_unit_pending);
      cJSON_AddNumberToObject(q, "code_unit_done", qc.code_unit_done);
   }
}

static cJSON *kb_service_health_object(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   cJSON_AddStringToObject(resp, "status", "ok");

   /* DB2: generic schema + KB-specific tables */
   int schema_ok = 0, have_pg_trgm = 0, kb_tables_ok = 0;
   int db2_ok = (db2_health_probe(&schema_ok, &have_pg_trgm) == 0 && schema_ok);
   (void)db2_kb_health_probe(&kb_tables_ok);
   cJSON_AddBoolToObject(resp, "db2_ok", db2_ok);
   cJSON_AddBoolToObject(resp, "db2_kb_tables_ok", kb_tables_ok);

   /* pgvector: delegate to the existing vector-status helper */
   int pgvec_ok = 0, pgvec_collection_ok = 0;
   int pgvec_vectors = 0, pgvec_indexed = 0;
   cJSON *vs = pgvec_vector_status_json();
   if (vs)
   {
      cJSON *s = cJSON_GetObjectItemCaseSensitive(vs, "status");
      pgvec_ok = cJSON_IsString(s) && strcmp(s->valuestring, "ok") == 0;
      /* pgvec_vector_status_json reports the KB vector table via the top-level
       * "kb_collection_ready" / "kb_points" fields — there is no nested
       * "collection" object. Reading the wrong shape left pgvec_collection_ok
       * permanently false, so /v1/health always warned "KB vector table
       * missing" even when the table was present and indexed. */
      cJSON *kb_ready = cJSON_GetObjectItemCaseSensitive(vs, "kb_collection_ready");
      pgvec_collection_ok = cJSON_IsTrue(kb_ready);
      cJSON *kb_pts = cJSON_GetObjectItemCaseSensitive(vs, "kb_points");
      if (cJSON_IsNumber(kb_pts))
         pgvec_vectors = (int)kb_pts->valuedouble;
      /* pgvector keeps every row in the HNSW index, so indexed == total. */
      pgvec_indexed = pgvec_collection_ok ? pgvec_vectors : 0;
      cJSON_Delete(vs);
   }
   cJSON_AddBoolToObject(resp, "pgvec_ok", pgvec_ok);
   cJSON_AddBoolToObject(resp, "pgvec_collection_ok", pgvec_collection_ok);
   cJSON_AddNumberToObject(resp, "pgvec_vectors", pgvec_vectors);
   cJSON_AddNumberToObject(resp, "pgvec_indexed_vectors", pgvec_indexed);

   /* Embed: report whether an embedder is configured. The command can come from
    * the config file OR the AIMEE_EMBEDDER_URL env (the combined image always
    * exports the latter), so resolve it the same way embed_command does instead
    * of reading the raw config field — otherwise an env-configured embedder is
    * wrongly reported embed_ok:false while embed_command shows a real URL. The
    * "builtin" fallback (nothing configured) still reports false, as before. */
   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = config_embedding_command(&cfg, NULL);
   int embed_ok = (embed_cmd[0] && strcmp(embed_cmd, "builtin") != 0) ? 1 : 0;
   cJSON_AddBoolToObject(resp, "embed_ok", embed_ok);
   cJSON_AddStringToObject(resp, "embed_command", embed_cmd);

   /* Curator (§4 observability): per-tier provider config + queue depth. The
    * live four-state reachability probe (ready/loading/gated/down) is deferred to
    * a follow-up — it needs a bounded async probe so the health path never blocks,
    * and the gated state needs a custom curator /health (the bundled official
    * llama.cpp doesn't expose it). api_key is never surfaced. */
   kb_health_add_curator(resp, &cfg);

   /* Freshness: read last_ingest_at from kb_runtime_state */
   char last_ingest_at[64] = "";
   int freshness_days = -1;
   if (db2_kb_runtime_state_get("last_ingest_at", last_ingest_at, sizeof(last_ingest_at)) == 0 &&
       last_ingest_at[0])
   {
      /* Parse ISO8601 prefix YYYY-MM-DD HH:MM:SS to compute days elapsed */
      struct tm t;
      memset(&t, 0, sizeof(t));
      if (sscanf(last_ingest_at, "%d-%d-%d %d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour,
                 &t.tm_min, &t.tm_sec) >= 3)
      {
         t.tm_year -= 1900;
         t.tm_mon -= 1;
         time_t then = mktime(&t);
         time_t now = time(NULL);
         if (then > 0 && now > then)
            freshness_days = (int)((now - then) / 86400);
         else
            freshness_days = 0;
      }
   }
   cJSON_AddStringToObject(resp, "last_ingest_at", last_ingest_at);
   cJSON_AddNumberToObject(resp, "freshness_days", freshness_days);

   /* Stats: aggregate chunk/embedding counts */
   db2_kb_service_project_status_t stats;
   memset(&stats, 0, sizeof(stats));
   db2_kb_service_collect_project_status(NULL, &stats);
   cJSON_AddNumberToObject(resp, "chunk_count", stats.chunks);
   cJSON_AddNumberToObject(resp, "embedding_count", stats.embeddings);

   /* Warning accumulation */
   cJSON *warnings = cJSON_AddArrayToObject(resp, "warnings");
   if (!db2_ok)
      cJSON_AddItemToArray(warnings, cJSON_CreateString("DB2 schema not ready"));
   if (!pgvec_ok)
      cJSON_AddItemToArray(warnings, cJSON_CreateString("pgvector extension not loaded in DB2"));
   if (!pgvec_collection_ok)
      cJSON_AddItemToArray(warnings, cJSON_CreateString("KB vector table missing"));
   if (freshness_days > 30)
      cJSON_AddItemToArray(warnings, cJSON_CreateString("KB not ingested in over 30 days"));
   else if (freshness_days > 7)
      cJSON_AddItemToArray(warnings, cJSON_CreateString("KB not ingested in over 7 days"));
   if (stats.chunks > 0 && stats.embeddings < stats.chunks * 9 / 10)
      cJSON_AddItemToArray(warnings,
                           cJSON_CreateString("KB has significant unembedded chunks (>10%)"));

   /* Maintenance stats */
   char last_maintenance_at[64] = "";
   db2_kb_runtime_state_get("last_maintenance_at", last_maintenance_at,
                            sizeof(last_maintenance_at));
   cJSON_AddStringToObject(resp, "last_maintenance_at", last_maintenance_at);

   char maint_decayed_buf[32] = "0";
   db2_kb_runtime_state_get("last_maintenance_decayed", maint_decayed_buf,
                            sizeof(maint_decayed_buf));
   cJSON_AddNumberToObject(resp, "last_maintenance_rows_decayed", atoi(maint_decayed_buf));

   char maint_pruned_buf[32] = "0";
   db2_kb_runtime_state_get("last_maintenance_pruned", maint_pruned_buf, sizeof(maint_pruned_buf));
   cJSON_AddNumberToObject(resp, "last_maintenance_orphans_pruned", atoi(maint_pruned_buf));

   cJSON_AddBoolToObject(resp, "maintenance_enabled", cfg.kb_maintenance_enabled);

   /* Typed-facts capability (proposal §8): the KB advertises its own typed-facts
    * state so aimee-server can gate per-turn fact injection on it WITHOUT owning
    * the config. The server never reads typed_facts_enabled itself. */
   cJSON_AddBoolToObject(resp, "typed_facts_enabled", cfg.typed_facts_enabled ? 1 : 0);

   return resp;
}

char *kb_service_health_json(void)
{
   cJSON *resp = kb_service_health_object();
   if (!resp)
      return strdup("{}");
   char *json = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   return json ? json : strdup("{}");
}

int kb_handle_file_get(int fd, cJSON *req)
{
   const char *project = jo_str(req, "project", NULL);
   const char *path = jo_str(req, "path", NULL);
   if (!project || !project[0] || !path || !path[0])
      return kb_send_error(fd, "missing project or path");

   char *content = db2_kb_file_index_get_content(project, path);

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddStringToObject(resp, "path", path);
   if (content)
   {
      cJSON_AddStringToObject(resp, "content", content);
      free(content);
   }
   else
   {
      cJSON_AddNullToObject(resp, "content");
   }
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

int kb_handle_maintenance_run(int fd, cJSON *req)
{
   config_t cfg;
   config_load(&cfg);

   int dry_run = 0;
   int force = 0;
   if (req)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
      if (params)
      {
         dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "dry_run")) ? 1 : 0;
         force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "force")) ? 1 : 0;
      }
   }

   if (!cfg.kb_maintenance_enabled && !force)
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "disabled");
      cJSON_AddBoolToObject(resp, "maintenance_enabled", 0);
      cJSON_AddNumberToObject(resp, "rows_decayed", 0);
      cJSON_AddNumberToObject(resp, "orphans_pruned", 0);
      cJSON_AddNumberToObject(resp, "elapsed_ms", 0);
      int srv_rc = kb_send_response(fd, resp);
      cJSON_Delete(resp);
      return srv_rc;
   }

   kb_maintenance_config_t mcfg;
   kb_maintenance_config_defaults(&mcfg);
   mcfg.lambda = cfg.kb_maintenance_lambda;
   mcfg.confidence_floor = cfg.kb_maintenance_floor;
   mcfg.min_age_days = cfg.kb_maintenance_min_age_days;
   mcfg.orphan_prune_days = cfg.kb_maintenance_orphan_days;
   mcfg.dry_run = dry_run;

   kb_maintenance_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = kb_maintenance_run(&mcfg, &result);

   cJSON *resp = cJSON_CreateObject();
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddNumberToObject(resp, "rows_decayed", result.rows_decayed);
      cJSON_AddNumberToObject(resp, "orphans_pruned", result.orphans_pruned);
      cJSON_AddNumberToObject(resp, "elapsed_ms", (double)result.elapsed_ms);
      cJSON_AddStringToObject(resp, "run_id", result.run_id);
      if (!mcfg.dry_run)
      {
         /* Persist last live run time so health can report it. */
         db2_kb_runtime_state_set_now("last_maintenance_at");
         char countbuf[32];
         snprintf(countbuf, sizeof(countbuf), "%d", result.rows_decayed);
         db2_kb_runtime_state_set("last_maintenance_decayed", countbuf);
         snprintf(countbuf, sizeof(countbuf), "%d", result.orphans_pruned);
         db2_kb_runtime_state_set("last_maintenance_pruned", countbuf);
      }
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "error", result.error[0] ? result.error : "maintenance failed");
   }

   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

/* ------------------------------------------------------------------ */
/* kb.export                                                            */
/* ------------------------------------------------------------------ */

int kb_handle_kb_export(int fd, cJSON *req)
{
   cJSON *workspace_j = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(req, "kind");
   cJSON *since_j = cJSON_GetObjectItemCaseSensitive(req, "since");
   cJSON *archived_j = cJSON_GetObjectItemCaseSensitive(req, "include_archived");

   const char *workspace = (cJSON_IsString(workspace_j) && workspace_j->valuestring[0])
                               ? workspace_j->valuestring
                               : NULL;
   const char *kind =
       (cJSON_IsString(kind_j) && kind_j->valuestring[0]) ? kind_j->valuestring : NULL;
   const char *since =
       (cJSON_IsString(since_j) && since_j->valuestring[0]) ? since_j->valuestring : NULL;
   int include_archived = cJSON_IsTrue(archived_j) ? 1 : 0;

   cJSON *resp =
       db2_kb_service_memory_export_filtered_json(workspace, kind, since, include_archived);
   if (!resp)
      return kb_send_error(fd, "kb.export: export failed");

   int srv_rc2 = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc2;
}

/* ------------------------------------------------------------------ */
/* kb.import                                                            */
/* ------------------------------------------------------------------ */

int kb_handle_kb_import(int fd, cJSON *req)
{
   cJSON *memories_j = cJSON_GetObjectItemCaseSensitive(req, "memories");
   cJSON *workspace_j = cJSON_GetObjectItemCaseSensitive(req, "workspace");
   cJSON *dry_run_j = cJSON_GetObjectItemCaseSensitive(req, "dry_run");

   if (!cJSON_IsArray(memories_j))
      return kb_send_error(fd, "kb.import: 'memories' array is required");

   const char *workspace_override = (cJSON_IsString(workspace_j) && workspace_j->valuestring[0])
                                        ? workspace_j->valuestring
                                        : NULL;
   int dry_run = cJSON_IsTrue(dry_run_j) ? 1 : 0;

   int imported = 0;
   int rc = db2_kb_service_memory_import_json(memories_j, workspace_override, dry_run, &imported);

   cJSON *resp = cJSON_CreateObject();
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddNumberToObject(resp, "imported", (double)imported);
      cJSON_AddBoolToObject(resp, "dry_run", dry_run);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "import failed");
   }

   int srv_rc3 = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc3;
}
