#if defined(AIMEE_DB2_DISABLED)
#error "memory_core KB-real TU must not be compiled into the AIMEE_DB2_DISABLED (server) build"
#endif
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "memory_core_internal.h"
/* memory_core_helpers.c: split from memory_core.c into a real translation unit
 * (was memory_core_helpers.inc, textually included only to stay under the
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

#define MCH_ERRBUF 256

static int64_t memory_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void memory_record_pagerank_stats(double elapsed_ms, int candidates, int edges)
{
   pthread_mutex_lock(&s_memory_pagerank_stats_mu);
   s_memory_pagerank_stats.samples++;
   s_memory_pagerank_stats.last_ms = elapsed_ms;
   s_memory_pagerank_stats.last_candidates = candidates;
   s_memory_pagerank_stats.last_edges = edges;
   if (elapsed_ms > s_memory_pagerank_stats.max_ms)
      s_memory_pagerank_stats.max_ms = elapsed_ms;
   if (s_memory_pagerank_stats.samples == 1)
      s_memory_pagerank_stats.avg_ms = elapsed_ms;
   else
      s_memory_pagerank_stats.avg_ms +=
          (elapsed_ms - s_memory_pagerank_stats.avg_ms) / (double)s_memory_pagerank_stats.samples;
   pthread_mutex_unlock(&s_memory_pagerank_stats_mu);
}

void memory_fill_pagerank_stats(memory_stats_t *out)
{
   if (!out)
      return;
   pthread_mutex_lock(&s_memory_pagerank_stats_mu);
   out->pagerank_last_ms = s_memory_pagerank_stats.last_ms;
   out->pagerank_avg_ms = s_memory_pagerank_stats.avg_ms;
   out->pagerank_max_ms = s_memory_pagerank_stats.max_ms;
   out->pagerank_samples = s_memory_pagerank_stats.samples;
   out->pagerank_last_candidates = s_memory_pagerank_stats.last_candidates;
   out->pagerank_last_edges = s_memory_pagerank_stats.last_edges;
   pthread_mutex_unlock(&s_memory_pagerank_stats_mu);
}

static void memory_apply_weight_kv(memory_rank_weights_t *w, const char *name, double value)
{
   if (!w || !name || !name[0])
      return;
   if (strcmp(name, "lexical_key") == 0)
      w->lexical_key = value;
   else if (strcmp(name, "lexical_content") == 0)
      w->lexical_content = value;
   else if (strcmp(name, "long_token") == 0)
      w->lexical_long_token_bonus = value;
   else if (strcmp(name, "coverage") == 0)
      w->coverage = value;
   else if (strcmp(name, "entity") == 0)
      w->entity = value;
   else if (strcmp(name, "temporal") == 0)
      w->temporal = value;
   else if (strcmp(name, "evidence") == 0)
      w->evidence = value;
   else if (strcmp(name, "semantic") == 0)
      w->semantic = value;
   else if (strcmp(name, "state") == 0)
      w->state = value;
   else if (strcmp(name, "workflow_intent") == 0)
      w->workflow_intent = value;
   else if (strcmp(name, "entity_intent") == 0)
      w->entity_intent = value;
   else if (strcmp(name, "temporal_intent") == 0)
      w->temporal_intent = value;
   else if (strcmp(name, "salience") == 0)
      w->salience = value;
   else if (strcmp(name, "surprise") == 0)
      w->surprise = value;
   else if (strcmp(name, "speaker") == 0)
      w->speaker = value;
}

static void memory_apply_weight_profile(memory_rank_weights_t *w, const char *path)
{
   if (!w || !path || !path[0])
      return;
   FILE *fp = fopen(path, "r");
   if (!fp)
      return;
   char line[256];
   while (fgets(line, sizeof(line), fp))
   {
      char *p = line;
      while (*p && isspace((unsigned char)*p))
         p++;
      if (!*p || *p == '#')
         continue;
      char *eq = strchr(p, '=');
      if (!eq)
         continue;
      *eq = '\0';
      char *name = p;
      char *value_str = eq + 1;
      char *end = value_str + strlen(value_str);
      while (end > value_str && isspace((unsigned char)end[-1]))
         *--end = '\0';
      while (*value_str && isspace((unsigned char)*value_str))
         value_str++;
      end = name + strlen(name);
      while (end > name && isspace((unsigned char)end[-1]))
         *--end = '\0';
      char *parse_end = NULL;
      double value = strtod(value_str, &parse_end);
      if (parse_end && parse_end != value_str)
         memory_apply_weight_kv(w, name, value);
   }
   fclose(fp);
}

int memory_skip_persistent_side_effects(void)
{
   return db2_vector_index_sync_suppressed();
}

void memory_load_pagerank_config(memory_pagerank_config_t *cfg)
{
   if (!cfg)
      return;
   memset(cfg, 0, sizeof(*cfg));
   cfg->enabled = 0;
   cfg->iterations = 6;
   cfg->weight = 0.35;
   snprintf(cfg->relations, sizeof(cfg->relations), "%s", "depends_on,related_to,co_edited,fixes");

   config_t app_cfg;
   if (config_load(&app_cfg) == 0)
   {
      if (app_cfg.memory_pagerank_enabled)
         cfg->enabled = 1;
      if (app_cfg.memory_pagerank_iterations > 0)
         cfg->iterations = app_cfg.memory_pagerank_iterations;
      if (app_cfg.memory_pagerank_weight > 0.0)
         cfg->weight = app_cfg.memory_pagerank_weight;
      if (app_cfg.memory_pagerank_relations[0])
         snprintf(cfg->relations, sizeof(cfg->relations), "%s", app_cfg.memory_pagerank_relations);
   }

   cfg->enabled = memory_env_int("AIMEE_MEMORY_PAGERANK_ENABLED", cfg->enabled, 0, 1);
   cfg->iterations = memory_env_int("AIMEE_MEMORY_PAGERANK_ITERATIONS", cfg->iterations, 1, 16);
   cfg->weight = memory_env_weight("AIMEE_MEMORY_PAGERANK_WEIGHT", cfg->weight);
   const char *rels = getenv("AIMEE_MEMORY_PAGERANK_RELATIONS");
   if (rels && rels[0])
      snprintf(cfg->relations, sizeof(cfg->relations), "%s", rels);
}

static int memory_relation_allowed(const char *allowed_csv, const char *relation)
{
   if (!relation || !relation[0])
      return 0;
   if (!allowed_csv || !allowed_csv[0])
      return 1;

   char buf[256];
   snprintf(buf, sizeof(buf), "%s", allowed_csv);
   for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
   {
      while (*tok && isspace((unsigned char)*tok))
         tok++;
      char *end = tok + strlen(tok);
      while (end > tok && isspace((unsigned char)end[-1]))
         *--end = '\0';
      if (*tok && strcmp(tok, relation) == 0)
         return 1;
   }
   return 0;
}

static int memory_find_index_by_id(const memory_t *matches, int count, int64_t memory_id)
{
   if (!matches || count <= 0 || memory_id <= 0)
      return -1;
   for (int i = 0; i < count; i++)
   {
      if (matches[i].id == memory_id)
         return i;
   }
   return -1;
}

int memory_append_linked_neighbors(memory_t *matches, int count, int max_count,
                                   const char *allowed_relations)
{
   if (!matches || count <= 0 || count >= max_count)
      return count;

   int seed_count = count;
   for (int i = 0; i < seed_count && count < max_count; i++)
   {
      memory_link_t links[64];
      int n = db2_memory_link_query(matches[i].id, links, (int)(sizeof(links) / sizeof(links[0])));
      for (int j = 0; j < n && count < max_count; j++)
      {
         int64_t neighbor_id =
             links[j].source_id == matches[i].id ? links[j].target_id : links[j].source_id;
         if (neighbor_id <= 0 || !memory_relation_allowed(allowed_relations, links[j].relation))
            continue;
         if (memory_find_index_by_id(matches, count, neighbor_id) >= 0)
            continue;
         if (memory_get(neighbor_id, &matches[count]) == 0)
            count++;
      }
   }
   return count;
}

int memory_compute_pagerank_scores(const memory_t *matches, int count,
                                   const memory_pagerank_config_t *cfg,
                                   memory_pagerank_score_t *out, int max_out)
{
   int64_t t0 = memory_now_ns();
   int edge_count = 0;
   if (!matches || count <= 1 || !cfg || !cfg->enabled || cfg->weight <= 0.0 || !out ||
       max_out < count)
   {
      memory_record_pagerank_stats(0.0, count > 0 ? count : 0, 0);
      return 0;
   }

   double adjacency[128][128];
   double scores[128];
   double next_scores[128];
   memset(adjacency, 0, sizeof(adjacency));

   for (int i = 0; i < count; i++)
      scores[i] = 1.0 / (double)count;

   for (int i = 0; i < count; i++)
   {
      memory_link_t links[64];
      int n = db2_memory_link_query(matches[i].id, links, (int)(sizeof(links) / sizeof(links[0])));
      for (int k = 0; k < n; k++)
      {
         int64_t other_id =
             links[k].source_id == matches[i].id ? links[k].target_id : links[k].source_id;
         int j = memory_find_index_by_id(matches, count, other_id);
         if (j < 0 || i == j || !memory_relation_allowed(cfg->relations, links[k].relation))
            continue;
         adjacency[i][j] += 1.0;
         adjacency[j][i] += 1.0;
         edge_count += 2;
      }
   }

   const double damping = 0.85;
   for (int iter = 0; iter < cfg->iterations; iter++)
   {
      for (int i = 0; i < count; i++)
         next_scores[i] = (1.0 - damping) / (double)count;

      for (int i = 0; i < count; i++)
      {
         double out_degree = 0.0;
         for (int j = 0; j < count; j++)
            out_degree += adjacency[i][j];
         if (out_degree <= 0.0)
         {
            double share = damping * scores[i] / (double)count;
            for (int j = 0; j < count; j++)
               next_scores[j] += share;
            continue;
         }
         for (int j = 0; j < count; j++)
         {
            if (adjacency[i][j] <= 0.0)
               continue;
            next_scores[j] += damping * scores[i] * (adjacency[i][j] / out_degree);
         }
      }

      for (int i = 0; i < count; i++)
         scores[i] = next_scores[i];
   }

   double max_score = 0.0;
   for (int i = 0; i < count; i++)
   {
      if (scores[i] > max_score)
         max_score = scores[i];
   }
   if (max_score <= 0.0)
   {
      memory_record_pagerank_stats((double)(memory_now_ns() - t0) / 1e6, count, edge_count);
      return 0;
   }

   for (int i = 0; i < count; i++)
   {
      out[i].memory_id = matches[i].id;
      out[i].score = (scores[i] / max_score) * cfg->weight;
   }
   memory_record_pagerank_stats((double)(memory_now_ns() - t0) / 1e6, count, edge_count);
   return count;
}

double memory_pagerank_bonus_lookup(const memory_pagerank_score_t *scores, int count,
                                    int64_t memory_id)
{
   if (!scores || count <= 0 || memory_id <= 0)
      return 0.0;
   for (int i = 0; i < count; i++)
   {
      if (scores[i].memory_id == memory_id)
         return scores[i].score;
   }
   return 0.0;
}

/* row_to_memory_db is gone; the 12-column row mapper now lives in db2 as
 * db2_fill_memory_12col, callable by every db2 read primitive that returns
 * memory_t. */

