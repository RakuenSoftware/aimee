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

/* Time decay: exponential decay based on days since last use */
static double time_decay(const char *last_used)
{
   if (!last_used || !last_used[0])
      return 0.5;

   /* Parse YYYY-MM-DD from timestamp */
   struct tm tm_used;
   memset(&tm_used, 0, sizeof(tm_used));
   if (sscanf(last_used, "%d-%d-%d", &tm_used.tm_year, &tm_used.tm_mon, &tm_used.tm_mday) < 3)
      return 0.5;

   tm_used.tm_year -= 1900;
   tm_used.tm_mon -= 1;
   time_t t_used = timegm(&tm_used);
   time_t t_now = time(NULL);

   double days = difftime(t_now, t_used) / 86400.0;
   if (days < 0)
      days = 0;

   return exp(-0.02 * days);
}

static int memory_search_impl(char **clusters, int cluster_count, int limit, search_result_t *out,
                              int max);

/* Public entry: time the whole search and log a one-line stage breakdown so the
 * embed-spawn cost (the suspected dominant term) is separable from the rest of
 * retrieval/rerank. Counters live in memory_core_helpers.inc (same TU). */
int memory_search(char **clusters, int cluster_count, int limit, search_result_t *out, int max)
{
   long long _t0 = util_now_ms();
   s_qembed_ms = 0;
   s_qembed_spawns = 0;
   int n = memory_search_impl(clusters, cluster_count, limit, out, max);
   aimee_log(LOG_INFO, "memory.search.timing", "total=%lldms embed=%lldms spawns=%d results=%d",
             util_now_ms() - _t0, s_qembed_ms, s_qembed_spawns, n);
   return n;
}

static int memory_search_impl(char **clusters, int cluster_count, int limit, search_result_t *out,
                              int max)
{
   if (!clusters || cluster_count <= 0)
      return 0;
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   /* Parse cluster terms and load stopwords */
   char *all_terms[256];
   int term_count = 0;

   for (int ci = 0; ci < cluster_count && term_count < 256; ci++)
   {
      char *tok_buf[64];
      int tc = tokenize_for_search(clusters[ci], tok_buf, 64);
      for (int t = 0; t < tc && term_count < 256; t++)
         all_terms[term_count++] = tok_buf[t];
   }

   if (term_count == 0)
      return 0;

   /* Load promoted stopwords from DB2 (typed API). */
   char stopwords[256][32];
   int stop_count = db2_stopwords_list(stopwords, 256);

   /* Filter out stopwords from terms */
   char *filtered[256];
   int fcount = 0;
   for (int i = 0; i < term_count; i++)
   {
      int is_stop = 0;
      for (int s = 0; s < stop_count; s++)
      {
         if (strcasecmp(all_terms[i], stopwords[s]) == 0)
         {
            is_stop = 1;
            break;
         }
      }
      if (!is_stop)
         filtered[fcount++] = all_terms[i];
      else
         free(all_terms[i]);
   }

   if (fcount == 0)
      return 0;
   if (!db1_windows_find_candidates_by_terms || !db1_windows_find_lexical_hits ||
       !db1_window_list_files)
   {
      for (int i = 0; i < fcount; i++)
         free(filtered[i]);
      return 0;
   }

   typedef struct
   {
      int64_t window_id;
      char session_id[128];
      int seq;
      char summary[1024];
      char created_at[32];
      int match_count;
      double score;
   } candidate_t;

   candidate_t candidates[200];
   db1_window_search_candidate_t db1_candidates[200];
   int cand_count = db1_windows_find_candidates_by_terms((const char *const *)filtered, fcount,
                                                         db1_candidates, 200);
   if (cand_count < 0)
      cand_count = 0;

   for (int i = 0; i < cand_count; i++)
   {
      candidate_t *c = &candidates[i];
      c->window_id = db1_candidates[i].window_id;
      c->seq = db1_candidates[i].seq;
      c->match_count = db1_candidates[i].match_count;
      snprintf(c->session_id, sizeof(c->session_id), "%s", db1_candidates[i].session_id);
      snprintf(c->summary, sizeof(c->summary), "%s", db1_candidates[i].summary);
      snprintf(c->created_at, sizeof(c->created_at), "%s", db1_candidates[i].created_at);

      /* Score: term match * cluster boost * time decay */
      double term_score = (double)c->match_count / (double)fcount;
      double t_decay = time_decay(c->created_at);
      c->score = term_score * 2.0 * t_decay;
   }

   db1_window_lexical_hit_t lexical_hits[200];
   int lexical_count =
       db1_windows_find_lexical_hits((const char *const *)filtered, fcount, lexical_hits, 200);
   if (lexical_count > 0)
   {
      for (int h = 0; h < lexical_count; h++)
      {
         /* rank is negative (lower = better), normalize; clamp denominator
          * to avoid div-by-zero. */
         double denom = 1.0 - lexical_hits[h].rank;
         if (denom < 0.001)
            denom = 0.001;
         double lexical_score = 1.0 / denom;

         for (int i = 0; i < cand_count; i++)
         {
            if (candidates[i].window_id == lexical_hits[h].window_id)
            {
               candidates[i].score += lexical_score;
               break;
            }
         }
      }
   }

   /* Sort by score descending */
   for (int i = 0; i < cand_count - 1; i++)
   {
      for (int j = i + 1; j < cand_count; j++)
      {
         if (candidates[j].score > candidates[i].score)
         {
            candidate_t tmp = candidates[i];
            candidates[i] = candidates[j];
            candidates[j] = tmp;
         }
      }
   }

   /* Take top N */
   int result_count = cand_count < limit ? cand_count : limit;

   /* Zero-initialize output before populating */
   memset(out, 0, (size_t)result_count * sizeof(out[0]));

   for (int i = 0; i < result_count; i++)
   {
      out[i].file_count = db1_window_list_files(candidates[i].window_id, out[i].files, 32);
      if (out[i].file_count < 0)
         out[i].file_count = 0;
   }

   /* Apply graph boost from entity_edges (outbound neighbors per term). */
   for (int i = 0; i < result_count; i++)
   {
      double boost = 0.0;
      for (int t = 0; t < fcount; t++)
      {
         db2_entity_neighbor_t neighbors[50];
         int n = db2_entity_edge_outbound_neighbors(filtered[t], neighbors, 50, 50);
         for (int b = 0; b < n; b++)
         {
            if (strstr(candidates[i].summary, neighbors[b].node))
               boost += 0.1 * neighbors[b].weight;
         }
      }
      candidates[i].score += boost;
   }

   /* Re-sort after boost */
   for (int i = 0; i < result_count - 1; i++)
   {
      for (int j = i + 1; j < result_count; j++)
      {
         if (candidates[j].score > candidates[i].score)
         {
            candidate_t tmp = candidates[i];
            candidates[i] = candidates[j];
            candidates[j] = tmp;

            /* Swap file data too */
            search_result_t ftmp = out[i];
            out[i] = out[j];
            out[j] = ftmp;
         }
      }
   }

   /* Fill output */
   for (int i = 0; i < result_count; i++)
   {
      snprintf(out[i].session_id, sizeof(out[i].session_id), "%s", candidates[i].session_id);
      out[i].seq = candidates[i].seq;
      snprintf(out[i].summary, sizeof(out[i].summary), "%s", candidates[i].summary);
      out[i].score = candidates[i].score;
      out[i].start_line = 0;
      out[i].end_line = 0;
      out[i].file_path[0] = '\0';
   }

   /* Free terms */
   for (int i = 0; i < fcount; i++)
      free(filtered[i]);

   return result_count;
}

/* --- Fact Search --- */

/* Static expansion dictionary for common technical terms to handle semantic drift. */
static const struct
{
   const char *term;
   const char *expansion;
} expansion_dict[] = {{"deploy", "install setup release ship rollout"},
                      {"database", "data store persistence storage sql"},
                      {"auth", "login password permission access pam"},
                      {"network", "ip port host address socket sse websocket"},
                      {"build", "make compile install artifact"},
                      {"test", "unit integration bench eval"},
                      {"memory", "context fact preference L2 L3"},
                      {NULL, NULL}};

