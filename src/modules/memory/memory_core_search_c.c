#if defined(AIMEE_DB2_DISABLED)
#error "memory_core KB-real TU must not be compiled into the AIMEE_DB2_DISABLED (server) build"
#endif
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "memory_core_internal.h"
/* memory_core_search.c: split from memory_core.c into a real translation unit
 * (was memory_core_search.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "aimee.h"
#include "memory_context_internal.h"
#include "memory_rewrite_llm.h" /* weak in-process rewrite seam (KB build only) */
#include <math.h>
#include "db1_optional.h"
#include "db2/entity_edges.h"
#include "db2/kb_runtime_state.h"
#include "db2/memory_health.h"
#include "db2/memory_payload.h"
#include "db2/feature_rows.h"
#include "db2/memory_promotion.h"
#include "db2/memory_query.h"
#include "memory_graph_fusion.h"
#include "kb_mdl.h"
#include "db2/memory_relations.h"
#include "db2/memory_scenes.h"
#include "db2/stopwords.h"
#include "db2/vector_index_ops.h"
#include "db2/vector_verify.h"
#include "memory_vectors.h"
#include "lifecycle.h"
#include "platform_process.h"
#include "memory_platform.h"
#include "log.h"
#include "util.h"       /* util_now_ms — memory.search stage timing */
#include "agent_exec.h" /* agent_http_post: in-process HTTP embedding (no fork) */
#include "cJSON.h"
#include "dogfood.h"
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

static int memory_collect_graph_candidates(const char *raw_query, const char *norm_query,
                                           int fetch_limit, memory_t *out, int count, int max,
                                           memory_candidate_source_t *source_stats,
                                           int *source_stats_count)
{
   if (!raw_query || !raw_query[0] || !norm_query || !norm_query[0] || !out || max <= 0)
      return count;

   char qtokens[24][64];
   int qtoken_count = memory_split_tokens(norm_query, qtokens, 24);
   if (qtoken_count <= 0)
      return count;

   for (int i = 0; i < qtoken_count && count < max; i++)
   {
      if ((int)strlen(qtokens[i]) < 3)
         continue;

      MEMORY_AUTOFREE memory_t *scratch = calloc(32, sizeof(*scratch));
      if (!scratch)
         break;
      int cap = 32;
      int rel_lim = fetch_limit < 8 ? fetch_limit : 8;
      int got = db2_memory_collect_relation_token_matches(qtokens[i], rel_lim, scratch, cap);
      for (int s = 0; s < got && count < max; s++)
      {
         int before = count;
         count = memory_append_unique(out, count, max, &scratch[s]);
         memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                       MEM_SOURCE_GRAPH);
      }

      if (count < max)
      {
         edge_t edges[6];
         int edge_lim = fetch_limit < 6 ? fetch_limit : 6;
         int n = db2_entity_edge_search_by_token(qtokens[i], edges, edge_lim, edge_lim);
         for (int e = 0; e < n && count < max; e++)
         {
            if (edges[e].source[0])
            {
               int before = count;
               count = memory_collect_entity_matches(
                   edges[e].source, fetch_limit < 4 ? fetch_limit : 4, out, count, max);
               memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before,
                                             count, MEM_SOURCE_GRAPH);
            }
            if (edges[e].target[0] && count < max)
            {
               int before = count;
               count = memory_collect_entity_matches(
                   edges[e].target, fetch_limit < 4 ? fetch_limit : 4, out, count, max);
               memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before,
                                             count, MEM_SOURCE_GRAPH);
            }
         }
      }
   }

   return count;
}

int memory_generate_candidates(const char *query, const char *norm_query,
                               memory_query_intent_t intent, const memory_query_plan_t *plan,
                               int fetch_limit, memory_t *out, int max, int64_t *semantic_ids,
                               double *semantic_scores, int *semantic_hit_count,
                               memory_candidate_source_t *source_stats, int *source_stats_count)
{
   if (!query || !query[0] || !norm_query || !norm_query[0] || !out || max <= 0)
      return 0;
   int count = 0;
   if (semantic_hit_count)
      *semantic_hit_count = 0;
   if (source_stats_count)
      *source_stats_count = 0;

   config_t embed_cfg;
   config_load(&embed_cfg);
   const char *embed_cmd = config_embedding_command(&embed_cfg, NULL);
   int variant_fetch_limit = fetch_limit;
   int semantic_fetch_limit = fetch_limit;
   if (plan && plan->route == MEM_ROUTE_HYBRID)
   {
      variant_fetch_limit = (fetch_limit * 2) / 3;
      semantic_fetch_limit = (fetch_limit * 3) / 4;
   }
   else if (plan && plan->route == MEM_ROUTE_GRAPH)
   {
      variant_fetch_limit = fetch_limit / 2;
      semantic_fetch_limit = 0;
   }
   if (variant_fetch_limit < 6)
      variant_fetch_limit = fetch_limit < 6 ? fetch_limit : 6;

   if (plan && plan->route == MEM_ROUTE_GRAPH)
   {
      count = memory_collect_graph_candidates(query, norm_query, fetch_limit, out, count, max,
                                              source_stats, source_stats_count);
      if (count < 0)
         return -1;
      memory_record_query_stage_metric(plan, "graph");
   }

   count = memory_collect_variant_candidates(query, norm_query, intent, variant_fetch_limit, out,
                                             count, max, source_stats, source_stats_count);
   if (count < 0)
      return -1;
   memory_record_query_stage_metric(plan, "variant");

   /* Query decomposition: heuristic sub-query expansion */
   {
      char subqueries[3][128];
      int subquery_count = memory_build_query_decomposition(norm_query, subqueries, 3);
      for (int i = 0; i < subquery_count && count < max; i++)
      {
         count = memory_collect_variant_candidates(
             query, subqueries[i], intent,
             variant_fetch_limit / 2 > 0 ? variant_fetch_limit / 2 : 1, out, count, max,
             source_stats, source_stats_count);
         if (count < 0)
            return -1;
      }
   }

   if (!plan || plan->semantic_enabled)
   {
      if (plan && plan->route == MEM_ROUTE_SEMANTIC)
         semantic_fetch_limit = fetch_limit;
      else if ((!plan || plan->route != MEM_ROUTE_HYBRID) && intent == MEM_QUERY_TEMPORAL)
         semantic_fetch_limit = (fetch_limit * 3) / 4;
      if (semantic_fetch_limit < 8)
         semantic_fetch_limit = fetch_limit < 8 ? fetch_limit : 8;
      if (intent == MEM_QUERY_TEMPORAL && semantic_fetch_limit < 12)
         semantic_fetch_limit = fetch_limit < 12 ? fetch_limit : 12;

      {
         int before = count;
         count = memory_collect_unit_semantic_matches(query, embed_cmd, semantic_fetch_limit, out,
                                                      count, max, semantic_ids, semantic_scores,
                                                      semantic_hit_count, 128);
         if (count < 0)
            return -1;
         memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                       MEM_SOURCE_UNIT | MEM_SOURCE_SEMANTIC);
      }

      {
         int before = count;
         count = memory_collect_semantic_matches(query, embed_cmd, semantic_fetch_limit, out, count,
                                                 max, semantic_ids, semantic_scores,
                                                 semantic_hit_count, 128);
         if (count < 0)
            return -1;
         memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                       MEM_SOURCE_SEMANTIC);
      }

      memory_record_query_stage_metric(plan, "semantic");
   }

   /* Negation lexical recall: when query has negative polarity, search for
    * not_<token> synthetic terms so negative facts surface as candidates. */
   {
      config_t neg_cfg;
      if (config_load(&neg_cfg) == 0 && neg_cfg.memory_negation_enabled &&
          memory_query_polarity(query) == POLARITY_NEGATIVE)
      {
         /* Build a synthetic "not_<token>" query from the query tokens */
         char neg_q[1024];
         int neg_count = extract_negation_tokens(query, neg_q, sizeof(neg_q));
         if (neg_count > 0 && neg_q[0])
         {
            MEMORY_AUTOFREE memory_t *scratch = calloc(64, sizeof(*scratch));
            if (!scratch)
               return count;
            int cap = 64;
            const char *embed_cmd = config_embedding_command(&neg_cfg, NULL);
            /* Negation tokens aren't in the vector payload yet, so this falls
             * back to memory-level semantic search of the synthetic
             * "not_<token>" query. Future work: add negation_tokens to the
             * memory point payload and switch to a payload filter for the
             * literal-token semantic the negation lexical index expects. */
            int got = memory_collect_memory_matches_via_vector(neg_q, embed_cmd, fetch_limit,
                                                               scratch, cap);
            for (int s = 0; s < got && count < max; s++)
            {
               int before = count;
               count = memory_append_unique(out, count, max, &scratch[s]);
               memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before,
                                             count, MEM_SOURCE_LEXICAL);
            }
         }
      }
   }

   /* Fallback: LIKE search */
   MEMORY_AUTOFREE memory_t *like_matches = calloc(128, sizeof(*like_matches));
   if (!like_matches)
      return count;
   int like_count = memory_find_facts_like(norm_query, fetch_limit, like_matches, 128);
   if (like_count > 0)
      memory_record_query_stage_metric(plan, "like");
   for (int i = 0; i < like_count && count < max; i++)
   {
      int before = count;
      count = memory_append_unique(out, count, max, &like_matches[i]);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_LIKE);
   }
   return count;
}

