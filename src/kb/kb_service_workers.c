/* kb_service_workers.c: kb_service socket init/shutdown.
 * aimee-kb owns DB2 and now drives KB ingest in-process via the worker
 * pool in kb_ingest_workers.c, alongside its KB maintenance, reflection,
 * curation, and mining background loops. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "aimee.h"
#include "kb_background.h"
#include "kb_curator_drain.h"
#include "kb_ingest_workers.h"
#include "kb_learning_version.h"
#include "kb_curator_version.h"
#include "kb_features.h"
#include "kb_mining.h"
#include "kb_ranker_fit.h"
#include "kb_reasoning.h"
#include "kb_reflection.h"
#include "kb_service.h"
#include "config.h"
#include "db2/db2.h"
#include "db2/kb_service_backend.h"
#include "db2/kb_maintenance.h"
#include "kb_blob_reconcile.h"
#include "db2/kb_runtime_state.h"
#include "log.h"
#include "cJSON.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef AIMEE_WINDOWS
#include <unistd.h>
#endif

kb_service_ctx_t *g_kb_ctx = NULL;

static volatile int g_maintenance_stop = 0;
static kb_reflection_ctx_t g_reflection_ctx;
static kb_curator_drain_ctx_t g_curator_drain_ctx;

char *kb_service_workers_json(kb_service_ctx_t *ctx)
{
   if (!ctx)
      return NULL;
   int configured = ctx->worker_count > 0 ? ctx->worker_count : 1;
   cJSON *resp = kb_workers_response_build(configured, kb_service_conn_slots_json(configured));
   if (!resp)
      return NULL;

   char *threads_json = kb_service_threads_json(ctx);
   cJSON *threads = threads_json ? cJSON_Parse(threads_json) : NULL;
   free(threads_json);
   if (threads)
      cJSON_AddItemToObject(resp, "threads", threads);

   char *out = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   return out;
}

static void *kb_maintenance_timer_thread(void *arg)
{
   (void)arg;

   config_t cfg;
   config_load(&cfg);
   int interval_secs = cfg.kb_maintenance_interval_hours * 3600;
   if (interval_secs <= 0)
      interval_secs = 86400;

   long elapsed = 0;
   while (!g_maintenance_stop)
   {
      /* Drop any pool connection held from the last maintenance run so this
       * long-lived timer doesn't pin one while idle (stuck-lease reaper). */
      db2_lease_release_idle();
      sleep(1);
      elapsed++;
      if (elapsed < interval_secs)
         continue;
      elapsed = 0;

      config_load(&cfg);
      if (!cfg.kb_maintenance_enabled)
         continue;

      kb_maintenance_config_t mcfg;
      kb_maintenance_config_defaults(&mcfg);
      mcfg.lambda = cfg.kb_maintenance_lambda;
      mcfg.confidence_floor = cfg.kb_maintenance_floor;
      mcfg.min_age_days = cfg.kb_maintenance_min_age_days;
      mcfg.orphan_prune_days = cfg.kb_maintenance_orphan_days;
      mcfg.dry_run = 0;

      kb_background_set("maintenance", "lambda=%.2f floor=%.2f min_age=%dd", mcfg.lambda,
                        mcfg.confidence_floor, mcfg.min_age_days);

      kb_maintenance_result_t res;
      int rc = kb_maintenance_run(&mcfg, &res);
      if (rc != 0)
      {
         aimee_log(LOG_WARN, "kb.maintenance", "scheduled run failed: %s", res.error);
      }
      else
      {
         aimee_log(LOG_INFO, "kb.maintenance",
                   "scheduled run %s: decayed=%d pruned=%d elapsed=%ldms", res.run_id,
                   res.rows_decayed, res.orphans_pruned, res.elapsed_ms);
         db2_kb_runtime_state_set_now("last_maintenance_at");
         char buf[32];
         snprintf(buf, sizeof(buf), "%d", res.rows_decayed);
         db2_kb_runtime_state_set("last_maintenance_decayed", buf);
         snprintf(buf, sizeof(buf), "%d", res.orphans_pruned);
         db2_kb_runtime_state_set("last_maintenance_pruned", buf);
      }

      /* Phase 4: learning-to-rank scheduled refit (default-off). Folded into the
       * same cycle. Keyed on the current KB_FEATURE_SET_VERSION — kb_ranker_fit_run
       * trains only on that version's rows and stamps it on the model, so a
       * v1→v2 feature-set bump naturally re-fits against v2 the next cycle. The
       * §2 floor/refusal + benchmark gate keep it from ever shipping a bad model. */
      if (cfg.kb_ranker_fit_enabled)
      {
         kb_background_set("ranker_fit", "%s", KB_FEATURE_SET_VERSION);
         char *fit_report = NULL;
         int frc = kb_ranker_fit_run(&cfg, NULL, 0, &fit_report);
         aimee_log(frc == 0 ? LOG_INFO : LOG_DEBUG, "kb.ranker_fit",
                   "scheduled refit rc=%d report=%s", frc, fit_report ? fit_report : "(none)");
         free(fit_report);
         db2_kb_runtime_state_set_now("last_ranker_fit_at");
         kb_background_clear("ranker_fit");
      }

      kb_background_clear("maintenance");
   }
   return NULL;
}