void memory_expand_synonyms(const char *query, char *out, size_t out_len)
{
   if (!query || !out || out_len == 0)
      return;

   char lower[512];
   snprintf(lower, sizeof(lower), "%s", query);
   for (int i = 0; lower[i]; i++)
      lower[i] = (char)tolower((unsigned char)lower[i]);

   int pos = snprintf(out, out_len, "%s", query);

   for (int i = 0; expansion_dict[i].term; i++)
   {
      if (strstr(lower, expansion_dict[i].term))
      {
         int remaining = (int)out_len - pos - 8;
         if (remaining > 0)
         {
            pos += snprintf(out + pos, out_len - pos, " LEX_OR %s", expansion_dict[i].expansion);
         }
      }
   }
}

int memory_find_facts_like(const char *query, int limit, memory_t *out, int max)
{
   return db2_memory_find_facts_like(query, limit, out, max);
}

int memory_append_unique(memory_t *out, int count, int max, const memory_t *candidate)
{
   if (!out || !candidate || count >= max)
      return count;

   for (int i = 0; i < count; i++)
   {
      if (out[i].id == candidate->id)
         return count;
   }

   out[count] = *candidate;
   return count + 1;
}

/* The five term-based collectors share one body: fetch up to a stack scratch
 * buffer via the matching db2 query, then append-unique into the caller's
 * result set. They differ only in which db2 collector they invoke, so route
 * them through one helper that takes the db2 query as a function pointer.
 * All five db2_memory_collect_*_matches share this signature. */
typedef int (*memory_db2_collect_fn)(const char *term, int limit, memory_t *out, int max);

