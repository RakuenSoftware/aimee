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
#include "db2/decision_log.h"        /* db2_decision_log_mark_revisit_due (P1) */
#include "db2/cross_repo_identity.h" /* db2_cross_repo_rebuild_identities (H0c) */
#include "db2/cross_repo_route.h"    /* db2_cross_repo_rebuild_routes (H0d) */
#include "db2/cross_repo_build.h"    /* db2_cross_repo_rebuild_build_deps (recall R2) */
#include "db2/cross_repo_stats.h"    /* db2_cross_repo_recompute_blocked_symbols */
#include "db2/ontology_evolution.h"  /* db2_ontology_eval_candidates/_approve (§7.2 auto-promote) */
#include "kb_curator_drain.h"
#include "kb_curator_extract.h"
#include "kb_curator_resolve_entities.h"
#include "kb_curator_index_narrative.h"
#include "kb_curator_index_claims.h"
#include "kb_curator_contradictions.h"
#include "kb_curator_queue.h"
#include "kb_curator_index_code_unit.h"
#include "kb_curator_link_artifacts.h"
#include "kb_curator_synthesize.h"
#include "kb_curator_promote.h"
#include "kb_curator_custom.h"
#include "kb_curator_pipeline.h"
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
#include "cJSON.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DRAIN_POLL_SECS 5
/* Index lane (CPU) idle backoff. It loops hot while a backlog remains, so this only
 * paces polling once its queues are empty. */
#define INDEX_LANE_POLL_SECS 2
/* Max synthesis ops processed per poll — each is an LLM round-trip. */
#define SYNTH_DRAIN_BATCH 4
/* Max curator jobs (extract/synth/…) drained back-to-back per poll. The per-poll
 * catch-up sweep above (code/doc embed + projection graph, O(projects)) is paid
 * ONCE per batch instead of before every job, so a large post-ingest backlog can't
 * starve the synth stages to one job per poll. Bounded so the catch-up + config
 * reload still get a turn each poll (both progress). */
#define CURATOR_DRAIN_BATCH 16

/* §7.2 autonomous ontology promotion: a provisional relation is auto-promoted to
 * active once it has recurred at least this many times (default; operator override
 * kb.typed_facts.promote_threshold). BATCH bounds promotions per poll. */
#define KB_ONTO_PROMOTE_DEFAULT_THRESHOLD 3
#define KB_ONTO_PROMOTE_BATCH             32

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

/* Projection-graph sweep (INDEX/CPU lane): publish a fresh typed-edge generation
 * per CHANGED project (content-addressed -- an unchanged project is skipped), so
 * `workspace add` materializes the code_projection_edges layer with no manual
 * `aimee graph sync-code`. Pure DB2 (reads files/terms/code_calls), no embedder/
 * LLM -- so it runs on the CPU/index lane, off the GPU/LLM thread. When a project
 * changed this cycle (the content-addressed `built` signal), it also does the
 * incremental cross-repo metadata refresh (H0c identities, H0d routes, build_deps,
 * distinctiveness model) to keep the structural-edge gate fresh. Idempotent and
 * cheap when nothing changed. Gated on kb_curator_projection_graph_enabled
 * (default on); cross_repo refresh additionally gated on
 * kb_curator_cross_repo_graph_enabled. */
static void kb_curator_projection_sweep(const config_t *cfg)
{
   if (!cfg->kb_curator_projection_graph_enabled)
      return;
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
                "published %lld edge(s) across %d changed project(s)", (long long)total, built);
   if (built > 0 && cfg->kb_curator_cross_repo_graph_enabled)
      kb_cross_repo_meta_rebuild(cfg);
}

/* Curator pipeline registry: the ordered stages the drain flows work through,
 * replacing the old hardcoded if-chain that starved downstream stages behind the
 * extract backlog. Each stage has a per-pass budget and a resource lane -- LLM
 * (GPU) stages run one unit/pass; INDEX (CPU embed/SQL) stages drain their queue
 * each pass so a freshly-extracted document's claims are indexed + contradiction-
 * checked promptly. Data-driven so a GUI can toggle/reorder and per-lane workers
 * can each drain one lane. */