/* === HyDE and Query Decomposition ===
 *
 * memory_query_rewrite: call the external rewrite command (if configured and
 * enabled) and parse the result into memory_query_rewrite_t.  On any error
 * the struct is zeroed out (no-op fallback).
 *
 * The external command receives a JSON object on stdin:
 *   {"query": "...", "hyde": true/false, "decompose": true/false,
 *    "max_subqueries": N}
 *
 * It must write a JSON object to stdout:
 *   {"hyde_answer": "...", "sub_questions": ["...", "..."]}
 *
 * Both fields are optional; missing / null means "not generated".
 */

void memory_query_rewrite(const char *query, const config_t *cfg, memory_query_rewrite_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!query || !query[0] || !cfg || !cfg->memory_rewrite_enabled)
      return;
   /* Need either the in-process curator-LLM seam (KB build) or a subprocess
    * command; nothing to do if neither is available. */
   int have_inproc = (memory_rewrite_llm_inproc != NULL);
   if (!have_inproc && !cfg->memory_rewrite_command[0])
      return;
   if (!cfg->memory_rewrite_hyde && !cfg->memory_rewrite_decompose)
      return;

   /* Only rewrite for SEMANTIC and HYBRID routes (not LEXICAL / GRAPH) */
   {
      memory_query_plan_t plan;
      if (memory_query_plan(query, 10, 96, &plan) == 0)
      {
         if (plan.route == MEM_ROUTE_LEXICAL || plan.route == MEM_ROUTE_GRAPH)
            return;
      }
   }

   int max_sub = cfg->memory_rewrite_max_subqueries > 0 ? cfg->memory_rewrite_max_subqueries : 4;
   char *resp = NULL;

   if (have_inproc)
   {
      /* In-process: build the rewrite instructions and run query + instructions
       * through the curator LLM (small/fast tier). No subprocess, no sidecar
       * script — this is the production path (rewrite runs KB-side). */
      const char *hyde_line =
          cfg->memory_rewrite_hyde
              ? "- hyde_answer: a single short hypothetical answer to the query, as if you knew "
                "the fact. 1-3 sentences, declarative prose, no caveats.\n"
              : "";
      char decomp_line[192] = "";
      if (cfg->memory_rewrite_decompose)
         snprintf(decomp_line, sizeof(decomp_line),
                  "- sub_questions: up to %d simpler sub-queries that together cover the original "
                  "(array of strings, may be empty if already atomic).\n",
                  max_sub);
      char sys_prompt[1024];
      snprintf(sys_prompt, sizeof(sys_prompt),
               "You are a query-rewrite assistant for a retrieval system. Given the user's query, "
               "produce these fields:\n%s%sRespond with a single JSON object, no prose and no code "
               "fences. Schema: {\"hyde_answer\": \"...\" (or \"\"), \"sub_questions\": [...] (or "
               "[])}",
               hyde_line, decomp_line);
      resp = memory_rewrite_llm_inproc(cfg, sys_prompt, query);
   }
   else
   {
      /* Legacy subprocess sidecar: the command builds the prompt from this JSON. */
      cJSON *input = cJSON_CreateObject();
      if (!input)
         return;
      cJSON_AddStringToObject(input, "query", query);
      cJSON_AddBoolToObject(input, "hyde", cfg->memory_rewrite_hyde);
      cJSON_AddBoolToObject(input, "decompose", cfg->memory_rewrite_decompose);
      cJSON_AddNumberToObject(input, "max_subqueries", max_sub);
      char *input_str = cJSON_PrintUnformatted(input);
      cJSON_Delete(input);
      if (!input_str)
         return;
      size_t resp_len = 0;
      int rc = platform_exec_pipe(cfg->memory_rewrite_command, input_str, strlen(input_str), &resp,
                                  &resp_len);
      free(input_str);
      if (rc != 0)
      {
         free(resp);
         resp = NULL;
      }
   }

   if (!resp || !resp[0])
   {
      free(resp);
      aimee_log(LOG_WARN, "memory_rewrite", "rewrite produced no output");
      return;
   }

   /* Parse response. Models (esp. small local ones) often wrap JSON in ```json
    * fences or add a sentence of prose, so parse the outermost {...} object. */
   const char *jstart = strchr(resp, '{');
   const char *jend = strrchr(resp, '}');
   cJSON *j = NULL;
   if (jstart && jend && jend >= jstart)
   {
      size_t span = (size_t)(jend - jstart) + 1;
      char *obj = malloc(span + 1);
      if (obj)
      {
         memcpy(obj, jstart, span);
         obj[span] = '\0';
         j = cJSON_Parse(obj);
         free(obj);
      }
   }
   free(resp);
   if (!j)
   {
      aimee_log(LOG_WARN, "memory_rewrite", "rewrite returned invalid JSON");
      return;
   }

   /* hyde_answer */
   if (cfg->memory_rewrite_hyde)
   {
      cJSON *ha = cJSON_GetObjectItemCaseSensitive(j, "hyde_answer");
      if (cJSON_IsString(ha) && ha->valuestring[0])
      {
         snprintf(out->hyde_answer, sizeof(out->hyde_answer), "%s", ha->valuestring);
         out->has_hyde = 1;
      }
   }

   /* sub_questions */
   if (cfg->memory_rewrite_decompose)
   {
      cJSON *sq = cJSON_GetObjectItemCaseSensitive(j, "sub_questions");
      if (cJSON_IsArray(sq))
      {
         cJSON *el;
         cJSON_ArrayForEach(el, sq)
         {
            if (out->sub_question_count >= MEMORY_REWRITE_MAX_SUBQUERIES)
               break;
            if (cJSON_IsString(el) && el->valuestring[0])
            {
               snprintf(out->sub_questions[out->sub_question_count], sizeof(out->sub_questions[0]),
                        "%s", el->valuestring);
               out->sub_question_count++;
            }
         }
         if (out->sub_question_count > 0)
            out->has_decomp = 1;
      }
   }

   cJSON_Delete(j);
}

int memory_find_facts(const char *query, int limit, memory_t *out, int max)
{
   /* Stage timing: separate the embed-spawn and rerank-spawn costs (the suspected
    * dominant terms in live find_facts latency) from the rest, one line per call.
    * Counters live in memory_core_helpers.inc (same TU). */
   long long _ff_t0 = util_now_ms();
   s_qembed_ms = 0;
   s_qembed_spawns = 0;
   s_rerank_ms = 0;
   s_rerank_calls = 0;
   int n = memory_find_facts_scoped(query, NULL, NULL, limit, out, max);
   aimee_log(LOG_INFO, "memory.find_facts.timing",
             "total=%lldms embed=%lldms/%d rerank=%lldms/%d results=%d", util_now_ms() - _ff_t0,
             s_qembed_ms, s_qembed_spawns, s_rerank_ms, s_rerank_calls, n);
   /* Dogfood record at the non-scoped entry point so MCP/agent callers land
    * here; scoped callers (CLI cmd_memory_find_facts) log themselves. */
   if (n > 0 && out)
   {
      int64_t ids[32];
      int id_count = n < 32 ? n : 32;
      for (int i = 0; i < id_count; i++)
         ids[i] = out[i].id;
      dogfood_log_moment_live("memory_search", query, ids, id_count, NULL);
   }
   else if (n >= 0)
   {
      dogfood_log_moment_live("memory_search", query, NULL, 0, NULL);
   }
   return n;
}

/* Merge additional candidates into the primary candidate array.
 * Deduplicates by memory id; existing entries are kept as-is. */
static int memory_candidates_merge(memory_t *primary, int primary_count, const memory_t *extra,
                                   int extra_count, int cap)
{
   for (int e = 0; e < extra_count && primary_count < cap; e++)
   {
      int found = 0;
      for (int p = 0; p < primary_count; p++)
      {
         if (primary[p].id == extra[e].id)
         {
            found = 1;
            break;
         }
      }
      if (!found)
         primary[primary_count++] = extra[e];
   }
   return primary_count;
}

int memory_expand_to_session_window(memory_t *out, int count, int max, int window_radius)
{
   if (!out || count <= 0 || max <= 0 || window_radius <= 0)
      return count;

   int original_count = count;
   for (int i = 0; i < original_count && count < max; i++)
   {
      if (!out[i].source_session[0])
         continue;

      MEMORY_AUTOFREE memory_t *scratch = calloc(32, sizeof(*scratch));
      if (!scratch)
         break;
      int cap = 32;

      int got = db2_memory_session_neighbors_before(out[i].source_session, out[i].id, window_radius,
                                                    scratch, cap);
      for (int s = 0; s < got && count < max; s++)
      {
         int dup = 0;
         for (int k = 0; k < count; k++)
            if (out[k].id == scratch[s].id)
            {
               dup = 1;
               break;
            }
         if (!dup)
            out[count++] = scratch[s];
      }

      got = db2_memory_session_neighbors_after(out[i].source_session, out[i].id, window_radius,
                                               scratch, cap);
      for (int s = 0; s < got && count < max; s++)
      {
         int dup = 0;
         for (int k = 0; k < count; k++)
            if (out[k].id == scratch[s].id)
            {
               dup = 1;
               break;
            }
         if (!dup)
            out[count++] = scratch[s];
      }
   }
   return count;
}