static int memory_collect_via(memory_db2_collect_fn collect, const char *term, int limit,
                              memory_t *out, int count, int max)
{
   if (!term || !term[0] || !out || count >= max)
      return count;
   MEMORY_AUTOFREE memory_t *scratch = calloc(64, sizeof(*scratch));
   if (!scratch)
      return count;
   int cap = 64;
   int got = collect(term, limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

int memory_collect_alias_matches(const char *alias, int limit, memory_t *out, int count, int max)
{
   return memory_collect_via(db2_memory_collect_alias_matches, alias, limit, out, count, max);
}

int memory_collect_entity_matches(const char *term, int limit, memory_t *out, int count, int max)
{
   return memory_collect_via(db2_memory_collect_entity_matches, term, limit, out, count, max);
}

int memory_collect_temporal_matches(const char *term, int limit, memory_t *out, int count, int max)
{
   return memory_collect_via(db2_memory_collect_temporal_matches, term, limit, out, count, max);
}

int memory_collect_summary_matches(const char *term, int limit, memory_t *out, int count, int max)
{
   return memory_collect_via(db2_memory_collect_summary_matches, term, limit, out, count, max);
}

int memory_collect_event_frame_matches(const char *term, int limit, memory_t *out, int count,
                                       int max)
{
   return memory_collect_via(db2_memory_collect_event_frame_matches, term, limit, out, count, max);
}

/* Forward declarations for the pgvector-routing helpers defined later in
 * the file (next to the other vector-search helpers). */

/* Chunks are not separately indexed in pgvector yet, so chunk_matches falls
 * back to memory-level semantic search via memory_collect_memory_matches_via_vector —
 * the caller still tags the hits with MEM_SOURCE_CHUNK. Future work: upsert
 * chunk embeddings to pgvector under a record_type='chunk' key for true
 * chunk-level retrieval. */
int memory_collect_chunk_matches(const char *query, int limit, memory_t *out, int count, int max)
{
   if (!query || !query[0] || !out || count >= max)
      return count;
   MEMORY_AUTOFREE memory_t *scratch = calloc(64, sizeof(*scratch));
   if (!scratch)
      return count;
   int cap = 64;
   config_t cm_cfg;
   config_load(&cm_cfg);
   const char *embed_cmd = config_embedding_command(&cm_cfg, NULL);
   int got = memory_collect_memory_matches_via_vector(query, embed_cmd, limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

int memory_collect_unit_matches(const char *query, int limit, memory_t *out, int count, int max)
{
   if (!query || !query[0] || !out || count >= max)
      return count;
   char norm_query[256];
   char qtokens[16][64];
   normalize_key(query, norm_query, sizeof(norm_query));
   int nq = memory_split_tokens(norm_query, qtokens, 16);
   int useful = 0;
   for (int i = 0; i < nq; i++)
   {
      if (memory_is_signal_token(qtokens[i]))
         useful++;
   }
   if (useful < 2)
      return count;
   MEMORY_AUTOFREE memory_t *scratch = calloc(64, sizeof(*scratch));
   if (!scratch)
      return count;
   int cap = 64;
   config_t mc_cfg;
   config_load(&mc_cfg);
   const char *embed_cmd = config_embedding_command(&mc_cfg, NULL);
   int got = memory_collect_unit_matches_via_vector(query, embed_cmd, limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

void memory_build_semantic_query_text(const char *raw_query, memory_query_intent_t intent,
                                      char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!raw_query || !raw_query[0])
      return;

   char norm[512];
   char tokens[24][64];
   char expanded[40][64];
   normalize_key(raw_query, norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 24);
   int expanded_count = memory_expand_query_terms(norm, tokens, token_count, expanded, 40);
   const char *intent_tag = "general";
   if (intent == MEM_QUERY_TEMPORAL)
      intent_tag = "temporal";
   else if (intent == MEM_QUERY_ENTITY)
      intent_tag = "entity";
   else if (intent == MEM_QUERY_PROCEDURAL)
      intent_tag = "procedural";

   snprintf(out, out_len, "query intent %s text %s terms", intent_tag, norm[0] ? norm : raw_query);
   size_t used = strlen(out);
   for (int i = 0; i < expanded_count && used + 70 < out_len; i++)
   {
      if (!memory_is_signal_token(expanded[i]))
         continue;
      int already = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(expanded[j], expanded[i]) == 0)
         {
            already = 1;
            break;
         }
      }
      if (already)
         continue;
      used += snprintf(out + used, out_len - used, " %s", expanded[i]);
   }
}

static double memory_unit_semantic_type_boost(const char *unit_type, memory_query_intent_t intent)
{
   if (!unit_type)
      return 0.0;
   if (intent == MEM_QUERY_TEMPORAL)
   {
      if (strcmp(unit_type, "temporal") == 0)
         return 0.18;
      if (strcmp(unit_type, "event") == 0)
         return 0.10;
      if (strcmp(unit_type, "summary") == 0)
         return -0.03;
   }
   else if (intent == MEM_QUERY_ENTITY)
   {
      if (strcmp(unit_type, "entity") == 0)
         return 0.18;
      if (strcmp(unit_type, "event") == 0)
         return 0.08;
   }
   else if (intent == MEM_QUERY_PROCEDURAL)
   {
      if (strcmp(unit_type, "summary") == 0 || strcmp(unit_type, "chunk") == 0)
         return 0.08;
   }
   else
   {
      if (strcmp(unit_type, "event") == 0)
         return 0.05;
      if (strcmp(unit_type, "summary") == 0)
         return 0.03;
   }
   return 0.0;
}

/* Whether the semantic-vector leg can run for a freshly embedded query: it must
 * have produced a vector AND that vector's dimension must match the dimension
 * the memory vectors were stored at (db2_embedding_dim()), so pgvector compares
 * like with like. This generalises the historical `qdim == 384` special-case —
 * which silently disabled semantic recall for every embedder other than the
 * retired 384-d builtin — to whatever single embedder the deployment runs. */
static int memory_semantic_dim_ok(int qdim)
{
   return qdim > 0 && qdim == db2_embedding_dim();
}

/* Cosine-similarity floors below were calibrated for the 384-d builtin embedder.
 * Different embedders occupy different cosine ranges, so a fixed floor that
 * keeps the right hits for one rejects them all for another (pplx-0.6b relevant
 * matches land ~0.35-0.40, well under the 0.62/0.52 builtin-era floors). Scale
 * the floors to the active embedder. config.memory_semantic_floor_scale > 0
 * pins the multiplier (live calibration without a rebuild); 0 auto-scales by the
 * active embedding dimension. Returns a multiplier applied to every floor. */
static double memory_semantic_floor_scale(void)
{
   config_t fs_cfg;
   if (config_load(&fs_cfg) == 0 && fs_cfg.memory_semantic_floor_scale > 0.0)
      return fs_cfg.memory_semantic_floor_scale;
   int dim = db2_embedding_dim();
   if (dim <= 384)
      return 1.0; /* 384-d builtin: the floors are calibrated for this range */
   if (dim <= 1024)
      return 0.55; /* pplx-0.6b: relevant cosines observed ~0.38 (0.62*0.55=0.34) */
   if (dim <= 2560)
      return 0.75; /* pplx-4b: wider than 0.6b but estimated — tune via config */
   return 0.65;    /* unknown embedder: lean permissive over silently dropping */
}

/* Test hooks (declared in memory.h): expose the embedder-aware semantic gate +
 * floor scale so unit tests can pin db2_set_embedding_dim() and assert that the
 * leg runs and the floor tracks the active embedder dimension. */
int memory_semantic_dim_ok_test(int qdim)
{
   return memory_semantic_dim_ok(qdim);
}
double memory_semantic_floor_scale_test(void)
{
   return memory_semantic_floor_scale();
}

/* Visitor state for the ephemeral-mode in-memory cosine scan. The visitor
 * keeps the top hits whose sim >= the embedder-scaled floor, deduplicating by
 * memory_id and resolving the memory row inline. The fixed cap matches the
 * original local array size; once we run out of slots we drop further matches. */
int memory_collect_semantic_matches(const char *query, const char *command, int limit,
                                    memory_t *out, int count, int max, int64_t *semantic_ids,
                                    double *semantic_scores, int *semantic_hit_count,
                                    int semantic_max)
{
   if (!query || !query[0] || !out || count >= max)
      return count;
   const char *embed_cmd = memory_effective_embedding_cmd(command);

   char semantic_query[1024];
   memory_build_semantic_query_text(query, memory_query_intent(query, query), semantic_query,
                                    sizeof(semantic_query));
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(semantic_query[0] ? semantic_query : query, embed_cmd, qvec,
                                        EMBED_MAX_DIM);
   if (qdim <= 0)
      return count;

   if (memory_semantic_dim_ok(qdim))
   {
      const double floor_scale = memory_semantic_floor_scale();
      int64_t ids[128];
      double scores[128];
      int hit_total = pgvec_memory_vector_search_record_type(
          "memory", qvec, qdim, limit > 32 ? limit : 32, ids, scores, 128);
      if (hit_total < 0)
         return memory_semantic_query_unavailable();
      if (hit_total > 0)
      {
         int appended = 0;
         for (int i = 0; i < hit_total && count < max && appended < limit; i++)
         {
            double sim = scores[i];
            if (sim < 0.62 * floor_scale)
               continue;

            memory_t candidate;
            if (memory_fetch_row_by_id(ids[i], &candidate) != 0)
               continue;
            if (semantic_ids && semantic_scores && semantic_hit_count &&
                *semantic_hit_count < semantic_max)
            {
               semantic_ids[*semantic_hit_count] = candidate.id;
               semantic_scores[*semantic_hit_count] = sim;
               (*semantic_hit_count)++;
            }
            count = memory_append_unique(out, count, max, &candidate);
            appended++;
         }
         if (appended > 0)
            return count;
      }
   }

   return count;
}

int memory_collect_unit_semantic_matches(const char *query, const char *command, int limit,
                                         memory_t *out, int count, int max, int64_t *semantic_ids,
                                         double *semantic_scores, int *semantic_hit_count,
                                         int semantic_max)
{
   if (!query || !query[0] || !out || count >= max)
      return count;
   const char *embed_cmd = memory_effective_embedding_cmd(command);

   char norm_query[512];
   normalize_key(query, norm_query, sizeof(norm_query));
   memory_query_intent_t intent = memory_query_intent(query, norm_query);
   char semantic_query[1024];
   memory_build_semantic_query_text(query, intent, semantic_query, sizeof(semantic_query));
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(semantic_query[0] ? semantic_query : query, embed_cmd, qvec,
                                        EMBED_MAX_DIM);
   if (qdim <= 0)
      return count;

   if (memory_semantic_dim_ok(qdim))
   {
      const double floor_scale = memory_semantic_floor_scale();
      int64_t ids[128];
      double vector_scores[128];
      int q_hits = pgvec_memory_vector_search_record_type(
          "unit", qvec, qdim, limit > 64 ? limit : 64, ids, vector_scores, 128);
      if (q_hits < 0)
         return memory_semantic_query_unavailable();
      if (q_hits > 0)
      {
         typedef struct
         {
            double sim;
            memory_t mem;
         } unit_sem_hit_t;

         unit_sem_hit_t hits[128];
         int hit_count = 0;
         for (int qi = 0; qi < q_hits; qi++)
         {
            if (ids[qi] < PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET)
               continue;
            int64_t unit_id = ids[qi] - PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET;

            double unit_weight = 0.0;
            char unit_type[32];
            char unit_kind[32];
            if (!db2_memory_unit_active_meta(unit_id, &unit_weight, unit_type, sizeof(unit_type),
                                             unit_kind, sizeof(unit_kind)))
               continue;
            if (!unit_kind[0])
               snprintf(unit_kind, sizeof(unit_kind), "%s", MEMORY_UNIT_KIND_EPISODIC_STR);

            double sim = vector_scores[qi];
            sim += unit_weight * 0.03;
            sim += memory_unit_semantic_type_boost(unit_type, intent);
            sim += memory_unit_kind_intent_boost(unit_kind, intent);
            double threshold = 0.52;
            if (intent == MEM_QUERY_TEMPORAL && strcmp(unit_type, "summary") == 0)
               threshold = 0.58;
            else if (intent == MEM_QUERY_TEMPORAL &&
                     (strcmp(unit_type, "temporal") == 0 || strcmp(unit_type, "event") == 0))
               threshold = 0.46;
            threshold *= floor_scale;
            if (sim < threshold)
               continue;

            memory_t candidate;
            if (memory_fetch_row_by_unit_id(unit_id, &candidate) != 0)
               continue;
            int existing = -1;
            for (int i = 0; i < hit_count; i++)
            {
               if (hits[i].mem.id == candidate.id)
               {
                  existing = i;
                  break;
               }
            }
            if (existing >= 0)
            {
               if (sim > hits[existing].sim)
                  hits[existing].sim = sim;
               continue;
            }
            if (hit_count >= (int)(sizeof(hits) / sizeof(hits[0])))
               continue;
            hits[hit_count].sim = sim;
            hits[hit_count].mem = candidate;
            hit_count++;
         }

         for (int i = 0; i < hit_count - 1; i++)
         {
            int best = i;
            for (int j = i + 1; j < hit_count; j++)
            {
               if (hits[j].sim > hits[best].sim)
                  best = j;
            }
            if (best != i)
            {
               unit_sem_hit_t tmp = hits[i];
               hits[i] = hits[best];
               hits[best] = tmp;
            }
         }

         int take = hit_count < limit ? hit_count : limit;
         for (int i = 0; i < take && count < max; i++)
         {
            if (semantic_ids && semantic_scores && semantic_hit_count &&
                *semantic_hit_count < semantic_max)
            {
               semantic_ids[*semantic_hit_count] = hits[i].mem.id;
               semantic_scores[*semantic_hit_count] = hits[i].sim * 1.05;
               (*semantic_hit_count)++;
            }
            count = memory_append_unique(out, count, max, &hits[i].mem);
         }
         if (take > 0)
            return count;
      }
   }

   return count;
}

double memory_semantic_bonus_lookup(int64_t memory_id, const int64_t *semantic_ids,
                                    const double *semantic_scores, int semantic_hit_count)
{
   if (!semantic_ids || !semantic_scores || semantic_hit_count <= 0)
      return 0.0;
   double best = 0.0;
   for (int i = 0; i < semantic_hit_count; i++)
   {
      if (semantic_ids[i] == memory_id)
      {
         double candidate = semantic_scores[i] * 1.2;
         if (candidate > best)
            best = candidate;
      }
   }
   return best;
}

static int memory_query_starts_with(const char *raw_query, const char *norm_query, const char *term)
{
   size_t len;
   if (!term || !(len = strlen(term)))
      return 0;
   if (norm_query && strncmp(norm_query, term, len) == 0 &&
       (norm_query[len] == '\0' || norm_query[len] == ' '))
      return 1;
   if (raw_query && strncasecmp(raw_query, term, len) == 0 &&
       (raw_query[len] == '\0' || raw_query[len] == ' '))
      return 1;
   return 0;
}

memory_query_shape_t memory_query_shape(const char *raw_query, const char *norm_query)
{
   if (!raw_query || !raw_query[0])
      return MEM_SHAPE_UNKNOWN;

   if (memory_query_starts_with(raw_query, norm_query, "when"))
      return MEM_SHAPE_WHEN;
   if (memory_query_starts_with(raw_query, norm_query, "why"))
      return MEM_SHAPE_WHY;
   if (memory_query_starts_with(raw_query, norm_query, "how"))
      return MEM_SHAPE_HOW;
   if (memory_query_starts_with(raw_query, norm_query, "is") ||
       memory_query_starts_with(raw_query, norm_query, "are") ||
       memory_query_starts_with(raw_query, norm_query, "do") ||
       memory_query_starts_with(raw_query, norm_query, "does") ||
       memory_query_starts_with(raw_query, norm_query, "did") ||
       memory_query_starts_with(raw_query, norm_query, "can") ||
       memory_query_starts_with(raw_query, norm_query, "should") ||
       memory_query_starts_with(raw_query, norm_query, "has") ||
       memory_query_starts_with(raw_query, norm_query, "have") ||
       memory_query_starts_with(raw_query, norm_query, "had"))
      return MEM_SHAPE_YES_NO;

   if ((norm_query &&
        (memory_query_has_term(norm_query, "list") || memory_query_has_term(norm_query, "all") ||
         memory_query_has_term(norm_query, "every"))) ||
       memory_query_starts_with(raw_query, norm_query, "which"))
      return MEM_SHAPE_LIST;

   if (memory_query_starts_with(raw_query, norm_query, "what") ||
       memory_query_starts_with(raw_query, norm_query, "who"))
      return MEM_SHAPE_FACTOID;

   return MEM_SHAPE_UNKNOWN;
}

memory_query_route_t memory_query_route(const char *raw_query, const char *norm_query,
                                        memory_query_shape_t shape)
{
   /* Profile-shaped queries benefit most from the graph + semantic hybrid */
   if (memory_is_profile_query(raw_query))
      return MEM_ROUTE_HYBRID;

   if ((raw_query && (strcasestr(raw_query, "what calls") || strcasestr(raw_query, "who calls") ||
                      strcasestr(raw_query, "depends on") || strcasestr(raw_query, "fixes"))) ||
       (norm_query && (strstr(norm_query, "callers") || strstr(norm_query, "depends on"))))
      return MEM_ROUTE_GRAPH;

   if ((raw_query && strstr(raw_query, "`")) || memory_query_is_code_like(raw_query) ||
       memory_query_is_code_like(norm_query))
      return MEM_ROUTE_LEXICAL;

   if ((shape == MEM_SHAPE_HOW || shape == MEM_SHAPE_WHY || shape == MEM_SHAPE_WHEN ||
        shape == MEM_SHAPE_FACTOID) &&
       !memory_query_is_code_like(raw_query) && !memory_query_is_code_like(norm_query))
      return MEM_ROUTE_SEMANTIC;

   return MEM_ROUTE_HYBRID;
}

void memory_record_query_plan_metrics(const memory_query_plan_t *plan, double elapsed_ms)
{
   char key[128];
   const char *route_name;
   const char *shape_name;

   if (!plan)
      return;

   route_name = memory_query_route_name(plan->route);
   shape_name = memory_query_shape_name(plan->shape);
   memory_runtime_state_increment("memory.query.total", 1);
   snprintf(key, sizeof(key), "memory.query.route.%s", route_name);
   memory_runtime_state_increment(key, 1);
   snprintf(key, sizeof(key), "memory.query.shape.%s", shape_name);
   memory_runtime_state_increment(key, 1);
   if (elapsed_ms >= 0.0)
   {
      int rounded_ms = (int)(elapsed_ms + 0.5);
      memory_runtime_state_increment("memory.query.latency_ms.total", rounded_ms);
      snprintf(key, sizeof(key), "memory.query.route.%s.latency_ms", route_name);
      memory_runtime_state_increment(key, rounded_ms);
      snprintf(key, sizeof(key), "memory.query.shape.%s.latency_ms", shape_name);
      memory_runtime_state_increment(key, rounded_ms);
   }
}

double memory_query_elapsed_ms(const struct timespec *start, const struct timespec *end)
{
   long sec = end->tv_sec - start->tv_sec;
   long nsec = end->tv_nsec - start->tv_nsec;
   if (nsec < 0)
   {
      sec--;
      nsec += 1000000000L;
   }
   return (double)sec * 1000.0 + (double)nsec / 1000000.0;
}

int memory_query_plan(const char *query, int limit, int hard_cap, memory_query_plan_t *out)
{
   if (!query || !query[0] || !out)
      return -1;

   char norm_query[512];
   normalize_key(query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", query);

   memset(out, 0, sizeof(*out));
   out->shape = memory_query_shape(query, norm_query);
   {
      config_t cfg;
      if (config_load(&cfg) == 0 && !cfg.memory_routing_enabled)
         out->route = MEM_ROUTE_HYBRID;
      else
         out->route = memory_query_route(query, norm_query, out->shape);
   }
   out->graph_hops = (out->route == MEM_ROUTE_GRAPH) ? 2 : 1;
   out->semantic_enabled = (out->route != MEM_ROUTE_LEXICAL && out->route != MEM_ROUTE_GRAPH);

   switch (out->route)
   {
   case MEM_ROUTE_LEXICAL:
      out->weights.lexical_weight = 1.00;
      out->weights.semantic_weight = 0.10;
      out->weights.graph_weight = 0.10;
      out->weights.temporal_weight = 0.10;
      break;
   case MEM_ROUTE_SEMANTIC:
      out->weights.lexical_weight = 0.45;
      out->weights.semantic_weight = 1.00;
      out->weights.graph_weight = 0.20;
      out->weights.temporal_weight = 0.35;
      break;
   case MEM_ROUTE_GRAPH:
      out->weights.lexical_weight = 0.35;
      out->weights.semantic_weight = 0.15;
      out->weights.graph_weight = 1.00;
      out->weights.temporal_weight = 0.15;
      break;
   case MEM_ROUTE_HYBRID:
   default:
      out->weights.lexical_weight = 0.80;
      out->weights.semantic_weight = 0.70;
      out->weights.graph_weight = 0.30;
      out->weights.temporal_weight = 0.30;
      break;
   }

   int mult = 4;
   int min_fetch = 24;
   int max_fetch = hard_cap > 0 ? hard_cap : 96;
   switch (out->shape)
   {
   case MEM_SHAPE_WHEN:
      mult = 5;
      min_fetch = 32;
      out->weights.temporal_weight += 0.55;
      break;
   case MEM_SHAPE_LIST:
      mult = 6;
      min_fetch = 36;
      break;
   case MEM_SHAPE_YES_NO:
      mult = 3;
      min_fetch = 16;
      break;
   case MEM_SHAPE_HOW:
   case MEM_SHAPE_WHY:
      mult = 4;
      min_fetch = 20;
      out->weights.semantic_weight += 0.20;
      break;
   case MEM_SHAPE_FACTOID:
      mult = 4;
      min_fetch = 18;
      break;
   case MEM_SHAPE_UNKNOWN:
   default:
      break;
   }

   if (out->route == MEM_ROUTE_LEXICAL)
   {
      mult = 2;
      min_fetch = min_fetch < 12 ? min_fetch : 12;
      max_fetch = max_fetch < 48 ? max_fetch : 48;
   }
   else if (out->route == MEM_ROUTE_GRAPH)
   {
      mult += 1;
      min_fetch += 4;
   }

   if (limit <= 0)
      limit = 10;
   if (max_fetch < limit)
      max_fetch = limit;
   out->fetch_multiplier = mult;
   out->min_fetch = min_fetch;
   out->max_fetch = max_fetch;
   return 0;
}

memory_query_intent_t memory_query_intent(const char *raw_query, const char *norm_query)
{
   if ((raw_query &&
        (strstr(raw_query, "when") || strstr(raw_query, "When") || strstr(raw_query, "what time") ||
         strstr(raw_query, "What time") || strstr(raw_query, "what day") ||
         strstr(raw_query, "What day") || strstr(raw_query, "date") || strstr(raw_query, "dated") ||
         strstr(raw_query, "before") || strstr(raw_query, "after") ||
         strstr(raw_query, "between") || strstr(raw_query, "since") || strstr(raw_query, "until") ||
         strstr(raw_query, "last week") || strstr(raw_query, "next week") ||
         strstr(raw_query, "last month") || strstr(raw_query, "next month") ||
         strstr(raw_query, "ago"))) ||
       (norm_query && (strstr(norm_query, "year") || strstr(norm_query, "month") ||
                       strstr(norm_query, "day") || strstr(norm_query, "time") ||
                       strstr(norm_query, "today") || strstr(norm_query, "yesterday") ||
                       strstr(norm_query, "tomorrow") || strstr(norm_query, "latest") ||
                       strstr(norm_query, "recent") || strstr(norm_query, "earliest"))))
      return MEM_QUERY_TEMPORAL;

   if ((raw_query && (strstr(raw_query, "how ") || strstr(raw_query, "How ") ||
                      strstr(raw_query, "steps") || strstr(raw_query, "procedure"))) ||
       (norm_query && (strstr(norm_query, "workflow") || strstr(norm_query, "setup"))))
      return MEM_QUERY_PROCEDURAL;

   if ((raw_query && (strstr(raw_query, "who ") || strstr(raw_query, "Who ") ||
                      strstr(raw_query, "person") || strstr(raw_query, "people"))) ||
       (norm_query && (strstr(norm_query, "team") || strstr(norm_query, "owner"))))
      return MEM_QUERY_ENTITY;

   return MEM_QUERY_GENERAL;
}

int memory_query_has_term(const char *norm_query, const char *term)
{
   return norm_query && term && term[0] && memory_token_in_norm(norm_query, term);
}

int memory_query_wants_recent(const char *norm_query)
{
   static const char *terms[] = {"latest", "recent", "newest", "current", "today", "now", NULL};
   for (int i = 0; terms[i]; i++)
      if (memory_query_has_term(norm_query, terms[i]))
         return 1;
   return 0;
}

int memory_query_wants_past(const char *norm_query)
{
   static const char *terms[] = {"before", "earlier", "earliest", "previous", "older", NULL};
   for (int i = 0; terms[i]; i++)
      if (memory_query_has_term(norm_query, terms[i]))
         return 1;
   return 0;
}

int memory_query_wants_future(const char *norm_query)
{
   static const char *terms[] = {"after", "later", "next", "upcoming", NULL};
   for (int i = 0; terms[i]; i++)
      if (memory_query_has_term(norm_query, terms[i]))
         return 1;
   return 0;
}

static int memory_parse_iso_date_token(const char *token, int *year, int *month, int *day)
{
   if (!token || strlen(token) != 10 || !isdigit((unsigned char)token[0]) ||
       !isdigit((unsigned char)token[1]) || !isdigit((unsigned char)token[2]) ||
       !isdigit((unsigned char)token[3]) || token[4] != ' ' || !isdigit((unsigned char)token[5]) ||
       !isdigit((unsigned char)token[6]) || token[7] != ' ' || !isdigit((unsigned char)token[8]) ||
       !isdigit((unsigned char)token[9]))
   {
      return 0;
   }
   if (year)
      *year = atoi(token);
   if (month)
      *month = atoi(token + 5);
   if (day)
      *day = atoi(token + 8);
   return 1;
}

static void memory_current_utc_date(memory_parsed_date_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   time_t now = time(NULL);
   struct tm tm_now = {0};
   if (!gmtime_r(&now, &tm_now))
      return;
   out->year = tm_now.tm_year + 1900;
   out->month = tm_now.tm_mon + 1;
   out->day = tm_now.tm_mday;
   out->granularity = 3;
   out->valid = 1;
}

static int memory_weekday_number(const char *tok)
{
   static const char *names[] = {"sunday",   "monday", "tuesday",  "wednesday",
                                 "thursday", "friday", "saturday", NULL};
   if (!tok || !tok[0])
      return -1;
   for (int i = 0; names[i]; i++)
      if (strcmp(tok, names[i]) == 0)
         return i;
   return -1;
}

static int memory_add_days(memory_parsed_date_t *date, int delta_days)
{
   if (!date || !date->valid)
      return 0;
   struct tm tm_buf;
   memset(&tm_buf, 0, sizeof(tm_buf));
   tm_buf.tm_year = date->year - 1900;
   tm_buf.tm_mon = date->month - 1;
   tm_buf.tm_mday = date->day + delta_days;
   tm_buf.tm_isdst = 0;
   time_t shifted = timegm(&tm_buf);
   if (shifted == (time_t)-1)
      return 0;
   gmtime_r(&shifted, &tm_buf);
   date->year = tm_buf.tm_year + 1900;
   date->month = tm_buf.tm_mon + 1;
   date->day = tm_buf.tm_mday;
   date->granularity = 3;
   date->valid = 1;
   return 1;
}

static int memory_add_months(memory_parsed_date_t *date, int delta_months)
{
   if (!date || !date->valid)
      return 0;
   int month_index = (date->year * 12 + (date->month - 1)) + delta_months;
   if (month_index < 0)
      return 0;
   date->year = month_index / 12;
   date->month = (month_index % 12) + 1;
   if (date->day <= 0)
      date->day = 1;
   date->granularity = date->day > 0 ? 3 : 2;
   date->valid = 1;
   return 1;
}

static int memory_parse_relative_temporal_constraint(char qtokens[][64], int nq,
                                                     memory_temporal_constraint_t *out)
{
   if (!qtokens || nq <= 0 || !out)
      return 0;

   memory_parsed_date_t today;
   memory_current_utc_date(&today);

   for (int i = 0; i < nq; i++)
   {
      if (strcmp(qtokens[i], "today") == 0)
      {
         out->first = today;
         out->mode = MEM_DATE_CONSTRAINT_MATCH;
         return 1;
      }
      if (strcmp(qtokens[i], "yesterday") == 0)
      {
         out->first = today;
         memory_add_days(&out->first, -1);
         out->mode = MEM_DATE_CONSTRAINT_MATCH;
         return 1;
      }
      if (strcmp(qtokens[i], "tomorrow") == 0)
      {
         out->first = today;
         memory_add_days(&out->first, 1);
         out->mode = MEM_DATE_CONSTRAINT_MATCH;
         return 1;
      }

      if (i + 1 < nq && (strcmp(qtokens[i], "last") == 0 || strcmp(qtokens[i], "next") == 0))
      {
         int delta = strcmp(qtokens[i], "last") == 0 ? -1 : 1;
         if (strcmp(qtokens[i + 1], "week") == 0)
         {
            struct tm tm_now = {0};
            time_t now = time(NULL);
            if (!gmtime_r(&now, &tm_now))
               continue;
            int weekday = tm_now.tm_wday;
            out->first = today;
            out->second = today;
            memory_add_days(&out->first, -(weekday == 0 ? 6 : weekday - 1) + delta * 7);
            out->second = out->first;
            memory_add_days(&out->second, 6);
            out->mode = MEM_DATE_CONSTRAINT_BETWEEN;
            return 1;
         }
         if (strcmp(qtokens[i + 1], "month") == 0)
         {
            out->first = today;
            out->first.day = 0;
            out->first.granularity = 2;
            memory_add_months(&out->first, delta);
            out->mode = MEM_DATE_CONSTRAINT_MATCH;
            return 1;
         }
         if (strcmp(qtokens[i + 1], "year") == 0)
         {
            out->first = today;
            out->first.year += delta;
            out->first.month = 0;
            out->first.day = 0;
            out->first.granularity = 1;
            out->mode = MEM_DATE_CONSTRAINT_MATCH;
            return 1;
         }
         if (memory_is_weekday_token(qtokens[i + 1]))
         {
            int target = memory_weekday_number(qtokens[i + 1]);
            struct tm tm_now = {0};
            time_t now = time(NULL);
            if (!gmtime_r(&now, &tm_now))
               continue;
            int current = tm_now.tm_wday;
            int offset = target - current + delta * 7;
            out->first = today;
            memory_add_days(&out->first, offset);
            out->mode = MEM_DATE_CONSTRAINT_MATCH;
            return 1;
         }
      }

      if (i + 2 < nq && isdigit((unsigned char)qtokens[i][0]))
      {
         int amount = atoi(qtokens[i]);
         if (amount > 0 && strcmp(qtokens[i + 2], "ago") == 0)
         {
            out->first = today;
            if (strncmp(qtokens[i + 1], "day", 3) == 0)
               memory_add_days(&out->first, -amount);
            else if (strncmp(qtokens[i + 1], "week", 4) == 0)
               memory_add_days(&out->first, -amount * 7);
            else if (strncmp(qtokens[i + 1], "month", 5) == 0)
            {
               out->first.day = 0;
               out->first.granularity = 2;
               memory_add_months(&out->first, -amount);
            }
            else
               continue;
            out->mode = MEM_DATE_CONSTRAINT_MATCH;
            return 1;
         }
      }
   }

   return 0;
}

static int memory_parse_date_tokens(char qtokens[][64], int nq, int start,
                                    memory_parsed_date_t *out, int *consumed)
{
   if (!qtokens || nq <= 0 || start < 0 || start >= nq || !out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (consumed)
      *consumed = 0;

   if (start + 2 < nq)
   {
      char joined[16];
      int year = 0, month = 0, day = 0;
      snprintf(joined, sizeof(joined), "%s %s %s", qtokens[start], qtokens[start + 1],
               qtokens[start + 2]);
      if (memory_parse_iso_date_token(joined, &year, &month, &day))
      {
         out->year = year;
         out->month = month;
         out->day = day;
         out->granularity = 3;
         out->valid = 1;
         if (consumed)
            *consumed = 3;
         return 1;
      }
   }

   if (start + 2 < nq)
   {
      int year = 0;
      if (strlen(qtokens[start + 2]) == 4 && sscanf(qtokens[start + 2], "%d", &year) == 1)
      {
         if (memory_is_month_token(qtokens[start]) &&
             sscanf(qtokens[start + 1], "%d", &out->day) == 1)
         {
            out->year = year;
            out->month = memory_month_number(qtokens[start]);
            out->granularity = 3;
            out->valid = out->month > 0 && out->day > 0 && out->day <= 31;
            if (out->valid)
            {
               if (consumed)
                  *consumed = 3;
               return 1;
            }
         }
         if (sscanf(qtokens[start], "%d", &out->day) == 1 &&
             memory_is_month_token(qtokens[start + 1]))
         {
            out->year = year;
            out->month = memory_month_number(qtokens[start + 1]);
            out->granularity = 3;
            out->valid = out->month > 0 && out->day > 0 && out->day <= 31;
            if (out->valid)
            {
               if (consumed)
                  *consumed = 3;
               return 1;
            }
         }
      }
   }

   if (start + 1 < nq)
   {
      if (memory_is_month_token(qtokens[start]) && sscanf(qtokens[start + 1], "%d", &out->day) == 1)
      {
         out->month = memory_month_number(qtokens[start]);
         out->granularity = 2;
         out->valid = out->month > 0 && out->day > 0 && out->day <= 31;
         if (out->valid)
         {
            if (consumed)
               *consumed = 2;
            return 1;
         }
      }
      if (sscanf(qtokens[start], "%d", &out->day) == 1 && memory_is_month_token(qtokens[start + 1]))
      {
         out->month = memory_month_number(qtokens[start + 1]);
         out->granularity = 2;
         out->valid = out->month > 0 && out->day > 0 && out->day <= 31;
         if (out->valid)
         {
            if (consumed)
               *consumed = 2;
            return 1;
         }
      }
   }

   if (memory_is_month_token(qtokens[start]))
   {
      out->month = memory_month_number(qtokens[start]);
      out->granularity = 2;
      out->valid = out->month > 0;
      if (out->valid)
      {
         if (consumed)
            *consumed = 1;
         return 1;
      }
   }
   if (strlen(qtokens[start]) == 4 && sscanf(qtokens[start], "%d", &out->year) == 1)
   {
      out->granularity = 1;
      out->valid = 1;
      if (consumed)
         *consumed = 1;
      return 1;
   }
   return 0;
}

static int memory_date_compare(const memory_parsed_date_t *a, const memory_parsed_date_t *b)
{
   if (!a || !b || !a->valid || !b->valid)
      return 0;
   int precision = a->granularity < b->granularity ? a->granularity : b->granularity;
   if (a->year != b->year)
      return a->year < b->year ? -1 : 1;
   if (precision == 1)
      return 0;
   if (a->month != b->month)
      return a->month < b->month ? -1 : 1;
   if (precision == 2)
      return 0;
   if (a->day != b->day)
      return a->day < b->day ? -1 : 1;
   return 0;
}

int memory_parse_temporal_ref_date(const char *ref_key, const char *granularity,
                                   memory_parsed_date_t *out)
{
   if (!ref_key || !ref_key[0] || !out)
      return 0;
   char norm[128];
   char tokens[8][64];
   normalize_key(ref_key, norm, sizeof(norm));
   int count = memory_split_tokens(norm, tokens, 8);
   if (count <= 0)
      return 0;
   if (memory_parse_date_tokens(tokens, count, 0, out, NULL))
   {
      if (out->granularity == 2 && granularity && strcmp(granularity, "month") == 0)
         out->day = 0;
      return 1;
   }
   return 0;
}

int memory_parse_temporal_constraint(const char *norm_query, char qtokens[][64], int nq,
                                     memory_temporal_constraint_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (!qtokens || nq <= 0)
      return 0;

   if (memory_parse_relative_temporal_constraint(qtokens, nq, out))
      return 1;

   for (int i = 0; i < nq; i++)
   {
      int consumed = 0;
      if (strcmp(qtokens[i], "between") == 0)
      {
         if (memory_parse_date_tokens(qtokens, nq, i + 1, &out->first, &consumed))
         {
            int second_start = i + 1 + consumed;
            while (second_start < nq && strcmp(qtokens[second_start], "and") == 0)
               second_start++;
            if (memory_parse_date_tokens(qtokens, nq, second_start, &out->second, NULL))
            {
               out->mode = MEM_DATE_CONSTRAINT_BETWEEN;
               return 1;
            }
         }
      }
      if ((strcmp(qtokens[i], "before") == 0 || strcmp(qtokens[i], "until") == 0) &&
          memory_parse_date_tokens(qtokens, nq, i + 1, &out->first, NULL))
      {
         out->mode = MEM_DATE_CONSTRAINT_BEFORE;
         return 1;
      }
      if ((strcmp(qtokens[i], "after") == 0 || strcmp(qtokens[i], "since") == 0) &&
          memory_parse_date_tokens(qtokens, nq, i + 1, &out->first, NULL))
      {
         out->mode = MEM_DATE_CONSTRAINT_AFTER;
         return 1;
      }
   }

   for (int i = 0; i < nq; i++)
   {
      if (memory_parse_date_tokens(qtokens, nq, i, &out->first, NULL))
      {
         out->mode = MEM_DATE_CONSTRAINT_MATCH;
         return 1;
      }
   }

   if (norm_query && (memory_query_wants_past(norm_query) || memory_query_wants_future(norm_query)))
      return 1;
   return 0;
}

double memory_temporal_constraint_score(const memory_temporal_constraint_t *constraint,
                                        const memory_parsed_date_t *candidate)
{
   if (!constraint || constraint->mode == MEM_DATE_CONSTRAINT_NONE || !candidate ||
       !candidate->valid)
      return 0.0;

   int cmp_first = constraint->first.valid ? memory_date_compare(candidate, &constraint->first) : 0;
   int cmp_second =
       constraint->second.valid ? memory_date_compare(candidate, &constraint->second) : 0;
   double base = candidate->granularity >= 3 ? 0.62 : (candidate->granularity == 2 ? 0.38 : 0.22);

   switch (constraint->mode)
   {
   case MEM_DATE_CONSTRAINT_MATCH:
      return cmp_first == 0 ? base : -0.28;
   case MEM_DATE_CONSTRAINT_BEFORE:
      return cmp_first < 0 ? base : -0.30;
   case MEM_DATE_CONSTRAINT_AFTER:
      return cmp_first > 0 ? base : -0.30;
   case MEM_DATE_CONSTRAINT_BETWEEN:
      if (!constraint->second.valid)
         return 0.0;
      return (cmp_first >= 0 && cmp_second <= 0) ? base + 0.08 : -0.34;
   default:
      return 0.0;
   }
}

int memory_temporal_value_conflict(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   char norm_a[1024];
   char norm_b[1024];
   char atok[24][64];
   char btok[24][64];
   char a_non[1024];
   char b_non[1024];
   int a_count = 0, b_count = 0;
   int a_sig = 0, b_sig = 0, shared_non = 0;

   normalize_key(a, norm_a, sizeof(norm_a));
   normalize_key(b, norm_b, sizeof(norm_b));
   a_count = memory_split_tokens(norm_a, atok, 24);
   b_count = memory_split_tokens(norm_b, btok, 24);
   a_non[0] = '\0';
   b_non[0] = '\0';

   for (int i = 0; i < a_count; i++)
   {
      int signal = memory_is_month_token(atok[i]) || memory_is_weekday_token(atok[i]) ||
                   (atok[i][0] && isdigit((unsigned char)atok[i][0]));
      if (signal)
         a_sig++;
      else if (!memory_is_stopword_token(atok[i]))
      {
         if (a_non[0])
            strncat(a_non, " ", sizeof(a_non) - strlen(a_non) - 1);
         strncat(a_non, atok[i], sizeof(a_non) - strlen(a_non) - 1);
      }
   }
   for (int i = 0; i < b_count; i++)
   {
      int signal = memory_is_month_token(btok[i]) || memory_is_weekday_token(btok[i]) ||
                   (btok[i][0] && isdigit((unsigned char)btok[i][0]));
      if (signal)
         b_sig++;
      else if (!memory_is_stopword_token(btok[i]))
      {
         if (b_non[0])
            strncat(b_non, " ", sizeof(b_non) - strlen(b_non) - 1);
         strncat(b_non, btok[i], sizeof(b_non) - strlen(b_non) - 1);
      }
   }

   if (a_sig == 0 || b_sig == 0)
      return 0;
   for (int i = 0; i < a_count; i++)
   {
      if (memory_is_month_token(atok[i]) || memory_is_weekday_token(atok[i]) ||
          (atok[i][0] && isdigit((unsigned char)atok[i][0])))
      {
         continue;
      }
      if (memory_token_in_norm(b_non, atok[i]))
         shared_non++;
   }
   return shared_non > 0 && word_similarity(a_non, b_non) > 0.55 &&
          trigram_similarity(norm_a, norm_b) < 0.95;
}

int memory_temporal_query_has_explicit_date(char qtokens[][64], int nq)
{
   for (int i = 0; i < nq; i++)
   {
      if ((strlen(qtokens[i]) == 4 || strlen(qtokens[i]) == 2) &&
          isdigit((unsigned char)qtokens[i][0]))
         return 1;
      if (memory_is_month_token(qtokens[i]))
         return 1;
   }
   for (int i = 0; i + 2 < nq; i++)
   {
      char joined[16];
      snprintf(joined, sizeof(joined), "%s %s %s", qtokens[i], qtokens[i + 1], qtokens[i + 2]);
      if (memory_parse_iso_date_token(joined, NULL, NULL, NULL))
         return 1;
   }
   return 0;
}

int memory_temporal_ref_matches_query(const char *ref_key, char qtokens[][64], int nq)
{
   if (!ref_key || !ref_key[0] || !qtokens || nq <= 0)
      return 0;
   for (int i = 0; i < nq; i++)
      if (memory_token_in_norm(ref_key, qtokens[i]))
         return 1;

   for (int i = 0; i + 2 < nq; i++)
   {
      int year = 0, month = 0, day = 0;
      char joined[16];
      char year_buf[8];
      char month_buf[4];
      char day_buf[4];
      snprintf(joined, sizeof(joined), "%s %s %s", qtokens[i], qtokens[i + 1], qtokens[i + 2]);
      if (!memory_parse_iso_date_token(joined, &year, &month, &day))
         continue;
      snprintf(year_buf, sizeof(year_buf), "%04d", year);
      snprintf(month_buf, sizeof(month_buf), "%02d", month);
      snprintf(day_buf, sizeof(day_buf), "%02d", day);
      if (memory_token_in_norm(ref_key, year_buf) &&
          (memory_token_in_norm(ref_key, month_buf) || memory_token_in_norm(ref_key, day_buf)))
      {
         return 1;
      }
   }
   return 0;
}

int memory_add_expanded_term(char terms[][64], int count, int max_terms, const char *term)
{
   if (!term || !term[0] || count >= max_terms)
      return count;
   for (int i = 0; i < count; i++)
      if (strcmp(terms[i], term) == 0)
         return count;
   snprintf(terms[count], 64, "%s", term);
   return count + 1;
}

int memory_expand_query_terms(const char *norm_query, char base_tokens[][64], int base_count,
                              char expanded[][64], int max_terms)
{
   int count = 0;
   int saw_before = 0, saw_after = 0, saw_between = 0;
   for (int i = 0; i < base_count && count < max_terms; i++)
   {
      count = memory_add_expanded_term(expanded, count, max_terms, base_tokens[i]);
      if (strcmp(base_tokens[i], "when") == 0 || strcmp(base_tokens[i], "date") == 0 ||
          strcmp(base_tokens[i], "year") == 0 || strcmp(base_tokens[i], "month") == 0 ||
          strcmp(base_tokens[i], "day") == 0 || strcmp(base_tokens[i], "today") == 0 ||
          strcmp(base_tokens[i], "yesterday") == 0 || strcmp(base_tokens[i], "tomorrow") == 0 ||
          strcmp(base_tokens[i], "last") == 0 || strcmp(base_tokens[i], "next") == 0 ||
          strcmp(base_tokens[i], "week") == 0 || strcmp(base_tokens[i], "ago") == 0)
      {
         count = memory_add_expanded_term(expanded, count, max_terms, "date");
         count = memory_add_expanded_term(expanded, count, max_terms, "time");
         count = memory_add_expanded_term(expanded, count, max_terms, "event_time");
         count = memory_add_expanded_term(expanded, count, max_terms, "timeline");
         count = memory_add_expanded_term(expanded, count, max_terms, "occurred");
         count = memory_add_expanded_term(expanded, count, max_terms, "happened");
      }
      if (strcmp(base_tokens[i], "who") == 0 || strcmp(base_tokens[i], "owner") == 0 ||
          strcmp(base_tokens[i], "person") == 0 || strcmp(base_tokens[i], "people") == 0)
      {
         count = memory_add_expanded_term(expanded, count, max_terms, "actor");
         count = memory_add_expanded_term(expanded, count, max_terms, "team");
      }
      if (strcmp(base_tokens[i], "setup") == 0 || strcmp(base_tokens[i], "install") == 0 ||
          strcmp(base_tokens[i], "configure") == 0 || strcmp(base_tokens[i], "workflow") == 0)
      {
         count = memory_add_expanded_term(expanded, count, max_terms, "procedure");
         count = memory_add_expanded_term(expanded, count, max_terms, "steps");
      }
      if (strcmp(base_tokens[i], "went") == 0)
         count = memory_add_expanded_term(expanded, count, max_terms, "go");
      if (strcmp(base_tokens[i], "joined") == 0)
         count = memory_add_expanded_term(expanded, count, max_terms, "join");
      if (strcmp(base_tokens[i], "visited") == 0)
         count = memory_add_expanded_term(expanded, count, max_terms, "visit");
      if (strcmp(base_tokens[i], "before") == 0)
         saw_before = 1;
      if (strcmp(base_tokens[i], "after") == 0)
         saw_after = 1;
      if (strcmp(base_tokens[i], "between") == 0)
         saw_between = 1;
      if (strcmp(base_tokens[i], "latest") == 0 || strcmp(base_tokens[i], "recent") == 0 ||
          strcmp(base_tokens[i], "current") == 0)
      {
         count = memory_add_expanded_term(expanded, count, max_terms, "latest");
         count = memory_add_expanded_term(expanded, count, max_terms, "recent");
         count = memory_add_expanded_term(expanded, count, max_terms, "current");
      }
   }
   if (saw_before)
      count = memory_add_expanded_term(expanded, count, max_terms, "before");
   if (saw_after)
      count = memory_add_expanded_term(expanded, count, max_terms, "after");
   if (saw_between)
      count = memory_add_expanded_term(expanded, count, max_terms, "between");
   if (norm_query && strstr(norm_query, "support group"))
      count = memory_add_expanded_term(expanded, count, max_terms, "group");
   return count;
}

/* Build up to max_phrases bigrams from consecutive non-stopword query tokens.
 * Returns the number of phrases written into out[][]. */
static int memory_query_bigrams(char qtokens[][64], int nq, char out[][128], int max_phrases)
{
   int count = 0;
   for (int i = 0; i < nq - 1 && count < max_phrases; i++)
   {
      if (memory_is_stopword_token(qtokens[i]) || memory_is_stopword_token(qtokens[i + 1]))
         continue;
      if (strlen(qtokens[i]) < 3 || strlen(qtokens[i + 1]) < 3)
         continue;
      snprintf(out[count], 128, "%s %s", qtokens[i], qtokens[i + 1]);
      count++;
   }
   return count;
}

double memory_entity_bonus(int64_t memory_id, char qtokens[][64], int nq)
{
   db2_memory_entity_row_t rows[64];
   int nrows =
       db2_memory_entities_list_weighted(memory_id, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   if (nrows <= 0)
      return 0.0;

   /* Pre-build query bigrams for multi-word entity matching */
   char bigrams[8][128];
   int nbigrams = memory_query_bigrams(qtokens, nq, bigrams, 8);

   double bonus = 0.0;
   for (int r = 0; r < nrows; r++)
   {
      const char *entity = rows[r].entity;
      const char *role = rows[r].role;
      double weight = rows[r].weight;
      if (!entity[0])
         continue;

      /* Role-based multiplier: actor/subject entities are stronger signals */
      double role_mult = 1.0;
      if (role[0] && (strcmp(role, "actor") == 0 || strcmp(role, "subject") == 0))
         role_mult = 1.5;

      int matched = 0;

      /* Single-token matching (existing behaviour) */
      for (int i = 0; i < nq && !matched; i++)
      {
         char canonical_q[64];
         memory_canonicalize_term(qtokens[i], canonical_q, sizeof(canonical_q));
         if ((canonical_q[0] && memory_token_in_norm(entity, canonical_q)) ||
             memory_token_in_norm(entity, qtokens[i]))
            matched = 1;
      }

      /* Multi-word bigram matching: query bigram found inside entity phrase */
      for (int b = 0; b < nbigrams && !matched; b++)
      {
         if (strstr(entity, bigrams[b]) != NULL)
            matched = 1;
         /* Also try entity inside query bigram */
         if (!matched && strstr(bigrams[b], entity) != NULL)
            matched = 1;
      }

      if (matched)
         bonus += (0.3 + weight * 0.15) * role_mult;
   }
   return bonus;
}

/* Speaker-alignment bonus: boost memories whose "actor" entity matches a
 * proper-noun token found in the raw query.  Proper nouns are identified
 * as space-separated words that start with an uppercase letter and are not
 * the first word of the sentence. */
double memory_speaker_bonus(int64_t memory_id, const char *raw_query)
{
   if (!raw_query || !raw_query[0])
      return 0.0;

   /* Extract proper-noun candidates from the raw query (capitalised, not sentence-start) */
   char proper[8][64];
   int nproper = 0;
   const char *p = raw_query;
   /* Skip the very first word (may be capitalised by sentence rules) */
   while (*p && !isspace((unsigned char)*p))
      p++;
   while (*p && nproper < 8)
   {
      while (*p && isspace((unsigned char)*p))
         p++;
      if (!*p)
         break;
      if (isupper((unsigned char)*p))
      {
         /* Collect the word */
         int wlen = 0;
         const char *ws = p;
         while (*p && !isspace((unsigned char)*p) && wlen < 63)
         {
            p++;
            wlen++;
         }
         /* Must be purely alphabetic and long enough to be a name */
         int alpha_only = 1;
         for (int i = 0; i < wlen; i++)
            if (!isalpha((unsigned char)ws[i]))
               alpha_only = 0;
         if (alpha_only && wlen >= 3)
         {
            memcpy(proper[nproper], ws, (size_t)wlen);
            proper[nproper][wlen] = '\0';
            /* Lowercase for comparison */
            for (int i = 0; i < wlen; i++)
               proper[nproper][i] = (char)tolower((unsigned char)proper[nproper][i]);
            nproper++;
         }
      }
      else
      {
         /* Skip non-capitalised word */
         while (*p && !isspace((unsigned char)*p))
            p++;
      }
   }
   if (nproper == 0)
      return 0.0;

   db2_memory_entity_row_t rows[64];
   int nrows =
       db2_memory_entities_list_weighted(memory_id, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   double bonus = 0.0;
   for (int r = 0; r < nrows; r++)
   {
      if (strcmp(rows[r].role, "actor") != 0)
         continue;
      const char *entity = rows[r].entity;
      double weight = rows[r].weight;
      if (!entity[0])
         continue;
      for (int i = 0; i < nproper; i++)
      {
         /* Check if the canonical entity matches the proper noun */
         char canonical_e[64];
         memory_canonicalize_term(entity, canonical_e, sizeof(canonical_e));
         if ((canonical_e[0] && strcmp(canonical_e, proper[i]) == 0) ||
             strcmp(entity, proper[i]) == 0)
         {
            bonus += 0.45 + weight * 0.1;
            break;
         }
      }
   }
   return bonus;
}