/* structured-PDF Phase C: orphan-blob reconciliation timer (its own cadence, independent of
 * the maintenance sweep — default hourly). A no-op until kb_pdf_assets_enabled is on. */
static volatile int g_blob_recon_stop = 0;
static pthread_t g_blob_recon_thread;
static int g_blob_recon_active = 0;

static void *kb_blob_recon_timer_thread(void *arg)
{
   (void)arg;
   config_t cfg;
   long elapsed = 0;
   while (!g_blob_recon_stop)
   {
      db2_lease_release_idle();
      sleep(1);
      elapsed++;
      config_load(&cfg);
      int interval = cfg.kb_pdf_blob_recon_secs;
      if (!cfg.kb_pdf_assets_enabled || interval <= 0)
      {
         elapsed = 0;
         continue;
      }
      if (elapsed < interval)
         continue;
      elapsed = 0;
      kb_blob_recon_stats_t st;
      if (kb_blob_reconcile_run(cfg.kb_pdf_blob_orphan_alarm_mb, KB_BLOB_RECON_GRACE_SECS, &st) ==
          0)
      {
         db2_kb_runtime_state_set_now("last_blob_recon_at");
         char buf[32];
         snprintf(buf, sizeof(buf), "%lld", st.orphans_unlinked);
         db2_kb_runtime_state_set("last_blob_recon_unlinked", buf);
      }
   }
   return NULL;
}

/* kb_worker_notify() is defined in kb_ingest_workers.c — it wakes the
 * in-process ingest worker pool. */

char *kb_service_threads_json(kb_service_ctx_t *ctx)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;

   cJSON *ingest = cJSON_CreateObject();
   if (ingest)
   {
      cJSON_AddStringToObject(ingest, "name", "ingest-workers");
      cJSON_AddBoolToObject(ingest, "active", ctx && ctx->ingest_count > 0);
      cJSON_AddStringToObject(ingest, "state",
                              ctx && ctx->ingest_count > 0 ? "waiting" : "disabled");
      cJSON_AddNumberToObject(ingest, "threads", ctx ? ctx->ingest_count : 0);
      cJSON_AddItemToArray(arr, ingest);
   }

   cJSON *ingest_timer = cJSON_CreateObject();
   if (ingest_timer)
   {
      cJSON_AddStringToObject(ingest_timer, "name", "ingest-timer");
      cJSON_AddBoolToObject(ingest_timer, "active", ctx && ctx->ingest_timer_active);
      cJSON_AddStringToObject(ingest_timer, "state",
                              ctx && ctx->ingest_timer_active ? "sleeping" : "disabled");
      cJSON_AddItemToArray(arr, ingest_timer);
   }

   cJSON *ingest_watch = cJSON_CreateObject();
   if (ingest_watch)
   {
      cJSON_AddStringToObject(ingest_watch, "name", "ingest-watch");
      cJSON_AddBoolToObject(ingest_watch, "active", ctx && ctx->bg_watch_active);
      cJSON_AddStringToObject(ingest_watch, "state",
                              ctx && ctx->bg_watch_active ? "watching" : "disabled");
      cJSON_AddItemToArray(arr, ingest_watch);
   }

   cJSON *maintenance = cJSON_CreateObject();
   if (maintenance)
   {
      cJSON_AddStringToObject(maintenance, "name", "maintenance-timer");
      cJSON_AddBoolToObject(maintenance, "active", ctx && ctx->bg_timer_active);
      cJSON_AddStringToObject(maintenance, "state",
                              ctx && ctx->bg_timer_active ? "sleeping" : "disabled");
      cJSON_AddItemToArray(arr, maintenance);
   }

   cJSON *reflection = cJSON_CreateObject();
   if (reflection)
   {
      cJSON_AddStringToObject(reflection, "name", "reflection-scheduler");
      cJSON_AddBoolToObject(reflection, "active", g_reflection_ctx.active);
      cJSON_AddStringToObject(reflection, "state",
                              g_reflection_ctx.active ? "waiting" : "disabled");
      cJSON_AddItemToArray(arr, reflection);
   }

   cJSON *curator = cJSON_CreateObject();
   if (curator)
   {
      cJSON_AddStringToObject(curator, "name", "curator-drain");
      cJSON_AddBoolToObject(curator, "active", g_curator_drain_ctx.active);
      cJSON_AddStringToObject(curator, "state",
                              g_curator_drain_ctx.active ? "polling" : "disabled");
      cJSON_AddItemToArray(arr, curator);
   }

   cJSON *mining = cJSON_CreateObject();
   if (mining)
   {
      cJSON_AddStringToObject(mining, "name", "mining-scheduler");
      cJSON_AddBoolToObject(mining, "active", kb_mining_active());
      cJSON_AddStringToObject(mining, "state", kb_mining_active() ? "polling" : "disabled");
      cJSON_AddItemToArray(arr, mining);
   }

   char *out = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return out;
}