static int en_extract_docs(const config_t *c)
{
   return c->kb_curator_extract_docs_enabled;
}
static int en_extract_code(const config_t *c)
{
   return c->kb_curator_extract_code_enabled;
}
static int en_resolve_entities(const config_t *c)
{
   return c->kb_curator_resolve_entities_enabled;
}
static int en_index_narrative(const config_t *c)
{
   return c->kb_curator_index_narrative_enabled;
}
static int en_index_claims(const config_t *c)
{
   return c->kb_curator_index_claims_enabled;
}
static int en_detect_contradictions(const config_t *c)
{
   return c->kb_curator_detect_contradictions_enabled;
}
static int en_index_code_unit(const config_t *c)
{
   return c->kb_curator_index_code_unit_enabled;
}
static int en_link_artifacts(const config_t *c)
{
   return c->kb_curator_link_artifacts_enabled;
}
static int en_synthesize(const config_t *c)
{
   return c->kb_curator_synthesize_enabled;
}
static int en_promote_entity(const config_t *c)
{
   return c->kb_curator_promote_entity_enabled;
}

static void curator_set_status(const char *label)
{
   kb_background_set("curator", label);
}

static void curator_index_set_status(const char *label)
{
   kb_background_set("curator.index", label);
}

/* -- Phase 3: per-project CPU sweeps as INDEX-lane stages --------------------
 * These wrap the embed/ingest/graph drains that previously ran inline in the main
 * (LLM) thread's poll loop. As INDEX-lane registry stages they run on the CPU
 * index-lane worker -- concurrent with GPU extraction -- and are toggleable like
 * any other stage. Each config_loads for the embedder command + project list (as
 * kb_curator_queue_docs_for_project does) and returns 1=did work / 0=idle. */
static int en_embedder(const config_t *c)
{
   return c->embedding_command[0] != '\0';
}
static int en_evidence_embed(const config_t *c)
{
   return c->kb_evidence_embed_enabled;
}

static int stage_embed_code(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   config_t cfg;
   config_load(&cfg);
   if (!cfg.embedding_command[0])
      return 0;
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
      aimee_log(LOG_DEBUG, "kb.code.embed", "embedded %d code/doc vector(s) across %d project(s)",
                total, np);
   return total > 0 ? 1 : 0;
}

static int stage_ingest_docs(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   config_t cfg;
   config_load(&cfg);
   if (!cfg.embedding_command[0])
      return 0;
   project_info_t projects[128];
   int np = index_list_projects(projects, 128);
   int total = 0;
   for (int i = 0; i < np; i++)
   {
      int e = kb_doc_refresh(projects[i].name, cfg.embedding_command, 200);
      if (e > 0)
         total += e;
      int b = kb_doc_embed_backfill(projects[i].name, cfg.embedding_command, 200);
      if (b > 0)
         total += b;
   }
   if (total > 0)
      aimee_log(LOG_DEBUG, "kb.docs.ingest", "ingested %d doc chunk(s) across %d project(s)", total,
                np);
   /* dim-change re-embed clears its maintenance marker once the backfill has caught
    * up (a pass that embedded nothing means every chunk has a vector). */
   if (total == 0 && db2_reembed_in_progress_get(NULL, NULL) == 1)
   {
      db2_reembed_in_progress_clear();
      aimee_log(LOG_INFO, "kb.reembed",
                "dim-change re-embed reconciled (doc corpus); cleared maintenance");
   }
   return total > 0 ? 1 : 0;
}

static int stage_embed_evidence(const kb_curator_extract_opts_t *opts)
{
   (void)opts;
   config_t cfg;
   config_load(&cfg);
   const char *embed_cmd = config_embedding_command(&cfg, NULL);
   int n = kb_evidence_embed_drain(cfg.kb_evidence_embed_batch, embed_cmd);
   if (n > 0)
      aimee_log(LOG_DEBUG, "kb.evidence.embed", "drained %d evidence op(s)", n);
   return n > 0 ? 1 : 0;
}

static const kb_curator_stage_desc_t CURATOR_STAGES[] = {
    {"extract_docs", "extract docs", en_extract_docs, kb_curator_extract_one, 1,
     KB_CURATOR_LANE_LLM},
    {"extract_code", "extract code", en_extract_code, kb_curator_extract_code_unit_one, 1,
     KB_CURATOR_LANE_LLM},
    {"resolve_entities", "resolve entities", en_resolve_entities, kb_curator_resolve_entities_one,
     1, KB_CURATOR_LANE_LLM},
    {"index_narrative", "index narrative", en_index_narrative, kb_curator_index_narrative_one, 32,
     KB_CURATOR_LANE_INDEX},
    {"index_claims", "index claims", en_index_claims, kb_curator_index_claims_one, 64,
     KB_CURATOR_LANE_INDEX},
    {"detect_contradictions", "detect contradictions", en_detect_contradictions,
     kb_curator_detect_contradictions_one, 1, KB_CURATOR_LANE_INDEX},
    {"index_code_unit", "index code_unit", en_index_code_unit, kb_curator_index_code_unit_one, 32,
     KB_CURATOR_LANE_INDEX},
    {"link_artifacts", "link artifacts", en_link_artifacts, kb_curator_link_artifacts_one, 8,
     KB_CURATOR_LANE_INDEX},
    {"synthesize", "synthesize topic", en_synthesize, kb_curator_synthesize_one, 1,
     KB_CURATOR_LANE_LLM},
    {"promote_entity", "promote entity", en_promote_entity, kb_curator_promote_entity_one, 1,
     KB_CURATOR_LANE_LLM},
    {"embed_code", "embed code", en_embedder, stage_embed_code, 1, KB_CURATOR_LANE_INDEX},
    {"ingest_docs", "ingest docs", en_embedder, stage_ingest_docs, 1, KB_CURATOR_LANE_INDEX},
    {"embed_evidence", "embed evidence", en_evidence_embed, stage_embed_evidence, 1,
     KB_CURATOR_LANE_INDEX},
};