static int memory_unique_token_count(char tokens[][64], int count)
{
   int unique = 0;
   for (int i = 0; i < count; i++)
   {
      int seen = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(tokens[i], tokens[j]) == 0)
         {
            seen = 1;
            break;
         }
      }
      if (!seen)
         unique++;
   }
   return unique;
}

static int memory_token_present(char tokens[][64], int count, const char *token)
{
   if (!token || !token[0])
      return 0;
   for (int i = 0; i < count; i++)
   {
      if (strcmp(tokens[i], token) == 0)
         return 1;
   }
   return 0;
}

double memory_content_salience(const char *content)
{
   if (!content || !content[0])
      return 0.5;
   return is_noise_utterance(content) ? 0.05 : 0.5;
}

int memory_surprise_enabled(void)
{
   config_t cfg;
   config_load(&cfg);
   return memory_env_int("AIMEE_MEMORY_SURPRISE_ENABLED", cfg.memory_surprise_enabled, 0, 1);
}

double memory_surprise_weight(void)
{
   config_t cfg;
   config_load(&cfg);
   double fallback = cfg.memory_surprise_weight > 0.0 ? cfg.memory_surprise_weight : 0.8;
   return memory_env_weight("AIMEE_MEMORY_SURPRISE_WEIGHT", fallback);
}