int kb_service_init(kb_service_ctx_t *ctx)
{
   memset(ctx, 0, sizeof(*ctx));
   ctx->listen_fd = -1;
   ctx->bg_listen_fd = -1;
   ctx->running = 1;
   ctx->start_time = (long)time(NULL);

   g_kb_ctx = ctx;

   /* Reset any jobs left running from a previous crash so this process's
    * ingest workers can claim them again. */
   int reset = db2_kb_ingest_queue_reset_running();
   if (reset > 0)
      aimee_log(LOG_INFO, "kb.service", "crash recovery: reset %d running job(s) to pending",
                reset);

   /* Drive KB ingest in-process (claim from DB2, build, store). */
   kb_ingest_workers_start(ctx);

   g_maintenance_stop = 0;
   if (pthread_create(&ctx->bg_timer_thread, NULL, kb_maintenance_timer_thread, NULL) == 0)
      ctx->bg_timer_active = 1;
   else
      aimee_log(LOG_WARN, "kb.service", "failed to start maintenance timer thread");

   g_blob_recon_stop = 0;
   if (pthread_create(&g_blob_recon_thread, NULL, kb_blob_recon_timer_thread, NULL) == 0)
      g_blob_recon_active = 1;
   else
      aimee_log(LOG_WARN, "kb.service", "failed to start blob reconciliation timer thread");

   kb_reflection_init(&g_reflection_ctx);
   kb_curator_drain_init(&g_curator_drain_ctx);
   kb_reasoning_seed_ruleset();

   config_t cfg;
   config_load(&cfg);
   if (cfg.kb_mining_enabled && kb_mining_start(cfg.kb_mining_min_poll_s) != 0)
      aimee_log(LOG_WARN, "kb.mining", "failed to start mining scheduler");

   /* Version-bump replay: if the embedding model or synthesis prompt version
    * changed since last boot, re-enqueue the affected work for the drains. */
   (void)learning_version_replay(cfg.learning_embed_model_version,
                                 cfg.learning_synthesize_prompt_version, NULL);
   {
      kb_curator_version_replay_t cvr;
      (void)kb_curator_version_replay(cfg.kb_curator_extract_prompt_version,
                                      cfg.kb_curator_embed_model_version, &cvr);
   }

   return 0;
}

void kb_service_shutdown(kb_service_ctx_t *ctx)
{
   if (!ctx)
      return;
   ctx->running = 0;

   kb_ingest_workers_stop(ctx);
   kb_curator_drain_shutdown(&g_curator_drain_ctx);
   kb_reflection_shutdown(&g_reflection_ctx);
   kb_mining_stop();

   if (ctx->bg_timer_active)
   {
      g_maintenance_stop = 1;
      pthread_join(ctx->bg_timer_thread, NULL);
      ctx->bg_timer_active = 0;
   }
   if (g_blob_recon_active)
   {
      g_blob_recon_stop = 1;
      pthread_join(g_blob_recon_thread, NULL);
      g_blob_recon_active = 0;
   }
}