/* Resolve a built-in stage by name for the composed-stage loader (Phase D): a
 * custom stage's base_op must name one of these vetted run()-backed ops. Returns
 * the registry descriptor or NULL. */
static const kb_curator_stage_desc_t *kb_curator_base_resolve(const char *name)
{
   if (!name)
      return NULL;
   for (size_t i = 0; i < sizeof(CURATOR_STAGES) / sizeof(CURATOR_STAGES[0]); i++)
      if (strcmp(CURATOR_STAGES[i].name, name) == 0)
         return &CURATOR_STAGES[i];
   return NULL;
}

/* Producer->consumer dependency DAG for the curator stages: a stage may not be
 * ordered before a prerequisite in the SAME lane (cross-lane prereqs are advisory
 * -- the two lanes run concurrently, so cross-lane order is not enforceable). The
 * reorder validator enforces the same-lane edges; the GUI surfaces the full list
 * via `requires`. Single source of truth for the constraints. See
 * docs/proposals/pending/user-configurable-curator-pipeline.md. */
static const struct
{
   const char *stage;
   const char *reqs; /* comma-separated prerequisite stage names */
} CURATOR_STAGE_DEPS[] = {
    {"resolve_entities", "extract_docs"}, {"index_narrative", "extract_docs"},
    {"index_claims", "extract_docs"},     {"detect_contradictions", "index_claims"},
    {"index_code_unit", "extract_code"},  {"link_artifacts", "extract_docs,extract_code"},
    {"synthesize", "index_claims"},       {"promote_entity", "resolve_entities"},
    {"embed_evidence", "index_claims"},   {"cross_repo_graph", "projection_graph"},
};

/* Prerequisite string for a stage (comma-separated), or "" if none. */
static const char *kb_curator_stage_reqs(const char *name)
{
   for (size_t i = 0; i < sizeof(CURATOR_STAGE_DEPS) / sizeof(CURATOR_STAGE_DEPS[0]); i++)
      if (strcmp(CURATOR_STAGE_DEPS[i].stage, name) == 0)
         return CURATOR_STAGE_DEPS[i].reqs;
   return "";
}

/* Add a "requires":[...] array (from the DAG) to a stage JSON object. */
static void kb_curator_add_requires(cJSON *o, const char *name)
{
   cJSON *reqs = cJSON_CreateArray();
   char buf[128];
   snprintf(buf, sizeof(buf), "%s", kb_curator_stage_reqs(name));
   char *save = NULL;
   for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save))
      cJSON_AddItemToArray(reqs, cJSON_CreateString(t));
   cJSON_AddItemToObject(o, "requires", reqs);
}

/* Option B (single source of truth): expose the stage registry as data so the
 * Pipeline GUI renders from the running backend instead of a hand-kept mirror.
 * Returns a fresh cJSON array [{name,label,lane,budget,order,config_key}]; the
 * caller owns it. `config_key` is the aimee.yaml flag the GUI toggles via
 * config.set: embed_code/ingest_docs are gated on the embedder command (no
 * individual flag) so they report null (read-only); embed_evidence uses
 * kb_evidence_embed_enabled; every other stage uses kb_curator_<name>_enabled
 * (the en_* predicate convention above). */
