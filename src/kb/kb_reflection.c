/* kb_reflection.c: idle-triggered reflection scheduler for aimee-kb.
 *
 * When now - last_session_rpc_ts > review_idle_trigger_minutes, fires a
 * reflection pass over unreflected session_summary artifacts older than
 * review_session_cooldown_hours.  Stamps reflected_at on each processed
 * artifact.  LLM candidate generation runs the shared curator LLM path
 * (kb_curator_llm_run, stage KB_CURATOR_STAGE_SYNTHESIZE_REFLECTION): a
 * configured Tier-B provider, else the legacy kb_synthesize_command sidecar.
 * The scheduler infrastructure, MDL selection, and deduplication run fully.
 *
 * No DB1 access from this file.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_background.h"
#include "kb_reasoning.h"
#include "kb_reflection.h"
#include "aimee.h"
#include "cJSON.h"
#include "config.h"
#include "db2/artifacts.h"
#include "db2/db2.h"             /* db2_lease_release_idle */
#include "kb_curator_llm.h"      /* kb_curator_llm_run — shared curator LLM path */
#include "kb_curator_provider.h" /* KB_CURATOR_STAGE_SYNTHESIZE_REFLECTION, provider_for_stage */
#include "kb_features.h"
#include "kb_mdl.h"
#include "kb_service.h"
#include "log.h"
#include "platform_process.h"

/* Reflection synthesis runs through the shared curator LLM path
 * (kb_curator_llm_run): a configured Tier-B provider (provider_client, strict
 * JSON) if present, else the legacy kb_synthesize_command sidecar as fallback.
 * The provider path needs a system prompt (the legacy sidecar carried its own). */
#define REFLECTION_SYNTH_OUTBUF 16384
static const char *const REFLECTION_SYNTH_SYSTEM_PROMPT =
    "You consolidate a batch of session-summary evidence into ONE higher-order "
    "insight. Reply with a single JSON object and nothing else: "
    "{\"candidate\": \"<the synthesized insight, one or two sentences>\", "
    "\"cluster\": \"<a short topic label the insight belongs to>\", "
    "\"confidence\": <0.0-1.0>}. Ground the insight in the evidence; do not invent "
    "facts. If the evidence supports no durable insight, return a low confidence.";

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Declared in kb_service_workers.c */
extern kb_service_ctx_t *g_kb_ctx;