int memory_find_facts_scoped(const char *query, const char *scope_type, const char *scope_value,
                             int limit, memory_t *out, int max)
{
   if (!query || !query[0])
      return 0;

   /* Fresh query-embedding memo for this recall: every lane / HyDE pass /
    * sub-query below embeds via memory_embed_text_runtime, which reuses vectors
    * cached here instead of re-spawning the embedder for identical text. */
   memory_query_embed_cache_reset();

   /* Aggregation route — detects "coverage" queries ("list all X", "every
    * Y", "what Xs has ENTITY Ved") and dispatches to memory_aggregate()
    * instead of the hybrid vector path. Runs before the vector readiness
    * check on purpose: aggregation works even when the semantic index is
    * unavailable.  Gated behind memory.aggregation.enabled so point-query
    * behaviour stays byte-identical until operators opt in. */
   {
      config_t agg_cfg;
      if (config_load(&agg_cfg) == 0 && agg_cfg.memory_aggregation_enabled)
      {
         memory_aggregation_hint_t hint;
         if (memory_detect_aggregation_shape(query, &hint) && hint.detected)
         {
            int req_limit = limit > 0 ? limit : 20;
            int cap = agg_cfg.memory_aggregation_max_items > 0
                          ? agg_cfg.memory_aggregation_max_items
                          : MEMORY_AGGREGATION_DEFAULT_MAX_ITEMS;
            if (cap > max)
               cap = max;
            if (req_limit > cap)
               req_limit = cap;
            int truncated = 0;
            int n = memory_aggregate(&hint, query, req_limit, out, max, &truncated);
            /* Scope hint hasn't been set on this early-return path, so
             * there's nothing to tear down. */
            (void)scope_type;
            (void)scope_value;
            return n;
         }
      }
   }

   if (!memory_vector_ready())
      return memory_find_facts_lexical_fallback(query, scope_type, scope_value, limit, out, max);
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   /* Set thread-local scope hint so semantic search filters pgvector points
    * by scope at the payload-index layer rather than post-filtering in DB2.
    * Cleared on every exit path below. */
   const char *hint_ws = (scope_type && strcmp(scope_type, "workspace") == 0) ? scope_value : NULL;
   const char *hint_pj = (scope_type && strcmp(scope_type, "project") == 0) ? scope_value : NULL;
   pgvec_memory_vector_scope_hint_set(hint_ws, hint_pj);
   enum
   {
      MEMORY_RERANK_BUFFER = 96
   };
   char norm_query[512];
   struct timespec ts0, ts1;
   normalize_key(query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", query);
   clock_gettime(CLOCK_MONOTONIC, &ts0);

   /* --- HyDE / Query Decomposition pre-retrieval rewrite --- */
   memory_query_rewrite_t rewrite;
   memset(&rewrite, 0, sizeof(rewrite));
   {
      config_t cfg;
      /* Fire when enabled and a rewrite mechanism exists: either the legacy
       * subprocess command, or the in-process curator-LLM seam (KB build). The
       * in-process path needs no command, so requiring one would silently
       * disable HyDE on an accelerated backend that only configures a provider. */
      if (config_load(&cfg) == 0 && cfg.memory_rewrite_enabled &&
          (cfg.memory_rewrite_command[0] || memory_rewrite_llm_inproc))
         memory_query_rewrite(query, &cfg, &rewrite);
   }

   memory_query_intent_t intent = memory_query_intent(query, norm_query);

   /* Embed-batching: warm the per-recall memo with the query texts the semantic
    * lanes will embed — the base query plus its semantic form, for the main query
    * and any decomposition sub-questions — in ONE batched embedder call. The
    * lanes then hit the memo instead of one embedder round-trip each. Best-effort:
    * texts are built with the same helpers the lanes use (so they match and hit);
    * any that don't match simply fall back to an individual embed. */
   {
      config_t pw_cfg;
      if (config_load(&pw_cfg) == 0 && pw_cfg.embedding_command[0])
      {
         const char *bt[3 + 2 * MEMORY_REWRITE_MAX_SUBQUERIES];
         char sem_main[1024];
         char sem_sub[MEMORY_REWRITE_MAX_SUBQUERIES][1024];
         int bn = 0;
         bt[bn++] = query;
         memory_build_semantic_query_text(query, memory_query_intent(query, query), sem_main,
                                          sizeof(sem_main));
         if (sem_main[0])
            bt[bn++] = sem_main;
         if (rewrite.has_hyde && rewrite.hyde_answer[0])
            bt[bn++] = rewrite.hyde_answer;
         for (int q = 0; q < rewrite.sub_question_count; q++)
         {
            const char *sq = rewrite.sub_questions[q];
            bt[bn++] = sq;
            memory_build_semantic_query_text(sq, memory_query_intent(sq, sq), sem_sub[q],
                                             sizeof(sem_sub[q]));
            if (sem_sub[q][0])
               bt[bn++] = sem_sub[q];
         }
         memory_query_embed_prewarm(bt, bn, pw_cfg.embedding_command);
      }
   }

   memory_query_plan_t plan;
   if (memory_query_plan(query, limit, MEMORY_RERANK_BUFFER, &plan) != 0)
      memset(&plan, 0, sizeof(plan));
   int fetch_multiplier = plan.fetch_multiplier > 0 ? plan.fetch_multiplier : 4;
   int fetch_min = plan.min_fetch > 0 ? plan.min_fetch : 24;
   int fetch_max = plan.max_fetch > 0 ? plan.max_fetch : MEMORY_RERANK_BUFFER;
   fetch_multiplier = memory_env_int("AIMEE_MEMORY_FETCH_MULTIPLIER", fetch_multiplier, 1, 16);
   fetch_min = memory_env_int("AIMEE_MEMORY_FETCH_MIN", fetch_min, limit, MEMORY_RERANK_BUFFER);
   fetch_max = memory_env_int("AIMEE_MEMORY_FETCH_MAX", fetch_max, limit, MEMORY_RERANK_BUFFER);
   if (fetch_max < fetch_min)
      fetch_max = fetch_min;
   /* Dynamic fetch budget: when enabled, scale candidate pool by query
    * specificity. With shape-aware mode on (the default when the budget
    * is enabled), the factor also accounts for query shape — LIST /
    * QUANTITATIVE queries want a wider pool for aggregation; YES_NO
    * queries converge fast with a smaller pool. Unlike the previous
    * clamp-down-only behaviour, the shape-aware path can also grow
    * fetch_max up to MEMORY_RERANK_BUFFER when a short, wide query
    * (e.g. "list all projects") would otherwise be starved. */
   {
      config_t fb_cfg;
      if (config_load(&fb_cfg) == 0 && fb_cfg.memory_fetch_budget_enabled &&
          fb_cfg.memory_fetch_budget_base > 0)
      {
         int base = fb_cfg.memory_fetch_budget_base;
         char fb_tokens[24][64];
         int ntok = memory_split_tokens(norm_query, fb_tokens, 24);
         double spec;
         if (fb_cfg.memory_fetch_budget_shape_aware)
         {
            spec = memory_fetch_budget_factor(plan.shape, ntok);
         }
         else
         {
            spec = ntok <= 2 ? 1.5 : ntok <= 4 ? 1.0 : ntok <= 7 ? 0.75 : 0.5;
         }
         int dynamic_max = (int)(base * spec);
         if (dynamic_max < fetch_min)
            dynamic_max = fetch_min;
         if (dynamic_max > MEMORY_RERANK_BUFFER)
            dynamic_max = MEMORY_RERANK_BUFFER;
         if (fb_cfg.memory_fetch_budget_shape_aware)
            fetch_max = dynamic_max;
         else if (dynamic_max < fetch_max)
            fetch_max = dynamic_max;
      }
   }
   int fetch_limit = limit * fetch_multiplier;
   if (fetch_limit < limit)
      fetch_limit = limit;
   if (fetch_limit < fetch_min)
      fetch_limit = fetch_min;
   if (fetch_limit > fetch_max)
      fetch_limit = fetch_max;
   int64_t semantic_ids[128];
   double semantic_scores[128];
   int semantic_hit_count = 0;
   memory_candidate_source_t source_stats[128];
   int source_stats_count = 0;
   memory_t *candidates = (memory_t *)calloc(MEMORY_RERANK_BUFFER, sizeof(*candidates));
   if (!candidates)
   {
      pgvec_memory_vector_scope_hint_clear();
      return -1;
   }

   /* Primary retrieval pass */
   const char *effective_query = query;
   char hyde_norm[512];
   if (rewrite.has_hyde && rewrite.hyde_answer[0])
   {
      /* Use HyDE answer as the text for embedding, but original query for
       * lexical/graph recall. */
      normalize_key(rewrite.hyde_answer, hyde_norm, sizeof(hyde_norm));
      if (!hyde_norm[0])
         snprintf(hyde_norm, sizeof(hyde_norm), "%s", rewrite.hyde_answer);
      effective_query = rewrite.hyde_answer;
   }

   int count = memory_generate_candidates(
       effective_query, rewrite.has_hyde ? hyde_norm : norm_query, intent, &plan, fetch_limit,
       candidates, MEMORY_RERANK_BUFFER, semantic_ids, semantic_scores, &semantic_hit_count,
       source_stats, &source_stats_count);
   if (count < 0)
   {
      pgvec_memory_vector_scope_hint_clear();
      free(candidates);
      return -1;
   }

   /* If HyDE was used, also run a pass with the original query and merge */
   if (rewrite.has_hyde && effective_query != query)
   {
      int64_t orig_sem_ids[128];
      double orig_sem_scores[128];
      int orig_sem_count = 0;
      memory_candidate_source_t orig_src[128];
      int orig_src_count = 0;
      MEMORY_AUTOFREE memory_t *extra = calloc(MEMORY_RERANK_BUFFER, sizeof(*extra));
      if (!extra)
      {
         pgvec_memory_vector_scope_hint_clear();
         free(candidates);
         return -1;
      }
      int extra_count = memory_generate_candidates(
          query, norm_query, intent, &plan, fetch_limit / 2 > 0 ? fetch_limit / 2 : 8, extra,
          MEMORY_RERANK_BUFFER, orig_sem_ids, orig_sem_scores, &orig_sem_count, orig_src,
          &orig_src_count);
      if (extra_count < 0)
      {
         pgvec_memory_vector_scope_hint_clear();
         free(candidates);
         return -1;
      }
      count = memory_candidates_merge(candidates, count, extra, extra_count, MEMORY_RERANK_BUFFER);
   }

   /* Decomposition: additional passes for each sub-question */
   for (int q = 0; q < rewrite.sub_question_count; q++)
   {
      char sub_norm[512];
      normalize_key(rewrite.sub_questions[q], sub_norm, sizeof(sub_norm));
      if (!sub_norm[0])
         snprintf(sub_norm, sizeof(sub_norm), "%s", rewrite.sub_questions[q]);

      memory_query_plan_t sub_plan;
      if (memory_query_plan(rewrite.sub_questions[q], limit, MEMORY_RERANK_BUFFER, &sub_plan) != 0)
         sub_plan = plan;

      int64_t sub_sem_ids[128];
      double sub_sem_scores[128];
      int sub_sem_count = 0;
      memory_candidate_source_t sub_src[128];
      int sub_src_count = 0;
      MEMORY_AUTOFREE memory_t *sub_cands = calloc(MEMORY_RERANK_BUFFER / 2, sizeof(*sub_cands));
      if (!sub_cands)
         break;
      int sub_count = memory_generate_candidates(
          rewrite.sub_questions[q], sub_norm,
          memory_query_intent(rewrite.sub_questions[q], sub_norm), &sub_plan,
          fetch_limit / (rewrite.sub_question_count + 1), sub_cands, MEMORY_RERANK_BUFFER / 2,
          sub_sem_ids, sub_sem_scores, &sub_sem_count, sub_src, &sub_src_count);
      if (sub_count < 0)
      {
         pgvec_memory_vector_scope_hint_clear();
         free(candidates);
         return -1;
      }
      count =
          memory_candidates_merge(candidates, count, sub_cands, sub_count, MEMORY_RERANK_BUFFER);
   }

   count = memory_filter_scope(candidates, count, scope_type, scope_value);

   /* Lifecycle archival filter: when the feature is enabled and
    * hide_archived is set, drop any candidate whose lifecycle_state is
    * `archived` before reranking.  memory_get() keeps returning them;
    * this only affects the recall surface. */
   {
      config_t lc_cfg;
      if (config_load(&lc_cfg) == 0 && lc_cfg.memory_lifecycle_enabled &&
          lc_cfg.memory_lifecycle_hide_archived && count > 0)
      {
         /* Batch-probe archive state via db2 and drop any archived ids from
          * the candidate array. */
         int64_t cand_ids[MEMORY_RERANK_BUFFER];
         int probe_n = count < MEMORY_RERANK_BUFFER ? count : MEMORY_RERANK_BUFFER;
         for (int i = 0; i < probe_n; i++)
            cand_ids[i] = candidates[i].id;
         int64_t archived_ids[MEMORY_RERANK_BUFFER];
         int n_arch =
             db2_memory_filter_archived_ids(cand_ids, probe_n, archived_ids, MEMORY_RERANK_BUFFER);
         {

            int write = 0;
            for (int i = 0; i < count; i++)
            {
               int is_archived = 0;
               for (int a = 0; a < n_arch; a++)
                  if (archived_ids[a] == candidates[i].id)
                  {
                     is_archived = 1;
                     break;
                  }
               if (!is_archived)
                  candidates[write++] = candidates[i];
            }
            count = write;
         }
      }
   }

   /* Graph-code fusion rerank (graph_code_fusion_state == "on", set thread-local
    * by the recall handler). Seed graph expansion from the base candidates'
    * canonical entity nodes, surface any graph-reachable memories not already in
    * the candidate set (the "bridge" case the proposal targets), and stage the
    * expansion so memory_compute_score blends each candidate's graph_score into
    * the rank. When fusion is off this whole block is skipped and ranking is
    * byte-identical. */
   memory_graph_expansion_t fusion_exp[MEMORY_RERANK_BUFFER];
   int fusion_exp_n = 0;
   if (memory_fusion_state_is_on() && count > 0)
   {
      memory_query_plan_t fplan;
      memset(&fplan, 0, sizeof(fplan));
      (void)memory_graph_detect_code_shape(query, 0, &fplan);

      enum
      {
         FUSION_SEED_CANDS = 12,
         FUSION_MAX_SEEDS = 48
      };
      char seed_storage[FUSION_MAX_SEEDS][GRAPH_ENDPOINT_MAX];
      const char *seeds[FUSION_MAX_SEEDS];
      int nseeds = 0;
      int seed_cands = count < FUSION_SEED_CANDS ? count : FUSION_SEED_CANDS;
      for (int i = 0; i < seed_cands && nseeds < FUSION_MAX_SEEDS; i++)
      {
         db2_memory_entity_row_t ents[8];
         int ne = db2_memory_entities_list_weighted(candidates[i].id, ents,
                                                    (int)(sizeof(ents) / sizeof(ents[0])));
         for (int e = 0; e < ne && nseeds < FUSION_MAX_SEEDS; e++)
         {
            int dup = 0;
            for (int s = 0; s < nseeds; s++)
               if (strcmp(seeds[s], ents[e].entity) == 0)
               {
                  dup = 1;
                  break;
               }
            if (dup)
               continue;
            snprintf(seed_storage[nseeds], GRAPH_ENDPOINT_MAX, "%s", ents[e].entity);
            seeds[nseeds] = seed_storage[nseeds];
            nseeds++;
         }
      }

      if (nseeds > 0)
      {
         /* Ablation sub-gates (set per arm by the eval runner; default on):
          * code_projection gates whether the expansion may enter code subgraphs
          * (AND-ed with the per-query code-shape detection), utility_scoring
          * toggles utility-weighting of the entity-edge neighbours. */
         int allow_code = fplan.allow_code_graph && memory_fusion_code_projection();
         fusion_exp_n = memory_graph_expand_from_seeds(
             seeds, nseeds, fplan.graph_hops > 0 ? fplan.graph_hops : 2, 16, allow_code,
             memory_fusion_utility_scoring(), fusion_exp,
             (int)(sizeof(fusion_exp) / sizeof(fusion_exp[0])));

         for (int i = 0; i < fusion_exp_n && count < MEMORY_RERANK_BUFFER; i++)
         {
            int present = 0;
            for (int c = 0; c < count; c++)
               if (candidates[c].id == fusion_exp[i].memory_id)
               {
                  present = 1;
                  break;
               }
            if (present)
               continue;
            memory_t row;
            if (db2_memory_get(fusion_exp[i].memory_id, &row) == 0 && row.id > 0)
               candidates[count++] = row;
         }
         memory_fusion_expansions_set(fusion_exp, fusion_exp_n);
      }
   }

   int reranked =
       memory_rerank_matches(query, candidates, count, limit, semantic_ids, semantic_scores,
                             semantic_hit_count, source_stats, source_stats_count);
   memory_fusion_expansions_clear();
   if (reranked > max)
      reranked = max;
   {
      config_t floor_cfg;
      if (config_load(&floor_cfg) == 0 && floor_cfg.memory_recall_lanes_enabled)
      {
         int eff =
             memory_apply_lane_floor(candidates, count, s_lane_summary_ids, s_lane_summary_count,
                                     floor_cfg.memory_recall_lanes_floor_summary, reranked);
         memory_apply_lane_floor(candidates, count, s_lane_fact_ids, s_lane_fact_count,
                                 floor_cfg.memory_recall_lanes_floor_fact, eff);
      }
   }
   for (int i = 0; i < reranked; i++)
      out[i] = candidates[i];

   /* Session window expansion: inject conversational neighbours when enabled */
   {
      config_t win_cfg;
      if (config_load(&win_cfg) == 0 && win_cfg.memory_window_radius > 0)
         reranked =
             memory_expand_to_session_window(out, reranked, max, win_cfg.memory_window_radius);
   }

   clock_gettime(CLOCK_MONOTONIC, &ts1);
   memory_record_query_plan_metrics(&plan, memory_query_elapsed_ms(&ts0, &ts1));
   pgvec_memory_vector_scope_hint_clear();
   free(candidates);
   return reranked;
}

int memory_find_facts_visible(const char *query, const char *workspace, const char *project,
                              int limit, memory_t *out, int max)
{
   if (!query || !query[0])
      return 0;
   /* Fresh query-embedding memo for this recall (see memory_find_facts_scoped). */
   memory_query_embed_cache_reset();
   if (!memory_vector_ready())
      return memory_find_facts_visible_lexical_fallback(query, workspace, project, limit, out, max);
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   /* Hint semantic search to filter pgvector candidates by scope. DB2-side
    * memory_scope_visibility_rank still runs below as the authoritative
    * visibility check — the hint is a pre-filter optimization. */
   pgvec_memory_vector_scope_hint_set(workspace, project);
   enum
   {
      MEMORY_RERANK_BUFFER = 96
   };
   char norm_query[512];
   struct timespec ts0, ts1;
   clock_gettime(CLOCK_MONOTONIC, &ts0);
   normalize_key(query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", query);
   memory_query_intent_t intent = memory_query_intent(query, norm_query);
   memory_query_plan_t plan;
   if (memory_query_plan(query, limit, MEMORY_RERANK_BUFFER, &plan) != 0)
      memset(&plan, 0, sizeof(plan));
   int fetch_multiplier = plan.fetch_multiplier > 0 ? plan.fetch_multiplier : 4;
   int fetch_min = plan.min_fetch > 0 ? plan.min_fetch : 24;
   int fetch_max = plan.max_fetch > 0 ? plan.max_fetch : MEMORY_RERANK_BUFFER;
   fetch_multiplier = memory_env_int("AIMEE_MEMORY_FETCH_MULTIPLIER", fetch_multiplier, 1, 16);
   fetch_min = memory_env_int("AIMEE_MEMORY_FETCH_MIN", fetch_min, limit, MEMORY_RERANK_BUFFER);
   fetch_max = memory_env_int("AIMEE_MEMORY_FETCH_MAX", fetch_max, limit, MEMORY_RERANK_BUFFER);
   if (fetch_max < fetch_min)
      fetch_max = fetch_min;
   int fetch_limit = limit * fetch_multiplier;
   if (fetch_limit < limit)
      fetch_limit = limit;
   if (fetch_limit < fetch_min)
      fetch_limit = fetch_min;
   if (fetch_limit > fetch_max)
      fetch_limit = fetch_max;
   int64_t semantic_ids[128];
   double semantic_scores[128];
   int semantic_hit_count = 0;
   memory_candidate_source_t source_stats[128];
   int source_stats_count = 0;
   MEMORY_AUTOFREE memory_t *candidates = calloc(MEMORY_RERANK_BUFFER, sizeof(*candidates));
   if (!candidates)
   {
      pgvec_memory_vector_scope_hint_clear();
      return -1;
   }
   int scope_rank[MEMORY_RERANK_BUFFER];

   int count = memory_generate_candidates(query, norm_query, intent, &plan, fetch_limit, candidates,
                                          MEMORY_RERANK_BUFFER, semantic_ids, semantic_scores,
                                          &semantic_hit_count, source_stats, &source_stats_count);
   if (count < 0)
   {
      pgvec_memory_vector_scope_hint_clear();
      return -1;
   }
   int kept = 0;
   for (int i = 0; i < count; i++)
   {
      int rank = memory_scope_visibility_rank(candidates[i].id, workspace, project);
      if (rank <= 0)
         continue;
      if (kept != i)
         candidates[kept] = candidates[i];
      scope_rank[kept] = rank;
      kept++;
   }
   count = kept;
   int reranked =
       memory_rerank_matches(query, candidates, count, limit, semantic_ids, semantic_scores,
                             semantic_hit_count, source_stats, source_stats_count);
   if (reranked > max)
      reranked = max;
   for (int i = 0; i < reranked; i++)
      scope_rank[i] = memory_scope_visibility_rank(candidates[i].id, workspace, project);
   for (int i = 0; i < reranked - 1; i++)
   {
      int best = i;
      for (int j = i + 1; j < reranked; j++)
      {
         if (scope_rank[j] > scope_rank[best])
            best = j;
      }
      if (best != i)
      {
         memory_t tmp_mem = candidates[i];
         int tmp_rank = scope_rank[i];
         candidates[i] = candidates[best];
         scope_rank[i] = scope_rank[best];
         candidates[best] = tmp_mem;
         scope_rank[best] = tmp_rank;
      }
   }
   {
      config_t floor_cfg;
      if (config_load(&floor_cfg) == 0 && floor_cfg.memory_recall_lanes_enabled)
      {
         int eff =
             memory_apply_lane_floor(candidates, count, s_lane_summary_ids, s_lane_summary_count,
                                     floor_cfg.memory_recall_lanes_floor_summary, reranked);
         memory_apply_lane_floor(candidates, count, s_lane_fact_ids, s_lane_fact_count,
                                 floor_cfg.memory_recall_lanes_floor_fact, eff);
      }
   }
   for (int i = 0; i < reranked; i++)
      out[i] = candidates[i];
   clock_gettime(CLOCK_MONOTONIC, &ts1);
   memory_record_query_plan_metrics(&plan, memory_query_elapsed_ms(&ts0, &ts1));
   pgvec_memory_vector_scope_hint_clear();
   return reranked;
}

int memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   return memory_diagnose_scoped(query, NULL, NULL, limit, out, max);
}

int memory_diagnose_scoped(const char *query, const char *scope_type, const char *scope_value,
                           int limit, memory_diagnostic_t *out, int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;

   char norm_query[512];
   normalize_key(query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", query);

   int64_t shortcut_ids[8];
   int shortcut_promoted = 0;
   int shortcut_n =
       db2_retrieval_shortcut_lookup(norm_query, shortcut_ids, 8, &shortcut_promoted, NULL);
   memory_t shortcut_matches[8];
   int shortcut_match_n = 0;
   if (shortcut_n > 0)
      for (int i = 0; i < shortcut_n && shortcut_match_n < 8; i++)
         if (db2_memory_get(shortcut_ids[i], &shortcut_matches[shortcut_match_n]) == 0)
            shortcut_match_n++;

   memory_t *matches = (memory_t *)calloc(64, sizeof(*matches));
   if (!matches)
      return -1;
   int count = memory_find_facts_scoped(query, scope_type, scope_value, limit > 0 ? limit : 10,
                                        matches, 64);
   if (count < 0)
   {
      free(matches);
      return -1;
   }
   if (count > max)
      count = max;

   config_t embed_cfg;
   config_load(&embed_cfg);
   const char *embed_cmd = config_embedding_command(&embed_cfg, NULL);
   int64_t semantic_ids[128];
   double semantic_scores[128];
   int semantic_hit_count = 0;
   if (memory_collect_semantic_matches(query, embed_cmd, 32, matches, count, 64, semantic_ids,
                                       semantic_scores, &semantic_hit_count, 128) < 0)
   {
      free(matches);
      return -1;
   }

   memory_pagerank_config_t pagerank_cfg;
   memory_pagerank_score_t pagerank_scores[64];
   memory_load_pagerank_config(&pagerank_cfg);
   int pagerank_count =
       memory_compute_pagerank_scores(matches, count, &pagerank_cfg, pagerank_scores, 64);

   for (int i = 0; i < count; i++)
   {
      out[i].memory = matches[i];
      memory_ranker_input_t ri = memory_ranker_input_from(&matches[i]);
      memory_compute_score_parts(query, norm_query, &ri, semantic_ids, semantic_scores,
                                 semantic_hit_count, pagerank_scores, pagerank_count,
                                 &out[i].parts);
      out[i].parts.confidence = matches[i].confidence;
   }
   {
      int64_t shortcut_ids[8];
      int shortcut_n = count < 8 ? count : 8;
      for (int i = 0; i < shortcut_n; i++)
         shortcut_ids[i] = out[i].memory.id;
      (void)db2_retrieval_shortcut_observe(norm_query, shortcut_ids, shortcut_n);
   }
   if (shortcut_promoted && shortcut_match_n > 0)
   {
      int promoted_count = shortcut_match_n < max ? shortcut_match_n : max;
      for (int i = 0; i < promoted_count; i++)
      {
         out[i].memory = shortcut_matches[i];
         memory_ranker_input_t ri = memory_ranker_input_from(&shortcut_matches[i]);
         memory_compute_score_parts(query, norm_query, &ri, semantic_ids, semantic_scores,
                                    semantic_hit_count, pagerank_scores, pagerank_count,
                                    &out[i].parts);
         out[i].parts.confidence = shortcut_matches[i].confidence;
      }
      free(matches);
      return promoted_count;
   }
   free(matches);
   return count;
}

int memory_explain_match(const char *query, int64_t memory_id, memory_diagnostic_t *out)
{
   if (!query || !query[0] || memory_id <= 0 || !out)
      return -1;
   if (!memory_vector_ready())
      return -1;

   memory_t mem;
   if (memory_get(memory_id, &mem) != 0)
      return -1;

   config_t embed_cfg;
   config_load(&embed_cfg);
   const char *embed_cmd = config_embedding_command(&embed_cfg, NULL);
   int64_t semantic_ids[128];
   double semantic_scores[128];
   int semantic_hit_count = 0;
   memory_t scratch[1];
   scratch[0] = mem;
   if (memory_collect_semantic_matches(query, embed_cmd, 16, scratch, 1, 1, semantic_ids,
                                       semantic_scores, &semantic_hit_count, 128) < 0)
      return -1;

   char norm_query[512];
   normalize_key(query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", query);

   out->memory = mem;
   memory_ranker_input_t ri = memory_ranker_input_from(&mem);
   memory_compute_score_parts(query, norm_query, &ri, semantic_ids, semantic_scores,
                              semantic_hit_count, NULL, 0, &out->parts);
   out->parts.confidence = mem.confidence;
   return 0;
}

static void memory_pick_answer_from_event(int64_t memory_id, memory_query_intent_t intent,
                                          char *buf, size_t buf_len)
{
   db2_memory_event_frame_row_t rows[1];
   if (db2_memory_event_frames_list(memory_id, rows, 1) <= 0)
      return;
   const char *actor = rows[0].actor;
   const char *action = rows[0].action;
   const char *object = rows[0].object;
   const char *location = rows[0].location;
   const char *event_time = rows[0].event_time;
   if (intent == MEM_QUERY_TEMPORAL && event_time[0])
      snprintf(buf, buf_len, "%s", event_time);
   else if (intent == MEM_QUERY_ENTITY && actor[0])
      snprintf(buf, buf_len, "%s", actor);
   else if (location[0] && (strstr(action, "move") || strstr(action, "visit")))
      snprintf(buf, buf_len, "%s", location);
   else if (object[0])
      snprintf(buf, buf_len, "%s", object);
}

static int memory_cluster_member(const memory_t *matches, int count, int anchor_idx,
                                 int candidate_idx)
{
   if (!matches || anchor_idx < 0 || candidate_idx < 0 || anchor_idx >= count ||
       candidate_idx >= count)
      return 0;
   if (anchor_idx == candidate_idx)
      return 1;
   if (!matches[anchor_idx].source_session[0] || !matches[candidate_idx].source_session[0])
      return 0;
   return strcmp(matches[anchor_idx].source_session, matches[candidate_idx].source_session) == 0;
}

static double memory_answer_cluster_score(const memory_t *matches, int count, int anchor_idx)
{
   if (!matches || anchor_idx < 0 || anchor_idx >= count)
      return -1.0;
   double score = 1.0 / (double)(anchor_idx + 1);
   int cluster_count = 0;
   int kind_variety = 0;
   int has_episode = 0;
   int has_fact = 0;
   for (int i = 0; i < count; i++)
   {
      if (!memory_cluster_member(matches, count, anchor_idx, i))
         continue;
      cluster_count++;
      score += 0.35 / (double)(i + 1);
      if (strcmp(matches[i].kind, matches[anchor_idx].kind) != 0)
         kind_variety = 1;
      if (strcmp(matches[i].kind, KIND_EPISODE) == 0)
         has_episode = 1;
      if (strcmp(matches[i].kind, KIND_FACT) == 0)
         has_fact = 1;
   }
   if (cluster_count > 1)
      score += (double)(cluster_count > 4 ? 4 : cluster_count) * 0.08;
   if (kind_variety)
      score += 0.10;
   if (has_episode)
      score += 0.12;
   if (has_fact)
      score += 0.08;
   return score;
}

static int memory_answer_anchor_index(const memory_t *matches, int count)
{
   if (!matches || count <= 0)
      return 0;
   int anchor = 0;
   double best = memory_answer_cluster_score(matches, count, 0);
   for (int i = 1; i < count; i++)
   {
      double score = memory_answer_cluster_score(matches, count, i);
      if (score > best)
      {
         best = score;
         anchor = i;
      }
   }
   return anchor;
}

static int memory_collect_answer_citation_ids(const memory_t *matches, int count, int anchor_idx,
                                              int64_t *out, int max)
{
   if (!matches || count <= 0 || !out || max <= 0)
      return 0;

   int added = 0;
   for (int pass = 0; pass < 2 && added < max; pass++)
   {
      for (int i = 0; i < count && added < max; i++)
      {
         if ((pass == 0 && !memory_cluster_member(matches, count, anchor_idx, i)) ||
             (pass == 1 && i != anchor_idx))
            continue;
         int dup = 0;
         for (int j = 0; j < added; j++)
         {
            if (out[j] == matches[i].id)
            {
               dup = 1;
               break;
            }
         }
         if (dup)
            continue;
         out[added++] = matches[i].id;
      }
   }
   return added;
}

static char *memory_render_cited_answer(const char *answer, const int64_t *citation_ids,
                                        int citation_count)
{
   if (!answer)
      answer = "";
   if (!citation_ids || citation_count <= 0)
      return safe_strdup(answer);

   size_t answer_len = strlen(answer);
   size_t cap = answer_len + 64 + ((size_t)citation_count * 28);
   char *out = malloc(cap);
   if (!out)
      return safe_strdup(answer);

   size_t pos = 0;
   if (answer_len > 0)
   {
      memcpy(out, answer, answer_len);
      pos = answer_len;
   }
   out[pos++] = ' ';
   out[pos++] = '[';
   for (int i = 0; i < citation_count; i++)
   {
      int written =
          snprintf(out + pos, cap - pos, "%s#%lld", i > 0 ? ", " : "", (long long)citation_ids[i]);
      if (written < 0 || (size_t)written >= cap - pos)
      {
         free(out);
         return safe_strdup(answer);
      }
      pos += (size_t)written;
   }
   out[pos++] = ']';
   out[pos] = '\0';
   return out;
}

static double memory_answer_confidence(const memory_t *matches, int count, int anchor_idx,
                                       int citation_count, int no_answer, int low_confidence)
{
   if (no_answer || !matches || count <= 0 || anchor_idx < 0 || anchor_idx >= count)
      return 0.0;

   double cluster = memory_answer_cluster_score(matches, count, anchor_idx);
   if (cluster < 0.0)
      cluster = 0.0;
   double cluster_norm = cluster / 2.0;
   if (cluster_norm > 1.0)
      cluster_norm = 1.0;

   double rank_norm = 1.0 / (double)(anchor_idx + 1);
   double support_norm = (double)(count > 4 ? 4 : count) / 4.0;
   double citation_norm = citation_count > 0 ? 1.0 : (low_confidence ? 0.0 : 0.35);

   double conf =
       0.45 * cluster_norm + 0.20 * rank_norm + 0.20 * support_norm + 0.15 * citation_norm;
   if (low_confidence)
      conf *= 0.45;
   if (conf < 0.0)
      conf = 0.0;
   if (conf > 1.0)
      conf = 1.0;
   return conf;
}

static const char *memory_answer_decision_name(memory_answer_decision_t decision)
{
   switch (decision)
   {
   case MEMORY_ANSWER_DECISION_ANSWERABLE:
      return "answerable";
   case MEMORY_ANSWER_DECISION_ABSTAIN:
      return "abstain";
   case MEMORY_ANSWER_DECISION_EXEMPT:
      return "exempt";
   }
   return "abstain";
}

static const char *memory_answer_reason_name(memory_answer_reason_t reason)
{
   switch (reason)
   {
   case MEMORY_ANSWER_REASON_OK:
      return "ok";
   case MEMORY_ANSWER_REASON_STRUCTURAL_EMPTY:
      return "structural_empty";
   case MEMORY_ANSWER_REASON_STRUCTURAL_NO_EXTRACT:
      return "structural_no_extract";
   case MEMORY_ANSWER_REASON_CITATION_REQUIRED:
      return "citation_required";
   case MEMORY_ANSWER_REASON_GROUNDING_LOW:
      return "grounding_low";
   case MEMORY_ANSWER_REASON_CHUNK_FLOOR:
      return "chunk_floor";
   case MEMORY_ANSWER_REASON_CURATED_EXEMPT:
      return "curated_exempt";
   case MEMORY_ANSWER_REASON_DB_UNAVAILABLE:
      return "db_unavailable";
   }
   return "ok";
}

const char *memory_answer_evidence_decision_str(const memory_answer_evidence_t *trace)
{
   return memory_answer_decision_name(trace ? trace->decision : MEMORY_ANSWER_DECISION_ABSTAIN);
}

const char *memory_answer_evidence_reason_str(const memory_answer_evidence_t *trace)
{
   return memory_answer_reason_name(trace ? trace->reason : MEMORY_ANSWER_REASON_DB_UNAVAILABLE);
}

static int memory_fixed_field_eq(const char *field, size_t field_len, const char *value)
{
   if (!field || !value)
      return 0;
   size_t value_len = strlen(value);
   if (value_len >= field_len)
      return 0;
   return memcmp(field, value, value_len) == 0 && field[value_len] == '\0';
}

static const char *memory_answer_mode_for_anchor(const memory_t *matches, int count, int anchor)
{
   if (!matches || anchor < 0 || anchor >= count)
      return "";
   const memory_t *m = &matches[anchor];
   if (memory_fixed_field_eq(m->tier, sizeof(m->tier), TIER_L5) ||
       memory_fixed_field_eq(m->kind, sizeof(m->kind), "synthesis") ||
       memory_fixed_field_eq(m->kind, sizeof(m->kind), "restoration") ||
       memory_fixed_field_eq(m->provenance_category, sizeof(m->provenance_category), "synthesis") ||
       memory_fixed_field_eq(m->provenance_category, sizeof(m->provenance_category), "restoration"))
      return "synthesised";
   return "verbatim";
}

static double memory_text_term_coverage(const char **terms, int term_count, const char *a,
                                        const char *b)
{
   int significant = 0;
   int covered = 0;
   for (int t = 0; t < term_count; t++)
   {
      const char *term = terms[t];
      if (!term || strlen(term) < 3 || is_coverage_stopword(term))
         continue;
      significant++;
      char lower_term[128];
      snprintf(lower_term, sizeof(lower_term), "%s", term);
      for (char *p = lower_term; *p; p++)
         *p = (char)tolower((unsigned char)*p);
      int found = 0;
      const char *texts[2] = {a ? a : "", b ? b : ""};
      for (int i = 0; i < 2 && !found; i++)
      {
         char lower_text[2048];
         snprintf(lower_text, sizeof(lower_text), "%s", texts[i]);
         for (char *p = lower_text; *p; p++)
            *p = (char)tolower((unsigned char)*p);
         if (strstr(lower_text, lower_term))
            found = 1;
      }
      if (found)
         covered++;
   }
   return significant > 0 ? (double)covered / (double)significant : 0.0;
}

static void memory_answer_trace_init(memory_answer_evidence_t *trace, const memory_t *matches,
                                     int count, int anchor)
{
   if (!trace)
      return;
   memset(trace, 0, sizeof(*trace));
   trace->decision = MEMORY_ANSWER_DECISION_ANSWERABLE;
   trace->reason = MEMORY_ANSWER_REASON_OK;
   trace->anchor_id = 0;
   trace->anchor_rank = -1;
   trace->ranked_count = count > 0 ? count : 0;
   if (matches && count > 0)
   {
      int cap = count < MEMORY_ANSWER_TRACE_MAX_IDS ? count : MEMORY_ANSWER_TRACE_MAX_IDS;
      for (int i = 0; i < cap; i++)
         trace->candidate_ids[i] = matches[i].id;
      trace->candidate_id_count = cap;
      trace->trace_truncated = count > MEMORY_ANSWER_TRACE_MAX_IDS ? 1 : 0;
      if (anchor >= 0 && anchor < count)
      {
         trace->anchor_id = matches[anchor].id;
         trace->anchor_rank =
             matches[anchor].hybrid_rank > 0 ? matches[anchor].hybrid_rank : anchor + 1;
      }
   }
}

static int memory_answer_anchor_exempt(const memory_t *matches, int count, int anchor)
{
   if (!matches || anchor < 0 || anchor >= count)
      return 0;
   return memory_tier_priority(matches[anchor].tier) >= 4;
}

static void memory_answer_trace_set(memory_answer_evidence_t *trace,
                                    memory_answer_decision_t decision,
                                    memory_answer_reason_t reason, int structural, int exempt)
{
   if (!trace)
      return;
   trace->decision = decision;
   trace->reason = reason;
   trace->structural = structural;
   trace->exempt = exempt;
}

static void memory_answerable(const memory_t *matches, int count, int anchor,
                              const char **query_terms, int term_count, int citation_count,
                              int citations_required, int structural_failure,
                              memory_answer_reason_t structural_reason, double threshold,
                              double chunk_floor, int abstain_enabled, double answer_confidence,
                              memory_answer_evidence_t *trace)
{
   memory_answer_trace_init(trace, matches, count, anchor);
   if (!trace)
      return;
   trace->threshold = threshold;
   trace->chunk_floor = chunk_floor;

   if (memory_answer_anchor_exempt(matches, count, anchor))
   {
      memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_EXEMPT,
                              MEMORY_ANSWER_REASON_CURATED_EXEMPT, 0, 1);
      return;
   }
   if (structural_failure)
   {
      memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_ABSTAIN, structural_reason, 1, 0);
      return;
   }
   if (citations_required && citation_count <= 0)
   {
      memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_ABSTAIN,
                              MEMORY_ANSWER_REASON_CITATION_REQUIRED, 1, 0);
      return;
   }

   context_candidate_t cands[16];
   int cand_count = count < 16 ? count : 16;
   int kept = 0;
   for (int i = 0; i < cand_count; i++)
   {
      double score = matches[i].retrieval_score;
      if (chunk_floor > 0.0 && score < chunk_floor)
         continue;
      memset(&cands[kept], 0, sizeof(cands[kept]));
      cands[kept].id = matches[i].id;
      snprintf(cands[kept].key, sizeof(cands[kept].key), "%s", matches[i].key);
      snprintf(cands[kept].content, sizeof(cands[kept].content), "%s", matches[i].content);
      cands[kept].score = score > 0.0 ? score : (double)(cand_count - i);
      kept++;
   }
   if (chunk_floor > 0.0 && kept <= 0)
   {
      memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_ABSTAIN,
                              MEMORY_ANSWER_REASON_CHUNK_FLOOR, 0, 0);
      return;
   }

   retrieval_confidence_t rconf;
   memory_retrieval_confidence(query_terms, term_count, cands, kept, threshold, &rconf);
   trace->topk_grounding = rconf.score;
   if (matches && anchor >= 0 && anchor < count)
      trace->anchor_coverage = memory_text_term_coverage(
          query_terms, term_count, matches[anchor].key, matches[anchor].content);

   int cluster_count = 0;
   int cluster_covered = 0;
   for (int t = 0; t < term_count; t++)
   {
      const char *term = query_terms[t];
      if (!term || strlen(term) < 3 || is_coverage_stopword(term))
         continue;
      cluster_count++;
      for (int i = 0; i < count; i++)
      {
         if (!memory_cluster_member(matches, count, anchor, i))
            continue;
         if (memory_text_term_coverage(&term, 1, matches[i].key, matches[i].content) > 0.0)
         {
            cluster_covered++;
            break;
         }
      }
   }
   trace->cluster_coverage =
       cluster_count > 0 ? (double)cluster_covered / (double)cluster_count : 0.0;

   if (!abstain_enabled)
   {
      memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_ANSWERABLE, MEMORY_ANSWER_REASON_OK, 0,
                              0);
      return;
   }

   double grounding = trace->anchor_coverage < trace->cluster_coverage ? trace->anchor_coverage
                                                                       : trace->cluster_coverage;
   double floored = grounding < trace->topk_grounding ? grounding : trace->topk_grounding;
   const double epsilon = 0.02;
   if (floored < threshold)
   {
      if (threshold - floored < epsilon && answer_confidence >= threshold)
      {
         memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_ANSWERABLE, MEMORY_ANSWER_REASON_OK,
                                 0, 0);
         return;
      }
      memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_ABSTAIN,
                              MEMORY_ANSWER_REASON_GROUNDING_LOW, 0, 0);
      return;
   }
   memory_answer_trace_set(trace, MEMORY_ANSWER_DECISION_ANSWERABLE, MEMORY_ANSWER_REASON_OK, 0, 0);
}