cJSON *kb_curator_stages_json(void)
{
   cJSON *arr = cJSON_CreateArray();
   size_t n = sizeof(CURATOR_STAGES) / sizeof(CURATOR_STAGES[0]);
   for (size_t i = 0; i < n; i++)
   {
      const kb_curator_stage_desc_t *st = &CURATOR_STAGES[i];
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "name", st->name);
      cJSON_AddStringToObject(o, "label", st->label);
      cJSON_AddStringToObject(o, "lane", st->lane == KB_CURATOR_LANE_LLM ? "llm" : "index");
      cJSON_AddNumberToObject(o, "budget", st->budget);
      cJSON_AddNumberToObject(o, "order", (double)i);
      if (strcmp(st->name, "embed_code") == 0 || strcmp(st->name, "ingest_docs") == 0)
         cJSON_AddNullToObject(o, "config_key");
      else if (strcmp(st->name, "embed_evidence") == 0)
         cJSON_AddStringToObject(o, "config_key", "kb_evidence_embed_enabled");
      else
      {
         char key[96];
         snprintf(key, sizeof(key), "kb_curator_%s_enabled", st->name);
         cJSON_AddStringToObject(o, "config_key", key);
      }
      kb_curator_add_requires(o, st->name);
      cJSON_AddItemToArray(arr, o);
   }
   /* The projection-graph + cross-repo refresh run on the INDEX lane via
    * kb_curator_projection_sweep (batch sweeps, not per-item CURATOR_STAGES
    * entries) -- but they ARE user-togglable pipeline steps, so surface them for
    * the GUI too, after the registry stages. */
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "name", "projection_graph");
      cJSON_AddStringToObject(o, "label", "projection graph");
      cJSON_AddStringToObject(o, "lane", "index");
      cJSON_AddNumberToObject(o, "budget", 1);
      cJSON_AddNumberToObject(o, "order", (double)n);
      cJSON_AddStringToObject(o, "config_key", "kb_curator_projection_graph_enabled");
      kb_curator_add_requires(o, "projection_graph");
      cJSON_AddItemToArray(arr, o);
   }
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "name", "cross_repo_graph");
      cJSON_AddStringToObject(o, "label", "cross-repo graph");
      cJSON_AddStringToObject(o, "lane", "index");
      cJSON_AddNumberToObject(o, "budget", 1);
      cJSON_AddNumberToObject(o, "order", (double)(n + 1));
      cJSON_AddStringToObject(o, "config_key", "kb_curator_cross_repo_graph_enabled");
      kb_curator_add_requires(o, "cross_repo_graph");
      cJSON_AddItemToArray(arr, o);
   }
   /* Composed custom stages (Phase D): the validated entries from
    * kb.curator.custom_stages, appended after the built-ins so the GUI can render
    * them. Marked custom:true with their base_op; config_key is null (they are
    * edited via the custom_stages JSON, not a flat flag). `requires` is inherited
    * from the base op. enabled:false entries are surfaced (deletable/toggleable in
    * the GUI) but not composed into the runner. Reuses the same validating parser
    * as the runner, so only well-formed entries appear. */
   {
      config_t cfg;
      config_load(&cfg);
      if (cfg.kb_curator_custom_stages[0])
      {
         kb_curator_custom_t cbuf[KB_CURATOR_MAX_CUSTOM];
         size_t nc =
             kb_curator_custom_stages_parse(cfg.kb_curator_custom_stages, kb_curator_base_resolve,
                                            cbuf, KB_CURATOR_MAX_CUSTOM, NULL);
         for (size_t i = 0; i < nc; i++)
         {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", cbuf[i].name);
            cJSON_AddStringToObject(o, "label", cbuf[i].name);
            cJSON_AddStringToObject(o, "lane",
                                    cbuf[i].base->lane == KB_CURATOR_LANE_LLM ? "llm" : "index");
            cJSON_AddNumberToObject(o, "budget", cbuf[i].budget);
            cJSON_AddNumberToObject(o, "order", (double)(n + 2 + i));
            cJSON_AddNullToObject(o, "config_key");
            cJSON_AddBoolToObject(o, "custom", 1);
            cJSON_AddStringToObject(o, "base_op", cbuf[i].base_op);
            cJSON_AddBoolToObject(o, "enabled", cbuf[i].enabled ? 1 : 0);
            kb_curator_add_requires(o, cbuf[i].base_op);
            cJSON_AddItemToArray(arr, o);
         }
      }
   }
   return arr;
}

/* Built-in curator presets (v1): named enabled-sets of stage config keys. The GUI
 * applies a preset by enabling the listed keys and disabling the rest. Backend-
 * defined so they stay in step with the stage set (single source of truth); the
 * user-defined/saved presets the product owner wants are a planned follow-up that
 * will extend this list from stored config rather than replace it. Returns a fresh
 * cJSON array [{name,description,enabled:[config_key,...]}]; caller owns it. */