static int run_synthesis_pass(const config_t *cfg, const db2_artifact_proposed_t *row)
{
   int n_attempts = cfg->kb_synthesize_n_attempts > 0 ? cfg->kb_synthesize_n_attempts : 3;
   if (n_attempts > MDL_MAX_CANDIDATES)
      n_attempts = MDL_MAX_CANDIDATES;

   const char *candidates[MDL_MAX_CANDIDATES];
   const char *clusters[MDL_MAX_CANDIDATES];
   double confidences[MDL_MAX_CANDIDATES];
   char *candidate_bufs[MDL_MAX_CANDIDATES] = {0};
   char *cluster_bufs[MDL_MAX_CANDIDATES] = {0};
   int n_valid = 0;

   const char *evidence = row->payload_json[0] ? row->payload_json : "{}";

   /* Graph reasoning: enrich evidence with citation_reachable context (deep-curator surface). */
   char *evidence_enriched = NULL;
   {
      char bindings[128];
      snprintf(bindings, sizeof(bindings), "{\"?a\":\"%s\"}", row->id);
      kb_reasoning_result_t graph_ctx = {0};
      if (kb_reasoning_query(cfg, "citation_reachable(?a, ?b)", bindings, NULL, NULL, &graph_ctx) ==
              0 &&
          graph_ctx.n_rows > 0)
      {
         cJSON *ev = cJSON_Parse(evidence);
         if (ev)
         {
            cJSON *cited = cJSON_AddArrayToObject(ev, "graph_citations");
            for (int gi = 0; gi < graph_ctx.n_rows && gi < 16; gi++)
            {
               for (int vi = 0; vi < graph_ctx.rows[gi].n_vars; vi++)
               {
                  if (strcmp(graph_ctx.rows[gi].vars[vi].var, "?b") == 0 &&
                      graph_ctx.rows[gi].vars[vi].value[0])
                  {
                     cJSON_AddItemToArray(cited,
                                          cJSON_CreateString(graph_ctx.rows[gi].vars[vi].value));
                     break;
                  }
               }
            }
            evidence_enriched = cJSON_PrintUnformatted(ev);
            cJSON_Delete(ev);
         }
      }
      kb_reasoning_result_free(&graph_ctx);
   }
   if (evidence_enriched)
      evidence = evidence_enriched;

   for (int i = 0; i < n_attempts; i++)
   {
      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "task", "synthesize");
      cJSON_AddStringToObject(req, "evidence", evidence);
      cJSON_AddNumberToObject(req, "attempt", i + 1);
      char *req_str = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);

      /* Route through the shared curator LLM path: a configured Tier-B provider
       * (provider_client) if present, else the legacy kb_synthesize_command
       * sidecar. A NULL return is a fail — skip this attempt (fail-closed: no
       * durable write happens on a failed/empty response). */
      char serr[256];
      char *out = kb_curator_llm_run(
          cfg, KB_CURATOR_STAGE_SYNTHESIZE_REFLECTION, REFLECTION_SYNTH_SYSTEM_PROMPT, req_str,
          NULL, cfg->kb_synthesize_command, REFLECTION_SYNTH_OUTBUF, serr, sizeof(serr));
      free(req_str);

      if (!out)
         continue;

      /* Tolerate prose/fence-wrapped output: scan to the first JSON object. */
      const char *start = strchr(out, '{');
      cJSON *resp = start ? cJSON_Parse(start) : NULL;
      free(out);

      if (!resp)
         continue;

      cJSON *cand = cJSON_GetObjectItemCaseSensitive(resp, "candidate");
      cJSON *cl = cJSON_GetObjectItemCaseSensitive(resp, "cluster");
      cJSON *conf = cJSON_GetObjectItemCaseSensitive(resp, "confidence");

      if (!cJSON_IsString(cand) || !cand->valuestring)
      {
         cJSON_Delete(resp);
         continue;
      }

      candidate_bufs[n_valid] = strdup(cand->valuestring);
      if (!candidate_bufs[n_valid])
      {
         cJSON_Delete(resp);
         continue;
      }
      candidates[n_valid] = candidate_bufs[n_valid];

      cluster_bufs[n_valid] = strdup(cJSON_IsString(cl) && cl->valuestring ? cl->valuestring : "");
      clusters[n_valid] = cluster_bufs[n_valid] ? cluster_bufs[n_valid] : "";
      confidences[n_valid] = cJSON_IsNumber(conf) ? conf->valuedouble : 0.5;
      n_valid++;

      cJSON_Delete(resp);
   }

   if (n_valid == 0)
   {
      aimee_log(LOG_WARN, "kb.reflection", "synthesis sidecar returned no valid candidates for %s",
                row->id);
      free(evidence_enriched);
      return -1;
   }

   kb_mdl_score_t scores[MDL_MAX_CANDIDATES];
   int winner = kb_mdl_select_agreed_cluster(candidates, clusters, n_valid, evidence, 2, scores);
   if (winner < 0)
   {
      winner = 0;
      aimee_log(LOG_INFO, "kb.reflection",
                "no MDL agreement cluster for %s; falling back to attempt index 0", row->id);
   }

   cJSON *win = cJSON_CreateObject();
   cJSON_AddStringToObject(win, "candidate", candidates[winner]);
   cJSON_AddStringToObject(win, "source_artifact_id", row->id);
   cJSON_AddNumberToObject(win, "mdl_total", scores[winner].total);
   char *win_str = cJSON_PrintUnformatted(win);
   cJSON_Delete(win);

   /* Shadow gate (proposal §4): score and log the winner as evidence, but write
    * NO durable candidate — fail-closed, nothing enters the promotion pipeline.
    * Promoting the shadow stream to normal is a bandit decision
    * (reflection_synthesis_mode), wired separately; this never flips itself. */
   if (cfg->kb_reflection_synthesis_shadow)
   {
      aimee_log(LOG_INFO, "kb.reflection",
                "shadow synthesis (scored, unpromoted): source=%s winner=%d conf=%.2f "
                "mdl_total=%.2f candidate=%s",
                row->id, winner, confidences[winner], scores[winner].total, win_str);
      free(win_str);
      for (int i = 0; i < n_valid; i++)
      {
         free(candidate_bufs[i]);
         free(cluster_bufs[i]);
      }
      free(evidence_enriched);
      return 0;
   }

   char new_id[37];
   db2_artifact_gen_id(new_id, sizeof(new_id));

   int write_rc = db2_artifact_write_ex(new_id, "session_synthesis", "proposed", "system", "",
                                        "kb_reflection", confidences[winner], n_valid, win_str);
   free(win_str);

   if (write_rc == 0)
   {
      kb_features_upsert_synthesis_mdl(new_id, &scores[winner]);
      aimee_log(LOG_INFO, "kb.reflection",
                "synthesis candidate committed: id=%s winner=%d mdl_total=%.2f attempts=%d", new_id,
                winner, scores[winner].total, n_valid);
   }
   else
   {
      aimee_log(LOG_WARN, "kb.reflection", "failed to write synthesis artifact for %s", row->id);
   }

   for (int i = 0; i < n_valid; i++)
   {
      free(candidate_bufs[i]);
      free(cluster_bufs[i]);
   }

   free(evidence_enriched);
   return write_rc;
}