static void memory_pick_answer_from_cluster(const memory_t *matches, int count,
                                            memory_query_intent_t intent, char *buf, size_t buf_len)
{
   if (!matches || count <= 0 || !buf || buf_len == 0)
      return;
   int anchor = memory_answer_anchor_index(matches, count);

   for (int pass = 0; pass < 2 && !buf[0]; pass++)
   {
      for (int i = 0; i < count && !buf[0]; i++)
      {
         if ((pass == 0 && !memory_cluster_member(matches, count, anchor, i)) ||
             (pass == 1 && i != anchor))
            continue;
         memory_pick_answer_from_event(matches[i].id, intent, buf, buf_len);
      }
   }

   if (!buf[0] && intent == MEM_QUERY_TEMPORAL)
   {
      for (int pass = 0; pass < 2 && !buf[0]; pass++)
      {
         for (int i = 0; i < count && !buf[0]; i++)
         {
            if ((pass == 0 && !memory_cluster_member(matches, count, anchor, i)) ||
                (pass == 1 && i != anchor))
               continue;
            db2_memory_pick_first_temporal_ref(matches[i].id, buf, (int)buf_len);
         }
      }
   }

   if (!buf[0])
   {
      db2_memory_summary_row_t row;
      for (int pass = 0; pass < 2 && !buf[0]; pass++)
      {
         for (int i = 0; i < count && !buf[0]; i++)
         {
            if ((pass == 0 && !memory_cluster_member(matches, count, anchor, i)) ||
                (pass == 1 && i != anchor))
               continue;
            if (db2_memory_summaries_list(matches[i].id, 1, &row, 1) > 0 && row.summary[0])
               snprintf(buf, buf_len, "%s", row.summary);
         }
      }
   }

   if (!buf[0])
      snprintf(buf, buf_len, "%.*s", 180,
               matches[anchor].content[0] ? matches[anchor].content : matches[anchor].key);
}