cJSON *kb_curator_presets_json(void)
{
   static const struct
   {
      const char *name;
      const char *desc;
      const char *keys; /* comma-separated config keys enabled by this preset */
   } PRESETS[] = {
       {"full", "Every curated stage — docs, code, contradictions, and graph.",
        "kb_curator_extract_docs_enabled,kb_curator_extract_code_enabled,"
        "kb_curator_resolve_entities_enabled,kb_curator_index_narrative_enabled,"
        "kb_curator_index_claims_enabled,kb_curator_detect_contradictions_enabled,"
        "kb_curator_index_code_unit_enabled,kb_curator_link_artifacts_enabled,"
        "kb_curator_synthesize_enabled,kb_curator_promote_entity_enabled,"
        "kb_evidence_embed_enabled,kb_curator_projection_graph_enabled,"
        "kb_curator_cross_repo_graph_enabled"},
       {"docs-only",
        "Document knowledge: extract, resolve, index, detect contradictions, synthesize.",
        "kb_curator_extract_docs_enabled,kb_curator_resolve_entities_enabled,"
        "kb_curator_index_narrative_enabled,kb_curator_index_claims_enabled,"
        "kb_curator_detect_contradictions_enabled,kb_curator_synthesize_enabled,"
        "kb_curator_promote_entity_enabled,kb_evidence_embed_enabled"},
       {"code-only",
        "Code knowledge: extract code units, index, link, projection + cross-repo graph.",
        "kb_curator_extract_code_enabled,kb_curator_index_code_unit_enabled,"
        "kb_curator_link_artifacts_enabled,kb_curator_projection_graph_enabled,"
        "kb_curator_cross_repo_graph_enabled"},
   };
   cJSON *arr = cJSON_CreateArray();
   for (size_t i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++)
   {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "name", PRESETS[i].name);
      cJSON_AddStringToObject(o, "description", PRESETS[i].desc);
      cJSON *keys = cJSON_CreateArray();
      char buf[1024];
      snprintf(buf, sizeof(buf), "%s", PRESETS[i].keys);
      char *save = NULL;
      for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
         cJSON_AddItemToArray(keys, cJSON_CreateString(tok));
      cJSON_AddItemToObject(o, "enabled", keys);
      cJSON_AddBoolToObject(o, "builtin", 1);
      cJSON_AddItemToArray(arr, o);
   }
   /* Append user-defined presets from kb.curator.user_presets (a JSON array string
    * the GUI edits via config.set). Marked builtin:false so the GUI shows them as
    * deletable. Malformed entries are skipped; capped so a bad config can't blow up
    * the response. */
   config_t cfg;
   config_load(&cfg);
   if (cfg.kb_curator_user_presets[0])
   {
      cJSON *ups = cJSON_Parse(cfg.kb_curator_user_presets);
      if (ups && cJSON_IsArray(ups))
      {
         int count = 0;
         cJSON *up = NULL;
         cJSON_ArrayForEach(up, ups)
         {
            if (count >= 64)
               break;
            if (!cJSON_IsObject(up))
               continue;
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(up, "name");
            cJSON *en = cJSON_GetObjectItemCaseSensitive(up, "enabled");
            if (!cJSON_IsString(nm) || !nm->valuestring || !cJSON_IsArray(en))
               continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", nm->valuestring);
            cJSON *dsc = cJSON_GetObjectItemCaseSensitive(up, "description");
            cJSON_AddStringToObject(o, "description",
                                    (cJSON_IsString(dsc) && dsc->valuestring) ? dsc->valuestring
                                                                              : "User preset");
            cJSON *keys = cJSON_CreateArray();
            cJSON *k = NULL;
            cJSON_ArrayForEach(k, en) if (cJSON_IsString(k) && k->valuestring)
                cJSON_AddItemToArray(keys, cJSON_CreateString(k->valuestring));
            cJSON_AddItemToObject(o, "enabled", keys);
            cJSON_AddBoolToObject(o, "builtin", 0);
            cJSON_AddItemToArray(arr, o);
            count++;
         }
      }
      if (ups)
         cJSON_Delete(ups);
   }
   return arr;
}

/* Lane of a registry stage by name, or -1 if not a CURATOR_STAGES entry. */
static int kb_curator_stage_lane_of(const char *name)
{
   for (size_t i = 0; i < sizeof(CURATOR_STAGES) / sizeof(CURATOR_STAGES[0]); i++)
      if (strcmp(CURATOR_STAGES[i].name, name) == 0)
         return (int)CURATOR_STAGES[i].lane;
   return -1;
}