static void run_reflection_pass(const config_t *cfg)
{
   db2_artifact_proposed_t rows[10];
   int batch = cfg->review_batch_cap > 0 ? cfg->review_batch_cap : 10;
   if (batch > 10)
      batch = 10;

   /* List proposed session_summary artifacts (target_surface NULL = all).
    * We filter by kind="session_summary" below. */
   int n = db2_artifact_list_proposed(NULL, batch * 4, rows, 10);
   if (n <= 0)
      return;

   long cooldown_secs = (long)cfg->review_session_cooldown_hours * 3600;
   long now = (long)time(NULL);
   int processed = 0;

   for (int i = 0; i < n && processed < batch; i++)
   {
      if (strcmp(rows[i].kind, "session_summary") != 0)
         continue;

      /* Parse created_at and check cooldown */
      if (rows[i].created_at[0])
      {
         struct tm tm;
         memset(&tm, 0, sizeof(tm));
         if (strptime(rows[i].created_at, "%Y-%m-%dT%H:%M:%SZ", &tm) ||
             strptime(rows[i].created_at, "%Y-%m-%d %H:%M:%S", &tm))
         {
            long created = (long)timegm(&tm);
            if (now - created < cooldown_secs)
               continue;
         }
      }

      aimee_log(LOG_INFO, "kb.reflection", "processing session artifact %s", rows[i].id);

      /* Stamp reflected_at to prevent re-processing */
      if (db2_artifact_stamp_reflected(rows[i].id) != 0)
      {
         aimee_log(LOG_WARN, "kb.reflection", "failed to stamp reflected_at on %s", rows[i].id);
         continue;
      }

      /* Run when MDL tie-break is on AND we have somewhere to send the work: a
       * configured Tier-B provider (provider_client) or the legacy sidecar. */
      provider_def_t rprov;
      int have_provider =
          kb_curator_provider_for_stage(cfg, KB_CURATOR_STAGE_SYNTHESIZE_REFLECTION, &rprov);
      if ((cfg->kb_synthesize_command[0] != '\0' || have_provider) && cfg->kb_mdl_tiebreak_enabled)
      {
         run_synthesis_pass(cfg, &rows[i]);
      }
      else
      {
         aimee_log(LOG_INFO, "kb.reflection", "session %s reflected (synthesis LLM not configured)",
                   rows[i].id);
      }
      processed++;
   }

   if (processed > 0)
      aimee_log(LOG_INFO, "kb.reflection", "pass complete: %d session artifact(s) processed",
                processed);
}

static kb_reflection_ctx_t *g_rctx = NULL;

static void *reflection_thread_main(void *arg)
{
   kb_reflection_ctx_t *ctx = (kb_reflection_ctx_t *)arg;

   config_t cfg;
   config_load(&cfg);
   long idle_threshold = (long)cfg.review_idle_trigger_minutes * 60;
   if (idle_threshold <= 0)
      idle_threshold = 1800;

   long last_fired = (long)time(NULL);

   while (!ctx->stop)
   {
      /* Return any pool connection a prior reflection pass acquired lazily
       * (run_reflection_pass uses db2_conn() at lease depth 0, not an explicit
       * begin/end scope) before idling, so this long-lived thread does not pin
       * one pool member for its whole lifetime — the stuck-lease reaper flags it
       * and it permanently shrinks the bounded pool. Matches the curator drain
       * (kb_curator_drain.c) and the maintenance timer (kb_service_workers.c). */
      db2_lease_release_idle();
      sleep(1);
      if (ctx->stop)
         break;

      /* Reload config periodically for live changes */
      long now = (long)time(NULL);
      if ((now - last_fired) % 300 == 0)
         config_load(&cfg);

      if (!cfg.review_scheduler_enabled)
         continue;

      idle_threshold = (long)cfg.review_idle_trigger_minutes * 60;
      if (idle_threshold <= 0)
         idle_threshold = 1800;

      /* Check idle window */
      long last_rpc = g_kb_ctx ? g_kb_ctx->last_session_rpc_ts : (long)time(NULL);
      long idle_secs = now - last_rpc;

      if (idle_secs < idle_threshold)
         continue;

      /* Fire reflection pass */
      aimee_log(LOG_INFO, "kb.reflection", "idle=%lds >= threshold=%lds; firing reflection pass",
                idle_secs, idle_threshold);
      kb_background_set("reflection", "idle=%lds threshold=%lds", idle_secs, idle_threshold);
      run_reflection_pass(&cfg);
      kb_background_clear("reflection");
      last_fired = now;

      /* Back off until the next idle window */
      sleep((int)(idle_threshold / 2));
   }
   return NULL;
}

void kb_reflection_init(kb_reflection_ctx_t *ctx)
{
   if (!ctx)
      return;
   memset(ctx, 0, sizeof(*ctx));
   g_rctx = ctx;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.review_scheduler_enabled)
   {
      aimee_log(LOG_DEBUG, "kb.reflection",
                "scheduler disabled (learning.review.scheduler_enabled=0)");
      return;
   }

   ctx->stop = 0;
   if (pthread_create(&ctx->thread, NULL, reflection_thread_main, ctx) == 0)
   {
      ctx->active = 1;
      aimee_log(LOG_INFO, "kb.reflection", "reflection scheduler started");
   }
   else
   {
      aimee_log(LOG_WARN, "kb.reflection", "failed to start reflection scheduler thread");
   }
}

void kb_reflection_shutdown(kb_reflection_ctx_t *ctx)
{
   if (!ctx || !ctx->active)
      return;
   ctx->stop = 1;
   pthread_join(ctx->thread, NULL);
   ctx->active = 0;
   aimee_log(LOG_INFO, "kb.reflection", "reflection scheduler stopped");
}
