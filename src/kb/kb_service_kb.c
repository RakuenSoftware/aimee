/* kb_service_kb.c: aimee-kb dispatch handlers for the kb.* RPC family
 * (build, update, ingest, repair, clear, search, status).  Split out of
 * kb_service.c so the file stays under the per-file line cap. */

#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "config_database.h" /* config_synth_chat_endpoint_current */
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
#include <aimee/workspace/workspace.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Defined in kb_service.c; non-static so this file can call them. */
int kb_send_response(int fd, cJSON *resp);
int kb_send_error(int fd, const char *message);
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg);
extern kb_service_ctx_t *g_kb_ctx;

/* Append the typed-fact backlog warning to `warnings`, if it applies.
 *
 * SHARED BY BOTH STATUS SURFACES ON PURPOSE. This warning was written for
 * kb_service_health_object() and its own comment names the symptom on the OTHER
 * surface -- "`aimee kb status` printed '4 pending' with no hint that pending
 * here means forever". kb_service_status_json() is a separate builder, so the
 * command an operator actually runs never grew the hint that was written for it.
 *
 * Reproduced on a live deployment: 65 memory_facts jobs, oldest 34 hours old,
 * attempts 0, while `aimee kb status` printed "Queue: 65 pending, 0 running, 0
 * failed" under "status: ok" and said nothing about why pending never moves.
 *
 * One helper, both callers, so the two cannot drift apart again. */