/* Validate a comma-separated stage order against the same-lane dependency DAG:
 * every listed stage must appear after its same-lane prerequisites, and those
 * prerequisites must themselves be listed (an omitted prereq would fall to the end
 * in registry order, i.e. after its dependent). Cross-lane prereqs are advisory
 * (the lanes run concurrently) and not checked. Unknown names are ignored. Returns
 * 1 valid, 0 invalid (fills `err`). */
static int kb_curator_order_valid(const char *order, char *err, size_t err_sz)
{
   if (err && err_sz)
      err[0] = '\0';
   if (!order || !order[0])
      return 1;
   char buf[512];
   snprintf(buf, sizeof(buf), "%s", order);
   const char *names[64];
   int npos = 0;
   char *save = NULL;
   for (char *t = strtok_r(buf, ",", &save); t && npos < 64; t = strtok_r(NULL, ",", &save))
   {
      while (*t == ' ')
         t++;
      names[npos++] = t;
   }
   for (int i = 0; i < npos; i++)
   {
      int lane = kb_curator_stage_lane_of(names[i]);
      if (lane < 0)
         continue;
      char rbuf[128];
      snprintf(rbuf, sizeof(rbuf), "%s", kb_curator_stage_reqs(names[i]));
      char *rsave = NULL;
      for (char *req = strtok_r(rbuf, ",", &rsave); req; req = strtok_r(NULL, ",", &rsave))
      {
         if (kb_curator_stage_lane_of(req) != lane)
            continue; /* cross-lane: advisory only */
         int rpos = -1;
         for (int j = 0; j < npos; j++)
            if (strcmp(names[j], req) == 0)
            {
               rpos = j;
               break;
            }
         if (rpos < 0 || rpos > i)
         {
            if (err && err_sz)
               snprintf(err, err_sz, "stage '%s' must come after prerequisite '%s' (same lane)",
                        names[i], req);
            return 0;
         }
      }
   }
   return 1;
}

/* Append enabled composed (custom) stages after the built-ins (Phase D). Parses
 * kb.curator.custom_stages, reuses each valid entry's base op run fn under the
 * custom name/budget on the base op's native lane, and appends to out[]. `cbuf`
 * (capacity cbuf_n) backs the custom name storage, so it must outlive out[]'s use.
 * Fail-safe: invalid entries are skipped with one WARN, mirroring stage_order. */
static size_t kb_curator_append_custom(const config_t *cfg, kb_curator_stage_desc_t *out, size_t k,
                                       size_t max, kb_curator_custom_t *cbuf, size_t cbuf_n)
{
   if (!cbuf || cbuf_n == 0 || !cfg->kb_curator_custom_stages[0])
      return k;
   int nrej = 0;
   size_t nc = kb_curator_custom_stages_parse(cfg->kb_curator_custom_stages,
                                              kb_curator_base_resolve, cbuf, cbuf_n, &nrej);
   if (nrej > 0)
      aimee_log(LOG_WARN, "kb.curator.custom",
                "skipped %d custom_stages entr%s (unknown base_op / bad name / re-lane / dup / bad "
                "budget / over cap); using the rest",
                nrej, nrej == 1 ? "y" : "ies");
   for (size_t i = 0; i < nc && k < max; i++)
   {
      if (!cbuf[i].enabled)
         continue; /* keep-but-disabled: surfaced on the endpoint, not composed */
      kb_curator_custom_to_desc(&cbuf[i], &out[k]);
      k++;
   }
   return k;
}

/* Build the stage array the lane workers iterate: kb_curator_stage_order when set
 * AND valid (listed stages first in that order, then any unlisted in registry
 * order), else the registry order; then any enabled composed custom stages
 * (Phase D) appended after the built-ins. Fail-safe: an invalid order logs one
 * WARN and falls back to registry order, so a bad config can never corrupt the
 * pipeline. Fills `out` (capacity `max`); `cbuf` (capacity `cbuf_n`) backs the
 * custom stages' name storage and must outlive `out`. Returns the count. */