static int memory_surprise_window_size(void)
{
   config_t cfg;
   config_load(&cfg);
   int fallback = cfg.memory_salience_window_size > 0 ? cfg.memory_salience_window_size : 8;
   return memory_env_int("AIMEE_MEMORY_SALIENCE_WINDOW_SIZE", fallback, 1, 64);
}

double memory_content_surprise(const char *session_id, const char *content)
{
   if (!session_id || !session_id[0] || !content || !content[0])
      return 0.5;

   char norm[1024];
   normalize_key(content, norm, sizeof(norm));
   char current[32][64];
   int current_count = memory_split_tokens(norm, current, 32);
   if (current_count <= 0)
      return 0.5;

   const int window_size = memory_surprise_window_size();
   db2_memory_prior_row_t window_rows[64];
   int window_cap = (int)(sizeof(window_rows) / sizeof(window_rows[0]));
   /* INT64_MAX yields the unconstrained "newest first" walk that the original
    * SQL did; the existing helper just adds a defensive id < N upper bound
    * which is harmless here. */
   int got = db2_memory_list_prior_in_session(session_id, INT64_MAX, window_size, window_rows,
                                              window_cap);

   char prior[128][64];
   int prior_count = 0;
   for (int r = 0; r < got && prior_count < 128; r++)
   {
      const char *prev_content = window_rows[r].content;
      if (!prev_content[0])
         continue;
      char prev_norm[1024];
      normalize_key(prev_content, prev_norm, sizeof(prev_norm));
      char prev_tokens[24][64];
      int prev_count = memory_split_tokens(prev_norm, prev_tokens, 24);
      for (int i = 0; i < prev_count && prior_count < 128; i++)
      {
         if (!memory_token_present(prior, prior_count, prev_tokens[i]))
            snprintf(prior[prior_count++], sizeof(prior[0]), "%s", prev_tokens[i]);
      }
   }

   int current_unique = memory_unique_token_count(current, current_count);
   if (current_unique <= 0)
      return 0.5;
   if (prior_count <= 0)
      return 1.0;

   int intersection = 0;
   int novel = 0;
   for (int i = 0; i < current_count; i++)
   {
      int seen = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(current[i], current[j]) == 0)
         {
            seen = 1;
            break;
         }
      }
      if (seen)
         continue;
      if (memory_token_present(prior, prior_count, current[i]))
         intersection++;
      else
         novel++;
   }

   int union_count = current_unique + prior_count - intersection;
   if (union_count <= 0)
      return 0.5;

   double jaccard_distance = 1.0 - ((double)intersection / (double)union_count);
   double novelty_bonus = novel > 0 ? fmin(0.2, 0.05 * (double)novel) : 0.0;
   double score = jaccard_distance + novelty_bonus;
   if (score < 0.05)
      score = 0.05;
   if (score > 1.0)
      score = 1.0;
   return score;
}

void add_provenance(int64_t memory_id, const char *session_id, const char *action,
                    const char *details)
{
   db2_memory_provenance_insert(memory_id, session_id, action, details);
}

int memory_alias_is_useful_token(const char *token)
{
   if (!token || !token[0])
      return 0;

   size_t len = strlen(token);
   if (len < 3)
      return 0;

   int alpha = 0;
   int digits = 0;
   for (size_t i = 0; token[i]; i++)
   {
      if (isalpha((unsigned char)token[i]))
         alpha++;
      else if (isdigit((unsigned char)token[i]))
         digits++;
   }

   if (alpha >= 3)
      return 1;
   return digits >= 4;
}

int memory_is_stopword_token(const char *tok)
{
   static const char *words[] = {
       "a",    "an",    "and", "are",  "as",   "at",    "be",   "but",  "by", "did", "do",   "for",
       "from", "had",   "has", "have", "her",  "him",   "his",  "if",   "in", "is",  "it",   "its",
       "just", "like",  "me",  "my",   "no",   "not",   "of",   "on",   "or", "our", "so",   "also",
       "than", "that",  "the", "them", "then", "there", "they", "this", "to", "was", "were", "what",
       "when", "where", "who", "why",  "how",  "with",  "yes",  "your", NULL};
   for (int i = 0; words[i]; i++)
      if (strcmp(tok, words[i]) == 0)
         return 1;
   return 0;
}

int memory_is_signal_token(const char *tok)
{
   if (!tok || !tok[0])
      return 0;
   if (memory_is_stopword_token(tok))
      return 0;
   size_t len = strlen(tok);
   if (len >= 3)
      return 1;
   /* Allow 2-char all-uppercase tokens (initials/abbreviations like "NY", "AI") */
   if (len == 2 && isupper((unsigned char)tok[0]) && isupper((unsigned char)tok[1]))
      return 1;
   return 0;
}

