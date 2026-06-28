/* kb_curator_drain.c: curator drain thread — polls for pending extract_doc
 * jobs and calls kb_curator_extract_one() to process each, draining the backlog
 * continuously and then idling (no artificial rate cap; §5 of curator-llm-
 * backend). The same thread also drains the evidence_index_ops embed queue
 * (kb_evidence_embed_drain), independent of the curator extract gates.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "db2/db2.h"
#include "db2/cross_repo_identity.h" /* db2_cross_repo_rebuild_identities (H0c) */
#include "db2/cross_repo_route.h"    /* db2_cross_repo_rebuild_routes (H0d) */
#include "db2/cross_repo_build.h"    /* db2_cross_repo_rebuild_build_deps (recall R2) */
#include "db2/cross_repo_stats.h"    /* db2_cross_repo_recompute_blocked_symbols */
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
#include "kb_memory_facts.h"
#include "kb_learning_synth.h"
#include "kb_service_code_embed.h"
#include "kb_service_graph.h" /* kb_graph_build_project_if_changed */
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
/* Max curator jobs (extract/synth/…) drained back-to-back per poll. The per-poll
 * catch-up sweep above (code/doc embed + projection graph, O(projects)) is paid
 * ONCE per batch instead of before every job, so a large post-ingest backlog can't
 * starve the synth stages to one job per poll. Bounded so the catch-up + config
 * reload still get a turn each poll (both progress). */
#define CURATOR_DRAIN_BATCH 16

/* Rebuild the corpus-derived cross-repo precision metadata (precision-hardening
 * H0/H1): the H0c repo-identity index, the H0d inter-repo structural-route
 * adjacency, and the distinctiveness blocked-symbols model. In dependency order:
 * routes are keyed on identities, so a failed identity rebuild must NOT proceed
 * to routes (it would key them on stale/partial identities). Returns 0 on a
 * completed rebuild, -1 if the store is not ready / a sub-rebuild failed (caller
 * retries next poll). Idempotent + set-based DB2; no embedder/LLM.
 *
 * Failure semantics — fail-to-last-known-good, NOT fail-open: each sub-rebuild
 * (rebuild_identities, rebuild_routes) is internally atomic (BEGIN/DELETE/INSERT/
 * COMMIT, ROLLBACK on error), and cross_repo_route rows store project NAMES (no FK
 * into the identity table). So on any failure path cross_repo_route remains a
 * complete, internally-consistent snapshot from the last SUCCESSFUL build — the
 * resolver's gate keeps using last-known-good routes (bounded staleness, self-
 * healed by the next successful rebuild), never a torn/partial table and never a
 * permissive no-route-accepts-all state (no-route demotes to LOW). A persistent
 * failure is surfaced via WARN so a wedged db2 / schema drift is observable. */
static int kb_cross_repo_meta_rebuild(const config_t *cfg)
{
   int ids = db2_cross_repo_rebuild_identities();
   if (ids < 0)
   {
      aimee_log(
          LOG_WARN, "kb.cross_repo.meta",
          "identity rebuild unavailable/failed; gate keeps last-known-good routes this cycle");
      return -1;
   }
   int routes = db2_cross_repo_rebuild_routes();
   if (routes < 0)
   {
      aimee_log(LOG_WARN, "kb.cross_repo.meta",
                "route rebuild failed; gate keeps last-known-good routes this cycle");
      return -1;
   }
   /* Recall R2: build-declared deps (FetchContent/submodule/Cargo) — a separate
    * evidence class the resolver merges as build_declared. Same fail-to-last-known-
    * good semantics. */
   int bdeps = db2_cross_repo_rebuild_build_deps();
   if (bdeps < 0)
   {
      aimee_log(LOG_WARN, "kb.cross_repo.meta",
                "build-dep rebuild failed; keeps last-known-good build deps this cycle");
      return -1;
   }
   int bsym = db2_cross_repo_recompute_blocked_symbols(cfg->kb_curator_cross_repo_k,
                                                       cfg->kb_curator_cross_repo_m,
                                                       cfg->kb_curator_cross_repo_len_min);
   aimee_log(
       LOG_INFO, "kb.cross_repo.meta",
       "rebuilt cross-repo metadata: identities=%d routes=%d build_deps=%d blocked_symbols=%d", ids,
       routes, bdeps, bsym);
   return 0;
}