static size_t kb_curator_ordered_stages(const config_t *cfg, kb_curator_stage_desc_t *out,
                                        size_t max, kb_curator_custom_t *cbuf, size_t cbuf_n)
{
   size_t n = sizeof(CURATOR_STAGES) / sizeof(CURATOR_STAGES[0]);
   size_t reg = n < max ? n : max;
   for (size_t i = 0; i < reg; i++)
      out[i] = CURATOR_STAGES[i];
   const char *order = cfg->kb_curator_stage_order;
   char err[160];
   if (!order[0] || !kb_curator_order_valid(order, err, sizeof(err)))
   {
      if (order[0])
         aimee_log(LOG_WARN, "kb.curator.order",
                   "ignoring invalid kb_curator_stage_order (%s); using registry order", err);
      return kb_curator_append_custom(cfg, out, reg, max, cbuf, cbuf_n);
   }
   /* `used` is indexed by built-in registry position; guard its width against the
    * built-in count so adding a 33rd stage can't silently overflow it. */
   _Static_assert(sizeof(CURATOR_STAGES) / sizeof(CURATOR_STAGES[0]) <= 32,
                  "used[] must cover every built-in curator stage");
   int used[32] = {0};
   size_t k = 0;
   char buf[512];
   snprintf(buf, sizeof(buf), "%s", order);
   char *save = NULL;
   for (char *t = strtok_r(buf, ",", &save); t && k < max; t = strtok_r(NULL, ",", &save))
   {
      while (*t == ' ')
         t++;
      for (size_t i = 0; i < n; i++)
         if (!used[i] && strcmp(CURATOR_STAGES[i].name, t) == 0)
         {
            out[k++] = CURATOR_STAGES[i];
            used[i] = 1;
            break;
         }
   }
   for (size_t i = 0; i < n && k < max; i++)
      if (!used[i])
         out[k++] = CURATOR_STAGES[i];
   return kb_curator_append_custom(cfg, out, k, max, cbuf, cbuf_n);
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

      /* Governance decision revisit sweep — runs every poll (P1). Flips active
       * decisions past their revisit_when to 'revisit_due' so they resurface.
       * Reuses this existing drain — no new scheduler. */
      {
         int due = db2_decision_log_mark_revisit_due();
         if (due > 0)
            aimee_log(LOG_DEBUG, "kb.decision.revisit", "flipped %d decision(s) to revisit_due",
                      due);
         else if (due < 0)
            aimee_log(LOG_WARN, "kb.decision.revisit",
                      "revisit sweep failed this cycle (db2 unavailable?)");
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

         /* §7.2 autonomous ontology reconciliation: promote provisional relations
          * (novel predicates the extractor's "other" fallback staged) to active
          * once they have recurred >= threshold times across sources, so facts
          * using them become durable/recallable WITHOUT a human approving each.
          * Default-on with typed facts; the threshold is operator-tunable via
          * kb.typed_facts.promote_threshold (§8), falling back to the built-in. */
         if (cfg.kb_typed_facts_auto_promote_enabled)
         {
            int thr = cfg.kb_typed_facts_promote_threshold > 0
                          ? cfg.kb_typed_facts_promote_threshold
                          : KB_ONTO_PROMOTE_DEFAULT_THRESHOLD;
            char cands[KB_ONTO_PROMOTE_BATCH][REL_TYPE_NAME_MAX];
            int nc = db2_ontology_eval_candidates(thr, cands, KB_ONTO_PROMOTE_BATCH);
            for (int i = 0; i < nc; i++)
            {
               /* Never promote a generic catch-all bucket to active: it would make
                * low-quality "misc" facts durable and can't be reconciled to a
                * real predicate. The extractor is instructed not to emit these,
                * but exclude them defensively. */
               if (strcmp(cands[i], "other") == 0 || strcmp(cands[i], "unknown") == 0 ||
                   strcmp(cands[i], "misc") == 0 || strcmp(cands[i], "unspecified") == 0)
                  continue;
               if (db2_ontology_approve(cands[i]) == 0)
                  aimee_log(LOG_INFO, "kb.ontology.promote",
                            "auto-promoted relation '%s' to active (>= %d observations)", cands[i],
                            thr);
            }
         }
      }

      /* Backfill: queue extract_doc curation for docs that entered kb_documents
       * via the drain (kb_doc_refresh) rather than the ingest-route hook -- else
       * drain-ingested docs are never curated. Idempotent, self-gated. */
      kb_curator_queue_docs_all_projects(cfg.kb_curator_extract_docs_enabled);

      /* Code projection-graph + incremental cross-repo metadata refresh moved to
       * the CPU/index lane (kb_curator_projection_sweep, driven by
       * kb_curator_index_lane_main) so this pure-DB2 O(projects) sweep drains
       * concurrently with -- rather than blocking -- the GPU/LLM pass here. */

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

      /* Stage order for this poll: kb_curator_stage_order when set + valid, else
       * registry order (fail-safe). Built once per poll so an invalid order WARNs
       * at most once, not once per drained job. */
      kb_curator_stage_desc_t ordered[16 + KB_CURATOR_MAX_CUSTOM];
      kb_curator_custom_t customs[KB_CURATOR_MAX_CUSTOM];
      size_t nordered = kb_curator_ordered_stages(
          &cfg, ordered, sizeof(ordered) / sizeof(ordered[0]), customs, KB_CURATOR_MAX_CUSTOM);

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
         /* This (main) worker drives the LLM lane; the CPU index lane runs on its own
          * thread (kb_curator_index_lane_main) so indexing does not wait behind each
          * multi-second extraction. */
         int r = kb_curator_pipeline_run_pass(ordered, nordered, KB_CURATOR_LANE_LLM, &cfg, &opts,
                                              curator_set_status);
         if (r < 0)
         {
            /* Stage error: stop the batch; the top-of-loop sleep backs off before
             * the next poll so a persistently-failing stage can't spin hot. */
            aimee_log(LOG_WARN, "kb.curator.drain", "curator stage returned error; backing off");
            break;
         }
         if (r == 1)
         {
            drained++; /* pass did work -- run another before re-sweeping */
            continue;
         }
         break; /* all stage queues empty */
      }
      if (drained > 0)
         aimee_log(LOG_DEBUG, "kb.curator.drain", "drained %d job(s) this poll", drained);
      kb_background_clear("curator");
   }

   kb_background_clear("curator");
   return NULL;
}