void memory_canonicalize_term(const char *input, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!input || !input[0])
      return;

   char norm[256];
   normalize_key(input, norm, sizeof(norm));
   if (strncmp(norm, "the ", 4) == 0)
      memmove(norm, norm + 4, strlen(norm + 4) + 1);
   else if (strncmp(norm, "a ", 2) == 0)
      memmove(norm, norm + 2, strlen(norm + 2) + 1);
   else if (strncmp(norm, "an ", 3) == 0)
      memmove(norm, norm + 3, strlen(norm + 3) + 1);
   static const char *titles[] = {"mr ", "mrs ", "ms ", "dr ", "prof ", "sir ", NULL};
   for (int i = 0; titles[i]; i++)
   {
      size_t tlen = strlen(titles[i]);
      if (strncmp(norm, titles[i], tlen) == 0)
      {
         memmove(norm, norm + tlen, strlen(norm + tlen) + 1);
         break;
      }
   }
   size_t len = strlen(norm);
   if (len >= 2 && norm[len - 2] == '\'' && norm[len - 1] == 's')
      norm[len - 2] = '\0';
   len = strlen(norm);
   if (len > 4 && strcmp(norm + len - 3, "ies") == 0)
   {
      norm[len - 3] = 'y';
      norm[len - 2] = '\0';
   }
   else if (len > 4 && strcmp(norm + len - 2, "es") == 0)
      norm[len - 2] = '\0';
   else if (len > 4 && norm[len - 1] == 's' && norm[len - 2] != 's')
      norm[len - 1] = '\0';
   snprintf(out, out_len, "%s", norm);
}

double memory_env_weight(const char *name, double fallback)
{
   const char *v = getenv(name);
   if (!v || !v[0])
      return fallback;
   char *end = NULL;
   double d = strtod(v, &end);
   if (!end || end == v)
      return fallback;
   return d;
}

int memory_env_int(const char *name, int fallback, int min_value, int max_value)
{
   const char *v = getenv(name);
   if (!v || !v[0])
      return fallback;
   char *end = NULL;
   long n = strtol(v, &end, 10);
   if (!end || end == v)
      return fallback;
   if (n < min_value)
      n = min_value;
   if (n > max_value)
      n = max_value;
   return (int)n;
}

const char *memory_query_route_name(memory_query_route_t route)
{
   switch (route)
   {
   case MEM_ROUTE_LEXICAL:
      return "lexical";
   case MEM_ROUTE_SEMANTIC:
      return "semantic";
   case MEM_ROUTE_GRAPH:
      return "graph";
   case MEM_ROUTE_HYBRID:
   default:
      return "hybrid";
   }
}

const char *memory_query_shape_name(memory_query_shape_t shape)
{
   switch (shape)
   {
   case MEM_SHAPE_FACTOID:
      return "factoid";
   case MEM_SHAPE_LIST:
      return "list";
   case MEM_SHAPE_YES_NO:
      return "yes_no";
   case MEM_SHAPE_WHEN:
      return "when";
   case MEM_SHAPE_HOW:
      return "how";
   case MEM_SHAPE_WHY:
      return "why";
   case MEM_SHAPE_QUANTITATIVE:
      return "quantitative";
   case MEM_SHAPE_TEMPORAL_INTERVAL:
      return "temporal_interval";
   case MEM_SHAPE_UNKNOWN:
   default:
      return "unknown";
   }
}

/* memory_fetch_budget_factor: combine token-count specificity with
 * shape-aware width into a single scaling factor applied to the
 * dynamic-fetch-budget base. Short queries (few tokens) are wide /
 * ambiguous and benefit from a larger candidate pool; long, specific
 * queries converge fast with a smaller pool. Shape adjusts on top:
 * LIST / QUANTITATIVE / TEMPORAL_INTERVAL want more candidates for
 * aggregation; YES_NO converges fastest with few. See
 * docs/proposals/pending/conversational-retrieval-rerank-and-hard-negatives.md. */
double memory_fetch_budget_factor(memory_query_shape_t shape, int ntokens)
{
   double token_factor;
   if (ntokens <= 2)
      token_factor = 1.5;
   else if (ntokens <= 4)
      token_factor = 1.0;
   else if (ntokens <= 7)
      token_factor = 0.75;
   else
      token_factor = 0.5;

   double shape_factor;
   switch (shape)
   {
   case MEM_SHAPE_LIST:
   case MEM_SHAPE_QUANTITATIVE:
   case MEM_SHAPE_TEMPORAL_INTERVAL:
      shape_factor = 1.3;
      break;
   case MEM_SHAPE_WHEN:
      shape_factor = 1.15;
      break;
   case MEM_SHAPE_FACTOID:
      shape_factor = 0.9;
      break;
   case MEM_SHAPE_YES_NO:
      shape_factor = 0.75;
      break;
   case MEM_SHAPE_HOW:
   case MEM_SHAPE_WHY:
   case MEM_SHAPE_UNKNOWN:
   default:
      shape_factor = 1.0;
      break;
   }

   return token_factor * shape_factor;
}