static void kb_status_add_typed_facts_warning(cJSON *warnings)
{
   if (!warnings || !config_typed_facts_enabled())
      return;
   int facts_pending = db2_kb_async_count_kind_pending("memory_facts");
   char synth_endpoint[512];
   if (facts_pending <= 0 ||
       config_synth_chat_endpoint_current(synth_endpoint, sizeof(synth_endpoint)))
      return;
   char msg[320];
   snprintf(msg, sizeof(msg),
            "typed-fact extraction: %d job(s) queued with nothing to drain them - no "
            "synthesis endpoint is configured, so the curator LLM lane is not running. "
            "Memories are stored and searchable but contribute no typed facts. The "
            "backlog is not lost: configuring a synthesis provider drains it.",
            facts_pending);
   cJSON_AddItemToArray(warnings, cJSON_CreateString(msg));
}

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

   /* The pending count above is meaningless without this: an undrainable queue
    * and a busy one print the same number. */
   {
      cJSON *warnings = cJSON_CreateArray();
      kb_status_add_typed_facts_warning(warnings);
      if (cJSON_GetArraySize(warnings) > 0)
         cJSON_AddItemToObject(obj, "warnings", warnings);
      else
         cJSON_Delete(warnings);
   }

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
         if (workspace_repo_index_keys(projects[i], ws_j->valuestring, pname, sizeof(pname), pws,
                                       sizeof(pws)) != 0)
         {
            aimee_log(LOG_ERROR, "kb.ingest.identity",
                      "skipping root='%s': no durable project identity", projects[i]);
            continue;
         }
         if (force)
         {
            pgvec_kb_vector_delete_current_project(pname);
            db2_kb_service_clear_current_project(pname);
            db2_kb_file_index_delete_current_project(pname);
         }
         db2_kb_ingest_queue_enqueue(pname, projects[i], pws, force,
                                     DB2_KB_INGEST_PRIO_INTERACTIVE);
         total_queued++;
      }
   }
   else
   {
      for (int w = 0; w < config_workspace_count(); w++)
      {
         int n = workspace_discover_projects(config_workspaces(w), 3, projects,
                                             MAX_DISCOVERED_PROJECTS);
         for (int i = 0; i < n; i++)
         {
            char pname[256];
            char pws[256];
            if (workspace_repo_index_keys(projects[i], config_workspaces(w), pname, sizeof(pname),
                                          pws, sizeof(pws)) != 0)
            {
               aimee_log(LOG_ERROR, "kb.ingest.identity",
                         "skipping root='%s': no durable project identity", projects[i]);
               continue;
            }
            if (force)
            {
               pgvec_kb_vector_delete_current_project(pname);
               db2_kb_service_clear_current_project(pname);
               db2_kb_file_index_delete_current_project(pname);
            }
            db2_kb_ingest_queue_enqueue(pname, projects[i], pws, force,
                                        DB2_KB_INGEST_PRIO_INTERACTIVE);
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

/* The synthesis provider sub-object {configured, base_url, model} (no api_key). */
static void kb_health_add_curator_provider(cJSON *curator)
{
   cJSON *t = cJSON_AddObjectToObject(curator, "synthesis");
   if (!t)
      return;
   provider_def_owned_t def;
   /* Any stage resolves the same provider now, so the stage passed here is not a
    * choice — it only has to be a real one. */
   int configured = kb_curator_provider_for_stage(KB_CURATOR_STAGE_EXTRACT_DOCS, &def);
   cJSON_AddBoolToObject(t, "configured", configured);
   cJSON_AddStringToObject(t, "base_url", configured && def.def.base_url ? def.def.base_url : "");
   cJSON_AddStringToObject(t, "model", configured && def.def.model ? def.def.model : "");
}

/* Curator observability block for /v1/health (§4): the synthesis provider and the
 * curator queue depth.
 *
 * ONE provider, not two. This used to report "tier_a" and "tier_b" separately,
 * from when extract/index ran on a small model and reason/judge on a capable one.
 * There are no tiers any more — every stage resolves the same provider — so the
 * two keys had become the same values under two names, which reads as a
 * configuration an operator could change and cannot. */
static void kb_health_add_curator(cJSON *resp, kb_curator_queue_counts_t *out_counts)
{
   cJSON *curator = cJSON_AddObjectToObject(resp, "curator");
   if (!curator)
      return;
   kb_health_add_curator_provider(curator);

   kb_curator_queue_counts_t qc;
   kb_curator_queue_counts(&qc);
   if (out_counts)
      *out_counts = qc;
   cJSON *q = cJSON_AddObjectToObject(curator, "queue");
   if (q)
   {
      cJSON_AddNumberToObject(q, "extract_pending", qc.extract_pending);
      cJSON_AddNumberToObject(q, "extract_done", qc.extract_done);
      cJSON_AddNumberToObject(q, "code_unit_pending", qc.code_unit_pending);
      cJSON_AddNumberToObject(q, "code_unit_done", qc.code_unit_done);
      cJSON_AddNumberToObject(q, "extract_failing", qc.extract_failing);
      cJSON_AddNumberToObject(q, "code_unit_failing", qc.code_unit_failing);
      if (qc.last_error[0])
         cJSON_AddStringToObject(q, "last_error", qc.last_error);
   }
}

/* THE ONE PLACE a capability verdict is computed. Add a blocker at the site that
 * discovers the evidence and the summary follows automatically — that is the whole
 * point of this function existing.
 *
 * `status` used to be the literal string "ok", written as the first statement of
 * kb_service_health_object and never revised. Every finding below it -- db2_ok,
 * pgvec_ok, embed_ok, the dimension-refusal counter -- was published as a SIBLING
 * field that nothing aggregated, so the response could say "ok" while carrying the
 * proof it was not. That is not a bug in any one check; it is the absence of a
 * place where the checks add up, and it reproduced three times (here, in
 * server_health_add_kb, and in the transport-breaker patch that answered it by
 * adding yet another sibling for the CLI to special-case).
 *
 * A blocker means CANNOT WORK, not "worth mentioning". Advisory findings stay in
 * `warnings` and leave the verdict at ok: a stale ingest, a disabled synthesis
 * tier, or zero vectors on a fresh install are all supported states, and
 * degrading on them would dilute the signal in exactly the direction this
 * function exists to correct. */
static const char *kb_health_verdict(cJSON *blockers)
{
   return (cJSON_IsArray(blockers) && cJSON_GetArraySize(blockers) > 0) ? "degraded" : "ok";
}

static cJSON *kb_service_health_object(void)
{
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return NULL;
   /* No `status` here. It is derived from the blockers array at the bottom of this
    * function, once the evidence it summarises actually exists. */

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
    * the config file OR the EMBEDDER_URL env (the deploy stack exports the
    * latter), so resolve it the same way embed_command does instead of reading the
    * raw config field — otherwise an env-configured embedder is wrongly reported
    * embed_ok:false while embed_command shows a real URL. Nothing configured
    * reports false — there is no fallback to report instead. */
   const char *embed_cmd = config_embedder_command_current(NULL);
   int embed_ok = embed_cmd[0] ? 1 : 0;
   cJSON_AddBoolToObject(resp, "embed_ok", embed_ok);
   cJSON_AddStringToObject(resp, "embed_command", embed_cmd);

   /* embed_ok reports only that an embedder is CONFIGURED. An embedder that runs
    * and produces the wrong width stores nothing, so publish the refusal count
    * too: non-zero means every vector since startup was dropped and dense
    * retrieval is dead, however healthy the rest of this response looks. */
   long long dim_refused = db2_embedding_dim_refused_count();
   cJSON_AddNumberToObject(resp, "embedding_dim_refused", (double)dim_refused);
   if (dim_refused > 0)
   {
      cJSON_AddNumberToObject(resp, "embedding_dim_expected", db2_embedding_dim());
      cJSON_AddNumberToObject(resp, "embedding_dim_offered", db2_embedding_dim_last_offered());
   }

   /* Curator (§4 observability): per-tier provider config + queue depth. The
    * live four-state reachability probe (ready/loading/gated/down) is deferred to
    * a follow-up — it needs a bounded async probe so the health path never blocks,
    * and the gated state needs a custom curator /health (the bundled official
    * llama.cpp doesn't expose it). api_key is never surfaced. */
   kb_curator_queue_counts_t qc;
   memset(&qc, 0, sizeof(qc));
   kb_health_add_curator(resp, &qc);

   /* Freshness: read last_ingest_at from kb_runtime_state */
   char last_ingest_at[64] = "";
   int freshness_days = -1;
   if (db2_kb_runtime_state_get("last_ingest_at", last_ingest_at, sizeof(last_ingest_at)) == 0 &&
       last_ingest_at[0])
   {
      /* Days elapsed. Shared parser: this read only the space form, and mktime()
       * treated a UTC stamp as local time, skewing freshness by the host offset.
       * An unparseable stamp must leave freshness_days at its -1 "unknown", not
       * fall through to 0 -- "we cannot tell how old this is" and "it is current"
       * are different answers. */
      time_t then = parse_utc_ts(last_ingest_at);
      if (then > 0)
      {
         time_t now = time(NULL);
         freshness_days = (now > then) ? (int)((now - then) / 86400) : 0;
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

   /* Warning accumulation.
    *
    * Two arrays, two meanings, kept deliberately distinct: `warnings` is
    * everything worth saying, `blockers` is the subset that means the kb cannot
    * do its job. Only the latter moves `status`. A blocker names its remedy —
    * the operator reading it is by definition looking at something broken, and
    * "vector store unavailable" without "reinstall the extension" costs them the
    * search that follows. */
   cJSON *warnings = cJSON_AddArrayToObject(resp, "warnings");
   cJSON *blockers = cJSON_AddArrayToObject(resp, "blockers");
   if (!db2_ok)
   {
      cJSON_AddItemToArray(warnings, cJSON_CreateString("DB2 schema not ready"));
      cJSON_AddItemToArray(blockers,
                           cJSON_CreateString("store unavailable: the KB database schema is not "
                                              "ready, so nothing can be stored or retrieved"));
   }
   if (!pgvec_ok)
   {
      cJSON_AddItemToArray(warnings, cJSON_CreateString("pgvector extension not loaded in DB2"));
      cJSON_AddItemToArray(blockers,
                           cJSON_CreateString("vector store unavailable: the pgvector extension is "
                                              "not loaded, so dense retrieval cannot run"));
   }
   if (!pgvec_collection_ok)
   {
      cJSON_AddItemToArray(warnings, cJSON_CreateString("KB vector table missing"));
      cJSON_AddItemToArray(blockers, cJSON_CreateString("vector table missing: the KB chunk vector "
                                                        "table is absent, so search returns "
                                                        "nothing"));
   }
   /* An unconfigured embedder is the single most common way this install reaches
    * "accepted, reports healthy, cannot work": deploy succeeds, the container goes
    * healthy, and the first `aimee memory store` fails with no mention of an
    * embedder anywhere. */
   if (!embed_ok)
      cJSON_AddItemToArray(blockers,
                           cJSON_CreateString("no embedder configured: set embedder_model (or "
                                              "EMBEDDER_URL) — memory and KB search cannot embed"));
   /* A curator that fails every job used to leave the kb reporting a clean
    * bill of health. The counters said pending=41485 done=0 — indistinguishable
    * from "queued, not started yet" — and nothing in warnings mentioned that
    * every one of those jobs had already burned its attempts against an
    * unreachable endpoint. `aimee status` printed "aimee-kb: ok" throughout,
    * for hours, while code indexing was completely dead.
    *
    * Say it. The sample error is included because the count alone does not tell
    * an operator whether to fix a config value or a network. */
   if (qc.code_unit_failing > 0 || qc.extract_failing > 0)
   {
      char msg[384];
      int n = qc.code_unit_failing + qc.extract_failing;
      if (qc.last_error[0])
         snprintf(msg, sizeof(msg), "curator: %d job(s) failing — last error: %s", n,
                  qc.last_error);
      else
         snprintf(msg, sizeof(msg), "curator: %d job(s) failing", n);
      cJSON_AddItemToArray(warnings, cJSON_CreateString(msg));
   }
   /* TWO GATES THAT DO NOT AGREE.
    *
    * memory.store enqueues a "memory_facts" job whenever typed_facts_enabled is on,
    * which is the DEFAULT. The only consumer of those jobs is kb_memory_facts_drain,
    * which runs on the curator LLM lane -- and that lane deliberately does not start
    * without a synthesis endpoint ("NO SYNTHESIS PROVIDER => NO LLM LANE"). Both
    * decisions are individually right. Together they mean an install with no synth
    * provider, which is a supported configuration, enqueues one row per stored
    * memory that nothing will ever claim.
    *
    * Measured on the e2e VM: 4 memory_facts jobs pending for 11.5 hours with
    * attempts=0, while /v1/health reported status ok and an empty warnings array,
    * and `aimee kb status` printed "4 pending" with no hint that pending here means
    * forever. It grows without bound for the life of the install.
    *
    * NOT a blocker. Running without a synthesis provider is explicitly supported,
    * and memory store and search work perfectly; only typed-fact enrichment is
    * deferred. Degrading the verdict for a supported configuration is exactly the
    * dilution this file's status derivation exists to avoid. It is a warning, and
    * it says the backlog is not lost -- configuring a provider drains it. */
   kb_status_add_typed_facts_warning(warnings);

   if (freshness_days > 30)
      cJSON_AddItemToArray(warnings, cJSON_CreateString("KB not ingested in over 30 days"));
   else if (freshness_days > 7)
      cJSON_AddItemToArray(warnings, cJSON_CreateString("KB not ingested in over 7 days"));
   if (stats.chunks > 0 && stats.embeddings < stats.chunks * 9 / 10)
      cJSON_AddItemToArray(warnings,
                           cJSON_CreateString("KB has significant unembedded chunks (>10%)"));

   /* AN EMBEDDER OF THE WRONG WIDTH IS A DEAD KB THAT REPORTS OK.
    *
    * The upsert guard in pgvec_transport.c already refuses a mismatched vector and
    * counts it, and its comment says non-zero "means every vector since startup was
    * dropped and dense retrieval is dead, however healthy the rest of this response
    * looks". The count was published and nothing escalated it, so the response kept
    * saying status ok with an empty warnings array.
    *
    * Measured on CT 302: booting the nomic image (768) over a store recorded at 384
    * gave a running container, /v1/health "status":"ok", "embed_ok":true,
    * "warnings":[], and a memory store that failed with "failed to store memory" and
    * no mention of a dimension anywhere. Nothing was corrupted -- the guard held, and
    * no 768 vector landed beside the 384s -- but nothing said the kb could no longer
    * embed, which is the whole of what an operator needed to know.
    *
    * Reported two ways on purpose:
    *
    *   refused > 0   proof it has already happened, with both widths, so an operator
    *                 does not have to infer which side is wrong
    *   width drift   BEFORE anything tries. The counter starts at zero after every
    *                 restart, so a freshly switched embedder looks perfectly healthy
    *                 until the first write -- which is exactly when someone is
    *                 checking whether the switch worked.
    *
    * The remedy is named because it is not guessable: the corpus has to be re-embedded
    * at the new width, and `aimee kb reembed` is the command that does it. */
   /* NO PROACTIVE WIDTH CHECK HERE, having written one and deleted it.
    *
    * The obvious version compares db2_embedding_dim() against kb_meta's
    * schema_embedding_dim. It cannot fire: both resolve to the RECORDED width, so it
    * agrees with itself. Booting the nomic image (768) over a 384 store leaves both
    * reading 384 while the embedder reports dim 768 on its own /health -- I shipped
    * that check, watched it stay silent against exactly the deployment it was written
    * for, and removed it. A guard that cannot fire is worse than an absent one,
    * because it reads like cover.
    *
    * The serving width is only knowable by asking the embedder: its /health carries
    * "dim" and a serving_id. Doing that needs a bounded probe so this path never
    * blocks -- the same requirement already deferred for the curator reachability
    * probe above -- and it becomes natural rather than bolted-on once the embedder is
    * a sidecar with a health endpoint of its own
    * (docs/proposals/pending/embedder-image-split-and-rebuild.md).
    *
    * So the refusal counter is the signal until then. It is reactive, and it is true. */
   int active_dim = db2_embedding_dim();
   if (dim_refused > 0)
   {
      char msg[320];
      snprintf(msg, sizeof(msg),
               "embedder width mismatch: %lld vector(s) refused (offered %d, store is %d). "
               "Dense retrieval is dead until the corpus is re-embedded at the new width "
               "(`aimee kb reembed`).",
               dim_refused, db2_embedding_dim_last_offered(), active_dim);
      cJSON_AddItemToArray(warnings, cJSON_CreateString(msg));
      /* The comment above says this state means "dense retrieval is dead". It said
       * so while the response reported status ok, because saying it in a warning
       * was the end of the sentence. It is a blocker. */
      cJSON_AddItemToArray(blockers, cJSON_CreateString(msg));
   }

   /* Derived last, from the evidence above, and never written anywhere else. */
   cJSON_AddStringToObject(resp, "status", kb_health_verdict(blockers));

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

   cJSON_AddBoolToObject(resp, "maintenance_enabled", config_kb_maintenance_enabled());

   /* Typed-facts capability (proposal §8): the KB advertises its own typed-facts
    * state so aimee-server can gate per-turn fact injection on it WITHOUT owning
    * the config. The server never reads typed_facts_enabled itself. */
   cJSON_AddBoolToObject(resp, "typed_facts_enabled", config_typed_facts_enabled() ? 1 : 0);

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

   if (!config_kb_maintenance_enabled() && !force)
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
   mcfg.lambda = config_kb_maintenance_lambda();
   mcfg.confidence_floor = config_kb_maintenance_floor();
   mcfg.min_age_days = config_kb_maintenance_min_age_days();
   mcfg.orphan_prune_days = config_kb_maintenance_orphan_days();
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