static void *kb_curator_index_lane_main(void *arg)
{
   kb_curator_drain_ctx_t *ctx = (kb_curator_drain_ctx_t *)arg;
   /* CPU/index lane: embed claims/narrative/code + contradiction SQL, on its own
    * thread so it drains concurrently with the GPU/LLM lane instead of waiting behind
    * each multi-second extraction. Loops hot while a backlog remains; backs off a
    * short interval when its queues are empty. */
   while (!ctx->stop)
   {
      config_t cfg;
      config_load(&cfg);
      kb_curator_extract_opts_t opts;
      memset(&opts, 0, sizeof(opts));
      snprintf(opts.extract_command, sizeof(opts.extract_command), "%s",
               cfg.kb_curator_extract_command);
      opts.max_tokens =
          cfg.kb_curator_extract_max_tokens > 0 ? cfg.kb_curator_extract_max_tokens : 2048;
      opts.max_attempts = cfg.kb_curator_max_attempts > 0 ? cfg.kb_curator_max_attempts : 3;

      /* Pure-DB2 projection-graph + cross-repo refresh once per poll, ahead of the
       * INDEX queue drain -- content-addressed, so cheap when nothing changed. */
      kb_curator_projection_sweep(&cfg);

      /* Same stage-order resolution as the LLM lane; run_pass filters to this lane. */
      kb_curator_stage_desc_t ordered[16 + KB_CURATOR_MAX_CUSTOM];
      kb_curator_custom_t customs[KB_CURATOR_MAX_CUSTOM];
      size_t nordered = kb_curator_ordered_stages(
          &cfg, ordered, sizeof(ordered) / sizeof(ordered[0]), customs, KB_CURATOR_MAX_CUSTOM);

      int r = 0, drained = 0;
      while (!ctx->stop && drained < CURATOR_DRAIN_BATCH)
      {
         db2_lease_release_idle();
         r = kb_curator_pipeline_run_pass(ordered, nordered, KB_CURATOR_LANE_INDEX, &cfg, &opts,
                                          curator_index_set_status);
         if (r != 1)
            break; /* 0 = INDEX queues empty; <0 = a stage errored */
         drained++;
      }
      kb_background_clear("curator.index");
      db2_lease_release_idle();
      if (ctx->stop)
         break;
      if (r != 1) /* idle or error: back off; a hit batch cap loops hot */
         sleep(INDEX_LANE_POLL_SECS);
   }
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
   if (pthread_create(&ctx->index_thread, NULL, kb_curator_index_lane_main, ctx) == 0)
   {
      ctx->index_active = 1;
      aimee_log(LOG_INFO, "kb.curator.drain", "index-lane thread started");
   }
   else
   {
      aimee_log(LOG_WARN, "kb.curator.drain", "failed to start index-lane thread");
   }
}

void kb_curator_drain_shutdown(kb_curator_drain_ctx_t *ctx)
{
   if (!ctx || (!ctx->active && !ctx->index_active))
      return;
   ctx->stop = 1;
   if (ctx->active)
   {
      pthread_join(ctx->thread, NULL);
      ctx->active = 0;
   }
   if (ctx->index_active)
   {
      pthread_join(ctx->index_thread, NULL);
      ctx->index_active = 0;
   }
   aimee_log(LOG_DEBUG, "kb.curator.drain", "drain threads stopped");
}