memory_rank_weights_t memory_rank_weights(void)
{
   memory_rank_weights_t w = {2.0, 1.0, 0.5, 2.6,  1.15, 1.35, 1.0, 1.0,
                              1.2, 1.2, 0.5, 0.75, 1.0,  1.0,  1.1};
   config_t cfg;
   config_load(&cfg);
   if (cfg.memory_weight_profile[0])
      memory_apply_weight_profile(&w, cfg.memory_weight_profile);
   memory_apply_weight_profile(&w, getenv("AIMEE_MEMORY_WEIGHT_PROFILE"));
   w.lexical_key = memory_env_weight("AIMEE_MEMORY_WEIGHT_LEXICAL_KEY", w.lexical_key);
   w.lexical_content = memory_env_weight("AIMEE_MEMORY_WEIGHT_LEXICAL_CONTENT", w.lexical_content);
   w.lexical_long_token_bonus =
       memory_env_weight("AIMEE_MEMORY_WEIGHT_LONG_TOKEN", w.lexical_long_token_bonus);
   w.coverage = memory_env_weight("AIMEE_MEMORY_WEIGHT_COVERAGE", w.coverage);
   w.entity = memory_env_weight("AIMEE_MEMORY_WEIGHT_ENTITY", w.entity);
   w.temporal = memory_env_weight("AIMEE_MEMORY_WEIGHT_TEMPORAL", w.temporal);
   w.evidence = memory_env_weight("AIMEE_MEMORY_WEIGHT_EVIDENCE", w.evidence);
   w.semantic = memory_env_weight("AIMEE_MEMORY_WEIGHT_SEMANTIC", w.semantic);
   w.state = memory_env_weight("AIMEE_MEMORY_WEIGHT_STATE", w.state);
   w.workflow_intent = memory_env_weight("AIMEE_MEMORY_WEIGHT_WORKFLOW_INTENT", w.workflow_intent);
   w.entity_intent = memory_env_weight("AIMEE_MEMORY_WEIGHT_ENTITY_INTENT", w.entity_intent);
   w.temporal_intent = memory_env_weight("AIMEE_MEMORY_WEIGHT_TEMPORAL_INTENT", w.temporal_intent);
   w.salience = memory_env_weight("AIMEE_MEMORY_WEIGHT_SALIENCE", w.salience);
   w.surprise = memory_env_weight("AIMEE_MEMORY_WEIGHT_SURPRISE", w.surprise);
   w.speaker = memory_env_weight("AIMEE_MEMORY_WEIGHT_SPEAKER", w.speaker);
   /* Inline YAML overrides: bm25_weight scales all lexical components;
    * semantic_weight scales the semantic component directly. */
   if (cfg.memory_bm25_weight > 0.0)
   {
      double bm25_scale = cfg.memory_bm25_weight;
      w.lexical_key *= bm25_scale;
      w.lexical_content *= bm25_scale;
      w.lexical_long_token_bonus *= bm25_scale;
   }
   if (cfg.memory_semantic_weight > 0.0)
      w.semantic = cfg.memory_semantic_weight;
   return w;
}

int memory_salience_enabled(void)
{
   config_t cfg;
   config_load(&cfg);
   return memory_env_int("AIMEE_MEMORY_SALIENCE_ENABLED", cfg.memory_salience_enabled, 0, 1);
}

double memory_salience_weight(void)
{
   config_t cfg;
   config_load(&cfg);
   double fallback = cfg.memory_salience_weight > 0.0 ? cfg.memory_salience_weight : 0.6;
   return memory_env_weight("AIMEE_MEMORY_SALIENCE_WEIGHT", fallback);
}

static const char *memory_rerank_mode_effective(const config_t *cfg)
{
   const char *env = getenv("AIMEE_MEMORY_RERANK_MODE");
   if (env && env[0])
      return env;
   if (cfg && cfg->memory_rerank_mode[0])
      return cfg->memory_rerank_mode;
   return "fast";
}

int memory_rerank_is_slow(void)
{
   config_t cfg;
   config_load(&cfg);
   return strcmp(memory_rerank_mode_effective(&cfg), "slow") == 0;
}

void memory_runtime_state_increment(const char *key, int delta)
{
   if (db1_runtime_state_add_int)
      (void)db1_runtime_state_add_int(key, delta, NULL);
}

void memory_maybe_run_maintenance(void)
{
   static int maintenance_running = 0;

   if (maintenance_running)
      return;

   config_t cfg;
   config_load(&cfg);
   int trigger_inserts =
       cfg.memory_maintenance_trigger_inserts > 0 ? cfg.memory_maintenance_trigger_inserts : 20;
   int trigger_secs =
       cfg.memory_maintenance_trigger_secs > 0 ? cfg.memory_maintenance_trigger_secs : 600;
   const char *env_inserts = getenv("AIMEE_MEMORY_MAINTENANCE_TRIGGER_INSERTS");
   if (env_inserts && env_inserts[0])
   {
      int v = atoi(env_inserts);
      if (v > 0)
         trigger_inserts = v;
   }
   const char *env_secs = getenv("AIMEE_MEMORY_MAINTENANCE_TRIGGER_SECS");
   if (env_secs && env_secs[0])
   {
      int v = atoi(env_secs);
      if (v > 0)
         trigger_secs = v;
   }

   char state_buf[32];
   int pending = 1;
   if (!db1_runtime_state_get || !db1_runtime_state_set)
      return;
   if (db1_runtime_state_get("maintenance_pending_writes", state_buf, sizeof(state_buf)) == 0 &&
       state_buf[0])
      pending = atoi(state_buf) + 1;
   snprintf(state_buf, sizeof(state_buf), "%d", pending);
   (void)db1_runtime_state_set("maintenance_pending_writes", state_buf);

   time_t now = time(NULL);
   int last_run = 0;
   if (db1_runtime_state_get("maintenance_last_run_epoch", state_buf, sizeof(state_buf)) == 0 &&
       state_buf[0])
      last_run = atoi(state_buf);
   int should_run = 0;
   if (trigger_inserts > 0 && pending >= trigger_inserts)
      should_run = 1;
   if (!should_run && trigger_secs > 0 && last_run > 0 &&
       (int)(now - (time_t)last_run) >= trigger_secs)
      should_run = 1;
   if (last_run == 0)
   {
      snprintf(state_buf, sizeof(state_buf), "%d", (int)now);
      (void)db1_runtime_state_set("maintenance_last_run_epoch", state_buf);
   }
   if (!should_run)
      return;

   maintenance_running = 1;

   int promoted = 0, demoted = 0, expired = 0;
   if (memory_run_maintenance(&promoted, &demoted, &expired) == 0)
   {
      (void)db1_runtime_state_set("maintenance_pending_writes", "0");
      snprintf(state_buf, sizeof(state_buf), "%d", (int)now);
      (void)db1_runtime_state_set("maintenance_last_run_epoch", state_buf);
   }

   /* Lifecycle pending-TTL sweep: runs on every maintenance cycle so
    * stale `pending` commitments age into `archived` deterministically.
    * Idempotent — memory_lifecycle_sweep_expired() only touches rows
    * whose ttl_at has actually crossed now(), so interrupting mid-batch
    * and resuming cannot double-archive. */
   if (cfg.memory_lifecycle_enabled)
      memory_lifecycle_sweep_expired();

   /* Scene clustering: run when pending >= 10x normal threshold and scenes enabled */
   if (cfg.memory_scenes_enabled && pending >= trigger_inserts * 10)
      memory_cluster_scenes("");

   maintenance_running = 0;
}

