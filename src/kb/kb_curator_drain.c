/* kb_curator_drain.c: curator drain thread — polls for pending extract_doc
 * jobs, rate-limits via a sliding-window ring buffer, and calls
 * kb_curator_extract_one() to process each job. The same thread also drains
 * the evidence_index_ops embed queue (kb_evidence_embed_drain), independent of
 * the curator extract gates.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_drain.h"
#include "kb_curator_extract.h"
#include "kb_curator_resolve_entities.h"
#include "kb_curator_index_narrative.h"
#include "kb_curator_index_claims.h"
#include "kb_curator_contradictions.h"
#include "kb_curator_index_code_unit.h"
#include "kb_curator_link_artifacts.h"
#include "kb_curator_synthesize.h"
#include "kb_curator_promote.h"
#include "kb_evidence_embed.h"
#include "kb_learning_synth.h"
#include "kb_service_code_embed.h"
#include "kb.h"
#include "index.h"
#include "aimee.h"
#include "config.h"
#include "kb_background.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DRAIN_POLL_SECS 5
/* Max synthesis ops processed per poll — each is an LLM round-trip. */
#define SYNTH_DRAIN_BATCH 4

static void *drain_thread_main(void *arg)
{
   kb_curator_drain_ctx_t *ctx = (kb_curator_drain_ctx_t *)arg;

   config_t cfg;
   config_load(&cfg);

   int max_jobs = cfg.kb_curator_max_jobs_per_hour > 0 ? cfg.kb_curator_max_jobs_per_hour : 120;

   /* Ring buffer of completion timestamps for rate limiting */
   long *ring = calloc((size_t)max_jobs, sizeof(long));
   if (!ring)
   {
      aimee_log(LOG_WARN, "kb.curator.drain", "out of memory; drain thread exiting");
      return NULL;
   }
   int ring_head = 0;
   int ring_count = 0;

   long last_cfg_reload = (long)time(NULL);

   while (!ctx->stop)
   {
      sleep(DRAIN_POLL_SECS);
      if (ctx->stop)
         break;

      long now = (long)time(NULL);

      /* Reload config periodically */
      if (now - last_cfg_reload >= 300)
      {
         config_load(&cfg);
         last_cfg_reload = now;
         int new_max =
             cfg.kb_curator_max_jobs_per_hour > 0 ? cfg.kb_curator_max_jobs_per_hour : 120;
         if (new_max != max_jobs)
         {
            long *new_ring = calloc((size_t)new_max, sizeof(long));
            if (new_ring)
            {
               free(ring);
               ring = new_ring;
               ring_head = 0;
               ring_count = 0;
               max_jobs = new_max;
            }
         }
      }

      /* Evidence-vector embed drain — runs every poll, independent of the
       * curator extract gates (it fills evidence_vectors for the neighbourhood
       * builder; the builtin embedder needs no external sidecar). */
      if (cfg.kb_evidence_embed_enabled)
      {
         const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";
         int n = kb_evidence_embed_drain(cfg.kb_evidence_embed_batch, embed_cmd);
         if (n > 0)
            aimee_log(LOG_DEBUG, "kb.evidence.embed", "drained %d evidence op(s)", n);
      }

      /* Code-vector embed drain — pipeline stage 2 (the 0.6B embedder), runs
       * every poll. Indexing (the structural scan) populates the `files` table
       * for everything ingested — code AND docs/config; this embeds those rows
       * into code_embeddings via the configured embedder, incrementally: the
       * "changed_files" scope skips any file whose content_hash already matches,
       * so a poll with nothing new is cheap, and a fresh 23k-file ingest catches
       * up over a handful of polls (max_points caps each project per poll). It is
       * gated only on an embedder being configured — no external LLM, no curator
       * gate — so embeddings appear automatically right after ingest. */
      if (cfg.embedding_command[0])
      {
         project_info_t projects[128];
         int np = index_list_projects(projects, 128);
         int total = 0;
         for (int i = 0; i < np; i++)
         {
            kb_code_embed_result_t r;
            memset(&r, 0, sizeof(r));
            if (kb_code_embed_refresh(projects[i].name, "changed_files", NULL, 0, 0, 0, 0, &r) == 0)
               total += (int)r.embedded;
         }
         if (total > 0)
            aimee_log(LOG_DEBUG, "kb.code.embed",
                      "embedded %d code/doc vector(s) across %d project(s)", total, np);
      }

      /* KB-docs ingest drain — the in-ingest replacement for `kb build`. For
       * each indexed project, chunk + embed prose/doc files (markdown, rst, txt,
       * …) into the curated kb_documents layer, reading content from DB2
       * file_contents (no disk). Bounded per poll; backfills then idles cheaply.
       * Same embedder gate as the code drain. */
      if (cfg.embedding_command[0])
      {
         project_info_t projects[128];
         int np = index_list_projects(projects, 128);
         int total = 0;
         for (int i = 0; i < np; i++)
         {
            int e = kb_doc_refresh(projects[i].name, cfg.embedding_command, 200);
            if (e > 0)
               total += e;
            /* Self-heal chunks that exist but lost their embedding (partial
             * ingest, late embedder, or a vector-store dim reset). */
            int b = kb_doc_embed_backfill(projects[i].name, cfg.embedding_command, 200);
            if (b > 0)
               total += b;
         }
         if (total > 0)
            aimee_log(LOG_DEBUG, "kb.docs.ingest", "ingested %d doc chunk(s) across %d project(s)",
                      total, np);
      }

      /* Candidate-generation synthesis drain — the heavy LLM pass, on the
       * scheduler, never on the capture hot path. Off by default. */
      if (cfg.learning_synthesize_enabled && cfg.learning_synthesize_command[0])
      {
         const char *embed_cmd = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";
         /* Bound LLM calls per poll — each op is one sidecar/LLM round-trip. */
         int n =
             kb_learning_synth_drain(SYNTH_DRAIN_BATCH, cfg.learning_synthesize_command, embed_cmd,
                                     cfg.learning_synthesize_k, cfg.learning_synthesize_max_tokens);
         if (n > 0)
            aimee_log(LOG_DEBUG, "kb.learning.synth", "drained %d synthesis op(s)", n);
      }

      if (!cfg.kb_curator_extract_docs_enabled && !cfg.kb_curator_extract_code_enabled &&
          !cfg.kb_curator_resolve_entities_enabled && !cfg.kb_curator_index_narrative_enabled &&
          !cfg.kb_curator_index_claims_enabled && !cfg.kb_curator_detect_contradictions_enabled &&
          !cfg.kb_curator_index_code_unit_enabled && !cfg.kb_curator_link_artifacts_enabled &&
          !cfg.kb_curator_synthesize_enabled && !cfg.kb_curator_promote_entity_enabled)
      {
         kb_background_clear("curator");
         continue;
      }

      /* Rate limiting: check if window is full */
      if (ring_count >= max_jobs)
      {
         int oldest_idx = (ring_head - ring_count + max_jobs) % max_jobs;
         long oldest_ts = ring[oldest_idx];
         long age = now - oldest_ts;
         if (age < 3600)
         {
            kb_background_set("curator", "rate-limited window=%d/%d", ring_count, max_jobs);
            aimee_log(LOG_DEBUG, "kb.curator.drain",
                      "rate limit: %d/%d jobs in last hour; sleeping %lds", ring_count, max_jobs,
                      3600 - age);
            sleep((int)(3600 - age));
            continue;
         }
         /* Oldest entry expired — slide the window */
         ring_count--;
      }

      kb_curator_extract_opts_t opts;
      memset(&opts, 0, sizeof(opts));
      snprintf(opts.extract_command, sizeof(opts.extract_command), "%s",
               cfg.kb_curator_extract_command);
      opts.max_tokens =
          cfg.kb_curator_extract_max_tokens > 0 ? cfg.kb_curator_extract_max_tokens : 2048;
      opts.max_attempts = cfg.kb_curator_max_attempts > 0 ? cfg.kb_curator_max_attempts : 3;

      int rc = 0;
      if (cfg.kb_curator_extract_docs_enabled)
      {
         kb_background_set("curator", "extract docs (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_extract_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_extract_code_enabled)
      {
         kb_background_set("curator", "extract code (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_extract_code_unit_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_resolve_entities_enabled)
      {
         kb_background_set("curator", "resolve entities (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_resolve_entities_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_index_narrative_enabled)
      {
         kb_background_set("curator", "index narrative (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_index_narrative_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_index_claims_enabled)
      {
         kb_background_set("curator", "index claims (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_index_claims_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_detect_contradictions_enabled)
      {
         kb_background_set("curator", "detect contradictions (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_detect_contradictions_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_index_code_unit_enabled)
      {
         kb_background_set("curator", "index code_unit (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_index_code_unit_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_link_artifacts_enabled)
      {
         kb_background_set("curator", "link artifacts (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_link_artifacts_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_synthesize_enabled)
      {
         kb_background_set("curator", "synthesize topic (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_synthesize_one(&opts);
      }
      if (rc == 0 && cfg.kb_curator_promote_entity_enabled)
      {
         kb_background_set("curator", "promote entity (window=%d/%d)", ring_count, max_jobs);
         rc = kb_curator_promote_entity_one(&opts);
      }

      if (rc == 1)
      {
         /* Record completion in ring buffer */
         ring[ring_head % max_jobs] = (long)time(NULL);
         ring_head = (ring_head + 1) % max_jobs;
         if (ring_count < max_jobs)
            ring_count++;
         aimee_log(LOG_DEBUG, "kb.curator.drain", "job processed (%d/%d in window)", ring_count,
                   max_jobs);
      }
      else if (rc == 0)
      {
         /* No jobs pending — wait a full poll interval before next attempt */
         kb_background_clear("curator");
         sleep(DRAIN_POLL_SECS);
      }
      else
      {
         aimee_log(LOG_WARN, "kb.curator.drain", "extractor returned error");
      }
      kb_background_clear("curator");
   }

   kb_background_clear("curator");
   free(ring);
   return NULL;
}

void kb_curator_drain_init(kb_curator_drain_ctx_t *ctx)
{
   if (!ctx)
      return;
   memset(ctx, 0, sizeof(*ctx));

   config_t cfg;
   config_load(&cfg);

   if (!cfg.kb_curator_extract_docs_enabled && !cfg.kb_curator_extract_code_enabled &&
       !cfg.kb_curator_resolve_entities_enabled && !cfg.kb_curator_index_narrative_enabled &&
       !cfg.kb_curator_index_claims_enabled && !cfg.kb_curator_detect_contradictions_enabled &&
       !cfg.kb_curator_index_code_unit_enabled && !cfg.kb_curator_link_artifacts_enabled &&
       !cfg.kb_curator_synthesize_enabled && !cfg.kb_curator_promote_entity_enabled &&
       !cfg.kb_evidence_embed_enabled && !cfg.learning_synthesize_enabled)
   {
      aimee_log(LOG_DEBUG, "kb.curator.drain",
                "all gates off (kb_curator_extract_docs_enabled=0,"
                " kb_curator_extract_code_enabled=0, kb_curator_resolve_entities_enabled=0,"
                " kb_curator_index_narrative_enabled=0, kb_curator_index_claims_enabled=0,"
                " kb_evidence_embed_enabled=0, learning_synthesize_enabled=0);"
                " drain thread not started");
      return;
   }

   ctx->stop = 0;
   if (pthread_create(&ctx->thread, NULL, drain_thread_main, ctx) == 0)
   {
      ctx->active = 1;
      aimee_log(LOG_INFO, "kb.curator.drain", "drain thread started");
   }
   else
   {
      aimee_log(LOG_WARN, "kb.curator.drain", "failed to start drain thread");
   }
}

void kb_curator_drain_shutdown(kb_curator_drain_ctx_t *ctx)
{
   if (!ctx || !ctx->active)
      return;
   ctx->stop = 1;
   pthread_join(ctx->thread, NULL);
   ctx->active = 0;
   aimee_log(LOG_DEBUG, "kb.curator.drain", "drain thread stopped");
}