char *memory_answer_query(const char *query, int limit)
{
   return memory_answer_query_scoped(query, NULL, NULL, limit);
}

int memory_ask_query(const char *query, int limit, memory_answer_result_t *out)
{
   int rc = memory_ask_query_scoped(query, NULL, NULL, limit, out);
   if (rc == 0 && out)
      dogfood_log_moment_live("memory_ask", query, out->citation_ids, out->citation_count, NULL);
   return rc;
}

int memory_ask_query_scoped(const char *query, const char *scope_type, const char *scope_value,
                            int limit, memory_answer_result_t *out)
{
   if (!out)
      return -1;

   memset(out, 0, sizeof(*out));
   out->confidence = 0.0;

   if (!query || !query[0])
   {
      snprintf(out->error, sizeof(out->error), "query is required");
      return -1;
   }

   memory_t matches[8];
   int count =
       memory_find_facts_scoped(query, scope_type, scope_value, limit > 0 ? limit : 5, matches, 8);
   if (count < 0)
   {
      snprintf(out->error, sizeof(out->error),
               "error: memory retrieval index unavailable; server-side maintenance is required");
      memory_answer_trace_init(&out->evidence, NULL, 0, -1);
      memory_answer_trace_set(&out->evidence, MEMORY_ANSWER_DECISION_ABSTAIN,
                              MEMORY_ANSWER_REASON_DB_UNAVAILABLE, 1, 0);
      return -1;
   }
   out->retrieval_count = count;
   if (count <= 0)
   {
      out->no_answer = 1;
      memory_answer_trace_init(&out->evidence, NULL, 0, -1);
      memory_answer_trace_set(&out->evidence, MEMORY_ANSWER_DECISION_ABSTAIN,
                              MEMORY_ANSWER_REASON_STRUCTURAL_EMPTY, 1, 0);
      return 0;
   }

   char norm_query[512];
   normalize_key(query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", query);
   memory_query_intent_t intent = memory_query_intent(query, norm_query);

   memory_pick_answer_from_cluster(matches, count, intent, out->answer, sizeof(out->answer));
   if (!out->answer[0])
   {
      out->no_answer = 1;
      int structural_anchor = memory_answer_anchor_index(matches, count);
      memory_answer_trace_init(&out->evidence, matches, count, structural_anchor);
      memory_answer_trace_set(&out->evidence, MEMORY_ANSWER_DECISION_ABSTAIN,
                              MEMORY_ANSWER_REASON_STRUCTURAL_NO_EXTRACT, 1, 0);
      return 0;
   }

   config_t cfg;
   config_load(&cfg);
   const char *citations_mode = getenv("AIMEE_MEMORY_CITATIONS_MODE");
   if (!citations_mode || !citations_mode[0])
      citations_mode = cfg.memory_citations_mode;
   int strip_unverified = cfg.memory_citations_strip_unverified;
   const char *strip_env = getenv("AIMEE_MEMORY_CITATIONS_STRIP_UNVERIFIED");
   if (strip_env && strip_env[0])
      strip_unverified = atoi(strip_env) != 0;

   int anchor = memory_answer_anchor_index(matches, count);
   snprintf(out->evidence_mode, sizeof(out->evidence_mode), "%s",
            memory_answer_mode_for_anchor(matches, count, anchor));
   out->citation_count = memory_collect_answer_citation_ids(
       matches, count, anchor, out->citation_ids, MEMORY_ANSWER_MAX_CITATIONS);

   int is_required =
       (citations_mode && citations_mode[0] && strcmp(citations_mode, "required") == 0);
   int reprompt_on_miss = cfg.memory_citations_reprompt_on_miss;
   if (is_required)
      memory_runtime_state_increment("memory.citation.required", 1);

   if (out->citation_count <= 0 && is_required && reprompt_on_miss)
   {
      memory_runtime_state_increment("memory.citation.reprompted", 1);
      memory_t reprompt_matches[8];
      int reprompt_count = memory_find_facts(query, 5, reprompt_matches, 8);
      if (reprompt_count < 0)
      {
         snprintf(out->error, sizeof(out->error),
                  "error: memory retrieval index unavailable; server-side maintenance is required");
         memory_answer_trace_init(&out->evidence, matches, count, anchor);
         memory_answer_trace_set(&out->evidence, MEMORY_ANSWER_DECISION_ABSTAIN,
                                 MEMORY_ANSWER_REASON_DB_UNAVAILABLE, 1, 0);
         return -1;
      }
      if (reprompt_count > 0)
      {
         char reprompt_answer[sizeof(out->answer)];
         reprompt_answer[0] = '\0';
         memory_pick_answer_from_cluster(reprompt_matches, reprompt_count, intent, reprompt_answer,
                                         sizeof(reprompt_answer));
         int reprompt_anchor = memory_answer_anchor_index(reprompt_matches, reprompt_count);
         int reprompt_citations =
             memory_collect_answer_citation_ids(reprompt_matches, reprompt_count, reprompt_anchor,
                                                out->citation_ids, MEMORY_ANSWER_MAX_CITATIONS);
         if (reprompt_answer[0] && reprompt_citations > 0)
         {
            snprintf(out->answer, sizeof(out->answer), "%s", reprompt_answer);
            out->citation_count = reprompt_citations;
            memcpy(matches, reprompt_matches, sizeof(reprompt_matches));
            count = reprompt_count;
            anchor = reprompt_anchor;
            out->retrieval_count = reprompt_count;
         }
      }
   }

   int citation_required_abstain = 0;
   if (out->citation_count <= 0 && citations_mode && citations_mode[0] &&
       strcmp(citations_mode, "off") != 0)
   {
      memory_runtime_state_increment("memory.citation.missing", 1);
      if (strip_unverified)
      {
         memory_runtime_state_increment("memory.citation.stripped", 1);
         out->answer[0] = '\0';
         out->no_answer = 1;
         out->confidence = 0.0;
         memory_answerable(matches, count, anchor, NULL, 0, 0, is_required, 1,
                           MEMORY_ANSWER_REASON_CITATION_REQUIRED, 0.0, 0.0, 0, 0.0,
                           &out->evidence);
         return 0;
      }
      if (is_required)
      {
         citation_required_abstain = 1;
         out->low_confidence = 1;
      }
   }

   if (out->citation_count > 0)
      memory_runtime_state_increment("memory.citation.verified", 1);
   out->confidence = memory_answer_confidence(matches, count, anchor, out->citation_count, 0,
                                              out->low_confidence);

   double threshold = cfg.memory_abstain_gate > 0.0 ? cfg.memory_abstain_gate : 0.40;
   double chunk_floor =
       cfg.memory_chunk_min_confidence > 0.0 ? cfg.memory_chunk_min_confidence : 0.0;
   if (chunk_floor > 1.0)
      chunk_floor = 1.0;
   char query_terms_storage[32][64];
   const char *query_terms[32];
   int term_count = memory_split_tokens(norm_query, query_terms_storage, 32);
   for (int i = 0; i < term_count; i++)
      query_terms[i] = query_terms_storage[i];
   int abstain_enabled = cfg.memory_abstain_enabled && threshold > 0.0;
   if (!citations_mode || !citations_mode[0] || strcmp(citations_mode, "off") == 0)
      citation_required_abstain = 0;
   double active_chunk_floor = abstain_enabled ? chunk_floor : 0.0;
   memory_answerable(matches, count, anchor, query_terms, term_count, out->citation_count,
                     citation_required_abstain, 0, MEMORY_ANSWER_REASON_OK, threshold,
                     active_chunk_floor, abstain_enabled, out->confidence, &out->evidence);
   if (out->evidence.decision == MEMORY_ANSWER_DECISION_ABSTAIN)
   {
      out->no_answer = 1;
      out->low_confidence = 1;
      out->answer[0] = '\0';
      out->citation_count = 0;
      memory_runtime_state_increment("memory.answer.abstained", 1);
   }
   else if (citation_required_abstain && out->citation_count <= 0)
   {
      char low_answer[sizeof(out->answer)];
      snprintf(low_answer, sizeof(low_answer),
               "## Retrieval Confidence: LOW\n"
               "No verified citations could be attached to this answer. "
               "The information below is unverified and may be inaccurate.\n\n%s",
               out->answer);
      snprintf(out->answer, sizeof(out->answer), "%s", low_answer);
   }
   return 0;
}

char *memory_answer_query_scoped(const char *query, const char *scope_type, const char *scope_value,
                                 int limit)
{
   memory_answer_result_t result;
   if (memory_ask_query_scoped(query, scope_type, scope_value, limit, &result) != 0)
      return safe_strdup(result.error[0] ? result.error : "");
   if (result.no_answer)
   {
      char msg[768];
      if (query && query[0])
         snprintf(msg, sizeof(msg), "No confident answer for \"%s\"", query);
      else
         snprintf(msg, sizeof(msg), "No confident answer.");
      return safe_strdup(msg);
   }
   if (result.citation_count > 0)
      return memory_render_cited_answer(result.answer, result.citation_ids, result.citation_count);
   return safe_strdup(result.answer);
}