void memory_alias_join_tokens(char *buf, size_t buf_len, char tokens[][64], int start, int count)
{
   size_t used = 0;
   if (!buf || buf_len == 0)
      return;
   buf[0] = '\0';

   for (int i = 0; i < count; i++)
   {
      const char *tok = tokens[start + i];
      size_t tlen = strlen(tok);
      if (used > 0 && used < buf_len - 1)
         buf[used++] = ' ';
      if (used + tlen >= buf_len)
         break;
      memcpy(buf + used, tok, tlen);
      used += tlen;
      buf[used] = '\0';
   }
}

void memory_alias_insert(int64_t memory_id, const char *alias, double weight)
{
   char norm_alias[256];
   normalize_key(alias, norm_alias, sizeof(norm_alias));
   if (!norm_alias[0])
      return;
   db2_memory_alias_insert(memory_id, norm_alias, weight);
}

double memory_base_evidence_strength(const char *key, const char *content, double confidence)
{
   double score = 0.35 + confidence * 0.45;
   if (key && key[0])
      score += 0.05;
   if (content && content[0] && gate_has_evidence_markers(content))
      score += 0.15;
   if (content && (strstr(content, "202") || strstr(content, "201") || strstr(content, "Jan") ||
                   strstr(content, "Feb") || strstr(content, "Mar") || strstr(content, "Apr") ||
                   strstr(content, "May") || strstr(content, "Jun") || strstr(content, "Jul") ||
                   strstr(content, "Aug") || strstr(content, "Sep") || strstr(content, "Oct") ||
                   strstr(content, "Nov") || strstr(content, "Dec")))
      score += 0.05;
   if (score > 1.0)
      score = 1.0;
   if (score < 0.1)
      score = 0.1;
   return score;
}

void memory_entity_insert(int64_t memory_id, const char *entity, const char *role, double weight)
{
   char norm[256];
   memory_canonicalize_term(entity, norm, sizeof(norm));
   if (!norm[0])
      return;
   db2_memory_entity_insert(memory_id, norm, role, weight);
}

void memory_temporal_insert(int64_t memory_id, const char *ref_key, const char *granularity,
                            double weight)
{
   char norm[128];
   normalize_key(ref_key, norm, sizeof(norm));
   if (!norm[0])
      return;
   db2_memory_temporal_insert(memory_id, norm, granularity, weight);
}

/* Extract a speaker name from raw content.  Recognises "Name: text" and
 * "Name Surname: text" patterns at the start of a turn.  Returns 1 and
 * fills *out* on success, 0 otherwise. */
static int memory_extract_speaker_from_content(const char *content, char *out, size_t out_len)
{
   if (!content || !content[0] || !out || out_len == 0)
      return 0;
   out[0] = '\0';

   /* Find the first colon in the content */
   const char *colon = strchr(content, ':');
   if (!colon || colon == content)
      return 0;

   /* Speaker prefix must be within the first 48 chars */
   if (colon - content > 48)
      return 0;

   /* The prefix before the colon must be non-empty and contain only word
    * chars and spaces (no digits, no punctuation besides space and hyphen) */
   char prefix[64];
   size_t plen = (size_t)(colon - content);
   if (plen == 0 || plen >= sizeof(prefix))
      return 0;
   memcpy(prefix, content, plen);
   prefix[plen] = '\0';

   /* Validate: alphanumeric + space + hyphen only; at least one letter */
   int has_letter = 0;
   for (size_t i = 0; i < plen; i++)
   {
      unsigned char ch = (unsigned char)prefix[i];
      if (isalpha(ch))
         has_letter = 1;
      else if (!isspace(ch) && ch != '-')
         return 0;
   }
   if (!has_letter)
      return 0;

   /* Strip leading/trailing spaces from the prefix */
   char *start = prefix;
   while (*start == ' ')
      start++;
   char *end = start + strlen(start);
   while (end > start && end[-1] == ' ')
      end--;
   *end = '\0';
   if (!start[0])
      return 0;

   snprintf(out, out_len, "%s", start);
   return 1;
}

void memory_refresh_entities(int64_t memory_id, const char *key, const char *content)
{
   db2_memory_entities_delete_for_memory(memory_id);

   if (key && key[0])
      memory_entity_insert(memory_id, key, "key", 3.0);

   /* Speaker detection: "Name: text" pattern -> insert as "actor" role */
   {
      char speaker[64];
      if (memory_extract_speaker_from_content(content, speaker, sizeof(speaker)))
         memory_entity_insert(memory_id, speaker, "actor", 2.8);
   }

   char norm[2048];
   char tokens[24][64];
   normalize_key(content ? content : "", norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 24);
   if (token_count <= 0)
      return;

   char phrase[256];
   for (int i = 0; i < token_count; i++)
   {
      size_t len = strlen(tokens[i]);
      if (len >= 5 && !memory_is_stopword_token(tokens[i]) &&
          !memory_is_likely_action_token(tokens[i]) && !memory_is_month_token(tokens[i]) &&
          !memory_is_weekday_token(tokens[i]) && !memory_is_relation_token(tokens[i]) &&
          !memory_is_probable_location_token(tokens[i]))
         memory_entity_insert(memory_id, tokens[i], "term", len >= 8 ? 1.5 : 1.0);

      if (i + 1 < token_count && strlen(tokens[i]) >= 4 && strlen(tokens[i + 1]) >= 4 &&
          !memory_is_stopword_token(tokens[i]) && !memory_is_stopword_token(tokens[i + 1]) &&
          !memory_is_relation_token(tokens[i]) && !memory_is_relation_token(tokens[i + 1]) &&
          !memory_is_likely_action_token(tokens[i]) &&
          !memory_is_likely_action_token(tokens[i + 1]))
      {
         memory_alias_join_tokens(phrase, sizeof(phrase), tokens, i, 2);
         if (phrase[0])
            memory_entity_insert(memory_id, phrase, "phrase", 2.0);
      }

      if (i + 2 < token_count && strlen(tokens[i]) >= 4 && strlen(tokens[i + 1]) >= 4 &&
          strlen(tokens[i + 2]) >= 4 && !memory_is_stopword_token(tokens[i]) &&
          !memory_is_stopword_token(tokens[i + 1]) && !memory_is_stopword_token(tokens[i + 2]) &&
          !memory_is_likely_action_token(tokens[i]) &&
          !memory_is_likely_action_token(tokens[i + 1]) &&
          !memory_is_likely_action_token(tokens[i + 2]))
      {
         memory_alias_join_tokens(phrase, sizeof(phrase), tokens, i, 3);
         if (phrase[0])
            memory_entity_insert(memory_id, phrase, "phrase", 2.5);
      }
   }
}