static void *drain_thread_main(void *arg)
{
   kb_curator_drain_ctx_t *ctx = (kb_curator_drain_ctx_t *)arg;

   config_t cfg;
   config_load(&cfg);

   /* Cold-start backfill: a quiescent / already-indexed corpus never produces a
    * built>0 event, so the incremental rebuild below would never fire and
    * cross_repo_route would stay empty — silently demoting every legitimate
    * non-LOW cross-repo edge to LOW indefinitely. Run the rebuild once at startup,
    * independent of corpus change, so routes exist before the resolver is queried.
    * Retried until it succeeds once (db2 may not be open on the first iteration),
    * then never again (cleared on the first 0-return); self-heals within one poll
    * of the store coming up. The retry is naturally rate-limited to one attempt
    * per DRAIN_POLL_SECS, and each attempt is cheap on a wedged store (db2_conn()
    * returns NULL fast -> -1). */
   int cross_repo_cold_start_pending = 1;

   /* No artificial rate cap (§5): the curator drains the backlog continuously and
    * then idles. The natural throttle is backend throughput; cost control for a
    * paid provider lives at the provider (endpoint choice / its own limits). */

   long last_cfg_reload = (long)time(NULL);

   while (!ctx->stop)
   {
      /* Return any pool connection acquired last cycle before sleeping, so this
       * long-lived thread doesn't pin one while idle (stuck-lease reaper). */
      db2_lease_release_idle();
      sleep(DRAIN_POLL_SECS);
      if (ctx->stop)
         break;

      long now = (long)time(NULL);

      /* Reload config periodically */
      if (now - last_cfg_reload >= 300)
      {
         config_load(&cfg);
         last_cfg_reload = now;
      }

      /* Cold-start cross-repo metadata backfill (see cross_repo_cold_start_pending
       * above): once, as soon as the store is ready, so a quiescent corpus's
       * routes/identities exist before the structural-edge gate queries them.
       * Cleared only on a successful (0-return) rebuild, so a failed attempt stays
       * pending and retries — the one-shot guarantee is the flag-clear here. */
      if (cross_repo_cold_start_pending && cfg.kb_curator_cross_repo_graph_enabled &&
          kb_cross_repo_meta_rebuild(&cfg) == 0)
         cross_repo_cold_start_pending = 0;

      /* Evidence-vector embed drain — runs every poll, independent of the
       * curator extract gates (it fills evidence_vectors for the neighbourhood
       * builder; the builtin embedder needs no external sidecar). */
      if (cfg.kb_evidence_embed_enabled)
      {
         const char *embed_cmd = config_embedding_command(&cfg, NULL);
         int n = kb_evidence_embed_drain(cfg.kb_evidence_embed_batch, embed_cmd);
         if (n > 0)
            aimee_log(LOG_DEBUG, "kb.evidence.embed", "drained %d evidence op(s)", n);
      }

      /* Typed-fact extraction drain — runs every poll, independent of the
       * curator extract gates. Pulls "memory_facts" jobs enqueued by memory.store
       * and runs the general LLM extractor over each memory's content. Gated on
       * typed_facts_enabled (a no-op when off). */
      if (cfg.typed_facts_enabled)
      {
         int n = kb_memory_facts_drain(&cfg, 8);
         if (n > 0)
            aimee_log(LOG_DEBUG, "kb.memory.facts", "drained %d memory_facts job(s)", n);
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
         /* §2c: a dim-change re-embed clears its `maintenance` marker once the
          * doc-embed backfill has caught up — a pass that embedded nothing means
          * every chunk now has a vector at the new dim (the doc corpus reconciled).
          * The TTL->degraded fallback (health) covers a backfill that never drains. */
         if (total == 0 && db2_reembed_in_progress_get(NULL, NULL) == 1)
         {
            db2_reembed_in_progress_clear();
            aimee_log(LOG_INFO, "kb.reembed",
                      "dim-change re-embed reconciled (doc corpus); cleared maintenance");
         }
      }

      /* Code projection-graph drain — publish a fresh typed-edge generation per
       * CHANGED project (content-addressed: an unchanged project is skipped), so
       * `workspace add` materializes the code_projection_edges layer with no manual
       * `aimee graph sync-code`. Pure DB2 (reads files/terms/code_calls), no
       * embedder/LLM. Gated on its own flag (default on); independent of the curator
       * pipeline below. */
      if (cfg.kb_curator_projection_graph_enabled)
      {
         project_info_t projects[128];
         int np = index_list_projects(projects, 128);
         int built = 0;
         int64_t total = 0;
         for (int i = 0; i < np; i++)
         {
            int rebuilt = 0;
            int64_t edges = kb_graph_build_project_if_changed(projects[i].name, &rebuilt);
            if (rebuilt)
            {
               built++;
               if (edges > 0)
                  total += edges;
            }
         }
         if (built > 0)
            aimee_log(LOG_INFO, "kb.graph.projection",
                      "published %lld edge(s) across %d changed project(s)", (long long)total,
                      built);

         /* Incremental cross-repo metadata refresh: a project changed this cycle
          * (the content-addressed `built` signal), so rebuild the H0c identities,
          * H0d routes, and the distinctiveness model to keep the structural-edge
          * gate fresh without a manual recompute. Idempotent; on a multi-poll
          * catch-up the rebuild repeats once per poll-with-changes (set-based, no
          * LLM — correctness-safe; coalescing to one rebuild at quiescence is a
          * possible future optimization). The flag here also gates the resolver
          * (cross_repo_deps.c) and the cold-start backfill above: disabling it
          * turns off precision gating AND route population together. */
         if (built > 0 && cfg.kb_curator_cross_repo_graph_enabled)
            kb_cross_repo_meta_rebuild(&cfg);
      }

      /* Candidate-generation synthesis drain — the heavy LLM pass, on the
       * scheduler, never on the capture hot path. Off by default. */
      if (cfg.learning_synthesize_enabled && cfg.learning_synthesize_command[0])
      {
         const char *embed_cmd = config_embedding_command(&cfg, NULL);
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

      kb_curator_extract_opts_t opts;
      memset(&opts, 0, sizeof(opts));
      snprintf(opts.extract_command, sizeof(opts.extract_command), "%s",
               cfg.kb_curator_extract_command);
      opts.max_tokens =
          cfg.kb_curator_extract_max_tokens > 0 ? cfg.kb_curator_extract_max_tokens : 2048;
      opts.max_attempts = cfg.kb_curator_max_attempts > 0 ? cfg.kb_curator_max_attempts : 3;

      /* Drain a batch of curator jobs back-to-back. Each iteration runs ONE job
       * from the highest-priority non-empty stage (the rc chain below). Draining a
       * batch per poll — rather than one job then re-running the catch-up sweep +
       * sleep above — amortizes that sweep across the batch so synth keeps pace
       * with throughput instead of paying the O(projects) sweep before every job.
       * The top-of-loop sleep provides the idle/error backoff. */
      int drained = 0;
      while (!ctx->stop && drained < CURATOR_DRAIN_BATCH)
      {
         /* Return the thread's pooled connection between jobs so a long batch
          * doesn't pin one past the stuck-lease ceiling (it's re-acquired lazily
          * by the next stage); cheap no-op when nothing is held. */
         db2_lease_release_idle();
         int rc = 0;
         if (cfg.kb_curator_extract_docs_enabled)
         {
            kb_background_set("curator", "extract docs");
            rc = kb_curator_extract_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_extract_code_enabled)
         {
            kb_background_set("curator", "extract code");
            rc = kb_curator_extract_code_unit_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_resolve_entities_enabled)
         {
            kb_background_set("curator", "resolve entities");
            rc = kb_curator_resolve_entities_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_index_narrative_enabled)
         {
            kb_background_set("curator", "index narrative");
            rc = kb_curator_index_narrative_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_index_claims_enabled)
         {
            kb_background_set("curator", "index claims");
            rc = kb_curator_index_claims_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_detect_contradictions_enabled)
         {
            kb_background_set("curator", "detect contradictions");
            rc = kb_curator_detect_contradictions_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_index_code_unit_enabled)
         {
            kb_background_set("curator", "index code_unit");
            rc = kb_curator_index_code_unit_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_link_artifacts_enabled)
         {
            kb_background_set("curator", "link artifacts");
            rc = kb_curator_link_artifacts_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_synthesize_enabled)
         {
            kb_background_set("curator", "synthesize topic");
            rc = kb_curator_synthesize_one(&opts);
         }
         if (rc == 0 && cfg.kb_curator_promote_entity_enabled)
         {
            kb_background_set("curator", "promote entity");
            rc = kb_curator_promote_entity_one(&opts);
         }

         if (rc == 1)
         {
            drained++; /* job processed — drain the next one without re-sweeping */
            continue;
         }
         if (rc < 0)
            /* Stage error: stop the batch; the top-of-loop sleep backs off before
             * the next poll so a persistently-failing stage can't spin hot. */
            aimee_log(LOG_WARN, "kb.curator.drain", "extractor returned error; backing off");
         break; /* rc==0 (all queues empty) or rc<0 (error): end this poll's batch */
      }
      if (drained > 0)
         aimee_log(LOG_DEBUG, "kb.curator.drain", "drained %d job(s) this poll", drained);
      kb_background_clear("curator");
   }

   kb_background_clear("curator");
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
       !cfg.kb_curator_projection_graph_enabled && !cfg.kb_evidence_embed_enabled &&
       !cfg.learning_synthesize_enabled && !cfg.typed_facts_enabled)
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