int memory_is_month_token(const char *tok)
{
   static const char *months[] = {
       "january", "february", "march",    "april", "may", "june", "july", "august", "september",
       "october", "november", "december", "jan",   "feb", "mar",  "apr",  "jun",    "jul",
       "aug",     "sep",      "sept",     "oct",   "nov", "dec",  NULL};
   for (int i = 0; months[i]; i++)
   {
      if (strcmp(tok, months[i]) == 0)
         return 1;
   }
   return 0;
}

int memory_is_weekday_token(const char *tok)
{
   static const char *days[] = {"monday", "tuesday",  "wednesday", "thursday",
                                "friday", "saturday", "sunday",    NULL};
   for (int i = 0; days[i]; i++)
   {
      if (strcmp(tok, days[i]) == 0)
         return 1;
   }
   return 0;
}

int memory_month_number(const char *tok)
{
   static const char *months[] = {
       "january", "february", "march",    "april", "may", "june", "july", "august", "september",
       "october", "november", "december", "jan",   "feb", "mar",  "apr",  "jun",    "jul",
       "aug",     "sep",      "sept",     "oct",   "nov", "dec",  NULL};
   for (int i = 0; months[i]; i++)
   {
      if (strcmp(tok, months[i]) == 0)
      {
         if (i < 12)
            return i + 1;
         return i - 11;
      }
   }
   return 0;
}

int memory_parse_created_date(int64_t memory_id, int *year, int *month, int *day)
{
   return db2_memory_parse_created_date(memory_id, year, month, day);
}

void memory_format_date(char *buf, size_t buf_len, int year, int month, int day)
{
   if (!buf || buf_len == 0)
      return;
   snprintf(buf, buf_len, "%04d-%02d-%02d", year, month, day);
}

void memory_shift_day(int *year, int *month, int *day, int delta)
{
   if (!year || !month || !day || delta == 0)
      return;
   struct tm tmv;
   memset(&tmv, 0, sizeof(tmv));
   tmv.tm_year = *year - 1900;
   tmv.tm_mon = *month - 1;
   tmv.tm_mday = *day + delta;
   time_t t = timegm(&tmv);
   gmtime_r(&t, &tmv);
   *year = tmv.tm_year + 1900;
   *month = tmv.tm_mon + 1;
   *day = tmv.tm_mday;
}

int memory_is_probable_location_token(const char *tok)
{
   static const char *words[] = {"home",     "office",  "school", "park",   "restaurant",
                                 "hospital", "airport", "hotel",  "beach",  "store",
                                 "room",     "city",    "town",   "campus", NULL};
   for (int i = 0; words[i]; i++)
      if (strcmp(tok, words[i]) == 0)
         return 1;
   return 0;
}

int memory_is_relation_token(const char *tok)
{
   static const char *words[] = {"at",   "in",   "on", "to",    "from",   "with",   "for", "into",
                                 "onto", "near", "by", "after", "before", "during", NULL};
   for (int i = 0; words[i]; i++)
      if (strcmp(tok, words[i]) == 0)
         return 1;
   return 0;
}

int memory_is_likely_action_token(const char *tok)
{
   static const char *verbs[] = {
       "go",         "went",     "meet",      "met",       "call",       "called",  "visit",
       "visited",    "join",     "joined",    "work",      "worked",     "start",   "started",
       "finish",     "finished", "travel",    "traveled",  "move",       "moved",   "buy",
       "bought",     "learn",    "learned",   "study",     "studied",    "eat",     "ate",
       "talk",       "talked",   "support",   "supported", "plan",       "planned", "celebrate",
       "celebrated", "install",  "installed", "configure", "configured", "setup",   "set",
       "deploy",     "deployed", NULL};
   for (int i = 0; verbs[i]; i++)
      if (strcmp(tok, verbs[i]) == 0)
         return 1;
   size_t len = strlen(tok);
   return len > 3 && (strcmp(tok + len - 2, "ed") == 0 || strcmp(tok + len - 3, "ing") == 0);
}

int memory_is_coref_pronoun_token(const char *tok)
{
   static const char *pronouns[] = {"she", "her", "he", "him", "they", "them", "their", NULL};
   if (!tok || !tok[0])
      return 0;
   for (int i = 0; pronouns[i]; i++)
      if (strcmp(tok, pronouns[i]) == 0)
         return 1;
   return 0;
}

const char *memory_coref_mode_effective(const config_t *cfg)
{
   const char *env = getenv("AIMEE_MEMORY_COREF_MODE");
   if (env && env[0])
      return env;
   if (cfg && cfg->memory_coref_mode[0])
      return cfg->memory_coref_mode;
   return "off";
}

int memory_coref_window_effective(const config_t *cfg)
{
   int fallback = (cfg && cfg->memory_coref_window > 0) ? cfg->memory_coref_window : 5;
   return memory_env_int("AIMEE_MEMORY_COREF_WINDOW", fallback, 1, 12);
}

int memory_coref_has_pronoun(const char *content)
{
   char norm[1024];
   char tokens[32][64];
   normalize_key(content ? content : "", norm, sizeof(norm));
   int count = memory_split_tokens(norm, tokens, 32);
   for (int i = 0; i < count; i++)
   {
      if (memory_is_coref_pronoun_token(tokens[i]))
         return 1;
   }
   return 0;
}

int memory_extract_named_entities(const char *text, char names[][128], int max_names)
{
   int count = 0;
   int i = 0;
   if (!text || !text[0] || !names || max_names <= 0)
      return 0;

   while (text[i] && count < max_names)
   {
      while (text[i] && !isalpha((unsigned char)text[i]))
         i++;
      if (!text[i])
         break;
      if (!isupper((unsigned char)text[i]))
      {
         while (text[i] && isalpha((unsigned char)text[i]))
            i++;
         continue;
      }

      char phrase[128];
      size_t used = 0;
      int words = 0;
      while (text[i] && words < 3)
      {
         if (!isupper((unsigned char)text[i]))
            break;
         int start = i;
         while (text[i] && isalpha((unsigned char)text[i]))
            i++;
         int len = i - start;
         if (len <= 0 || (size_t)len + used + 2 >= sizeof(phrase))
            break;
         if (used > 0)
            phrase[used++] = ' ';
         memcpy(phrase + used, text + start, (size_t)len);
         used += (size_t)len;
         words++;
         while (text[i] == ' ')
            i++;
         if (!isupper((unsigned char)text[i]))
            break;
      }
      phrase[used] = '\0';
      if (words <= 0)
         continue;

      char canonical[128];
      memory_canonicalize_term(phrase, canonical, sizeof(canonical));
      if (!canonical[0] || strlen(canonical) < 3 || memory_is_stopword_token(canonical) ||
          memory_is_month_token(canonical) || memory_is_weekday_token(canonical))
         continue;

      int duplicate = 0;
      for (int j = 0; j < count; j++)
      {
         if (strcmp(names[j], canonical) == 0)
         {
            duplicate = 1;
            break;
         }
      }
      if (!duplicate)
         snprintf(names[count++], 128, "%s", canonical);
   }

   return count;
}

/* LLM-assisted coreference: send context window to the cognifier and use its
 * coref_bindings output.  This is the "llm" mode path for coref resolution.
 * Returns 1 if a binding was inserted, 0 otherwise. */
int memory_coref_llm_resolve(int64_t memory_id, const char *content, const char *session_buf,
                             const config_t *cfg)
{
   if (!cfg->memory_cognify_command[0])
      return 0;

   /* Collect prior context window */
   db2_memory_prior_row_t prior[64];
   int prior_max = (int)(sizeof(prior) / sizeof(prior[0]));
   int prior_n = db2_memory_list_prior_in_session(
       session_buf, memory_id, memory_coref_window_effective(cfg), prior, prior_max);

   cJSON *context_arr = cJSON_CreateArray();
   for (int i = 0; i < prior_n; i++)
   {
      if (prior[i].content[0])
         cJSON_AddItemToArray(context_arr, cJSON_CreateString(prior[i].content));
   }

   cJSON *input = cJSON_CreateObject();
   if (!input)
   {
      cJSON_Delete(context_arr);
      return 0;
   }
   cJSON_AddStringToObject(input, "task", "coref");
   cJSON_AddNumberToObject(input, "memory_id", (double)memory_id);
   cJSON_AddStringToObject(input, "content", content);
   cJSON_AddItemToObject(input, "context", context_arr);

   char *input_str = cJSON_PrintUnformatted(input);
   cJSON_Delete(input);
   if (!input_str)
      return 0;

   char *resp = NULL;
   size_t resp_len = 0;
   int rc = platform_exec_pipe(cfg->memory_cognify_command, input_str, strlen(input_str), &resp,
                               &resp_len);
   free(input_str);
   if (rc != 0 || !resp || resp_len == 0)
   {
      free(resp);
      return 0;
   }

   memory_cognify_result_t result;
   rc = memory_cognify_parse_response(resp, &result);
   free(resp);
   if (rc != 0 || result.coref_count == 0)
      return 0;

   /* Pick the highest-confidence binding that exceeds the ambiguity threshold */
   const cognify_coref_binding_t *best = NULL;
   for (int i = 0; i < result.coref_count; i++)
   {
      const cognify_coref_binding_t *b = &result.coref_bindings[i];
      if (!b->entity[0] || b->confidence < 0.5)
         continue;
      if (!best || b->confidence > best->confidence)
         best = b;
   }
   if (!best)
      return 0;

   memory_entity_insert(memory_id, best->entity, "coref", 2.8);
   return 1;
}

/* In-process coreference outcome counters. */
static pthread_mutex_t s_coref_stats_mu = PTHREAD_MUTEX_INITIALIZER;
static memory_coref_stats_t s_coref_stats;

void memory_coref_stats(memory_coref_stats_t *out)
{
   if (!out)
      return;
   pthread_mutex_lock(&s_coref_stats_mu);
   *out = s_coref_stats;
   pthread_mutex_unlock(&s_coref_stats_mu);
}

void memory_coref_stats_reset(void)
{
   pthread_mutex_lock(&s_coref_stats_mu);
   memset(&s_coref_stats, 0, sizeof(s_coref_stats));
   pthread_mutex_unlock(&s_coref_stats_mu);
}

/* Record one coref resolution attempt in the audit table. */
void memory_coref_audit_record(int64_t memory_id, const char *session_id, const char *outcome,
                               const char *entity, const char *mode, double confidence)
{
   /* Update in-process counters. */
   const char *eff_outcome = outcome ? outcome : "none";
   pthread_mutex_lock(&s_coref_stats_mu);
   if (strcmp(eff_outcome, "bound") == 0)
      s_coref_stats.bound++;
   else if (strcmp(eff_outcome, "ambiguous") == 0)
      s_coref_stats.ambiguous++;
   else
      s_coref_stats.unbound++;
   pthread_mutex_unlock(&s_coref_stats_mu);

   db2_memory_coref_audit_insert(memory_id, session_id, eff_outcome, entity, mode, confidence);
}
