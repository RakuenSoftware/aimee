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

static void memory_fill_pagerank_stats(memory_stats_t *out)
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

static int memory_skip_persistent_side_effects(void)
{
   return db2_vector_index_sync_suppressed();
}

static void memory_load_pagerank_config(memory_pagerank_config_t *cfg)
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

static int memory_append_linked_neighbors(memory_t *matches, int count, int max_count,
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

static int memory_compute_pagerank_scores(const memory_t *matches, int count,
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

static double memory_pagerank_bonus_lookup(const memory_pagerank_score_t *scores, int count,
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

static double memory_content_salience(const char *content)
{
   if (!content || !content[0])
      return 0.5;
   return is_noise_utterance(content) ? 0.05 : 0.5;
}

static int memory_surprise_enabled(void)
{
   config_t cfg;
   config_load(&cfg);
   return memory_env_int("AIMEE_MEMORY_SURPRISE_ENABLED", cfg.memory_surprise_enabled, 0, 1);
}

static double memory_surprise_weight(void)
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

static double memory_content_surprise(const char *session_id, const char *content)
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

static int memory_alias_is_useful_token(const char *token)
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

static int memory_is_stopword_token(const char *tok)
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

static int memory_is_signal_token(const char *tok)
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

static void memory_canonicalize_term(const char *input, char *out, size_t out_len)
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

static double memory_env_weight(const char *name, double fallback)
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

static int memory_env_int(const char *name, int fallback, int min_value, int max_value)
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

static memory_rank_weights_t memory_rank_weights(void)
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

static int memory_salience_enabled(void)
{
   config_t cfg;
   config_load(&cfg);
   return memory_env_int("AIMEE_MEMORY_SALIENCE_ENABLED", cfg.memory_salience_enabled, 0, 1);
}

static double memory_salience_weight(void)
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

static int memory_rerank_is_slow(void)
{
   config_t cfg;
   config_load(&cfg);
   return strcmp(memory_rerank_mode_effective(&cfg), "slow") == 0;
}

static void memory_runtime_state_increment(const char *key, int delta)
{
   if (db1_runtime_state_add_int)
      (void)db1_runtime_state_add_int(key, delta, NULL);
}

static void memory_maybe_run_maintenance(void)
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

static void memory_alias_join_tokens(char *buf, size_t buf_len, char tokens[][64], int start,
                                     int count)
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

static void memory_alias_insert(int64_t memory_id, const char *alias, double weight)
{
   char norm_alias[256];
   normalize_key(alias, norm_alias, sizeof(norm_alias));
   if (!norm_alias[0])
      return;
   db2_memory_alias_insert(memory_id, norm_alias, weight);
}

static double memory_base_evidence_strength(const char *key, const char *content, double confidence)
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

static void memory_entity_insert(int64_t memory_id, const char *entity, const char *role,
                                 double weight)
{
   char norm[256];
   memory_canonicalize_term(entity, norm, sizeof(norm));
   if (!norm[0])
      return;
   db2_memory_entity_insert(memory_id, norm, role, weight);
}

static void memory_temporal_insert(int64_t memory_id, const char *ref_key, const char *granularity,
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

static void memory_refresh_entities(int64_t memory_id, const char *key, const char *content)
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

static int memory_is_month_token(const char *tok)
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

static int memory_is_weekday_token(const char *tok)
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

static int memory_month_number(const char *tok)
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

static int memory_parse_created_date(int64_t memory_id, int *year, int *month, int *day)
{
   return db2_memory_parse_created_date(memory_id, year, month, day);
}

static void memory_format_date(char *buf, size_t buf_len, int year, int month, int day)
{
   if (!buf || buf_len == 0)
      return;
   snprintf(buf, buf_len, "%04d-%02d-%02d", year, month, day);
}

static void memory_shift_day(int *year, int *month, int *day, int delta)
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

static int memory_is_probable_location_token(const char *tok)
{
   static const char *words[] = {"home",     "office",  "school", "park",   "restaurant",
                                 "hospital", "airport", "hotel",  "beach",  "store",
                                 "room",     "city",    "town",   "campus", NULL};
   for (int i = 0; words[i]; i++)
      if (strcmp(tok, words[i]) == 0)
         return 1;
   return 0;
}

static int memory_is_relation_token(const char *tok)
{
   static const char *words[] = {"at",   "in",   "on", "to",    "from",   "with",   "for", "into",
                                 "onto", "near", "by", "after", "before", "during", NULL};
   for (int i = 0; words[i]; i++)
      if (strcmp(tok, words[i]) == 0)
         return 1;
   return 0;
}

static int memory_is_likely_action_token(const char *tok)
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

static int memory_is_coref_pronoun_token(const char *tok)
{
   static const char *pronouns[] = {"she", "her", "he", "him", "they", "them", "their", NULL};
   if (!tok || !tok[0])
      return 0;
   for (int i = 0; pronouns[i]; i++)
      if (strcmp(tok, pronouns[i]) == 0)
         return 1;
   return 0;
}

static const char *memory_coref_mode_effective(const config_t *cfg)
{
   const char *env = getenv("AIMEE_MEMORY_COREF_MODE");
   if (env && env[0])
      return env;
   if (cfg && cfg->memory_coref_mode[0])
      return cfg->memory_coref_mode;
   return "off";
}

static int memory_coref_window_effective(const config_t *cfg)
{
   int fallback = (cfg && cfg->memory_coref_window > 0) ? cfg->memory_coref_window : 5;
   return memory_env_int("AIMEE_MEMORY_COREF_WINDOW", fallback, 1, 12);
}

static int memory_coref_has_pronoun(const char *content)
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

static int memory_extract_named_entities(const char *text, char names[][128], int max_names)
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
static int memory_coref_llm_resolve(int64_t memory_id, const char *content, const char *session_buf,
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
static void memory_coref_audit_record(int64_t memory_id, const char *session_id,
                                      const char *outcome, const char *entity, const char *mode,
                                      double confidence)
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

static void memory_refresh_coref_entities(int64_t memory_id, const char *content)
{
   if (memory_id <= 0 || !content || !content[0] || !memory_coref_has_pronoun(content))
      return;

   config_t cfg;
   config_load(&cfg);
   const char *mode = memory_coref_mode_effective(&cfg);
   if (strcmp(mode, "heuristic") != 0 && strcmp(mode, "llm") != 0)
      return;

   char session_buf[128];
   if (db2_memory_get_source_session(memory_id, session_buf, (int)sizeof(session_buf)) != 0)
      return;

   /* LLM mode: delegate to cognifier for higher-recall resolution */
   if (strcmp(mode, "llm") == 0)
   {
      int bound = memory_coref_llm_resolve(memory_id, content, session_buf, &cfg);
      memory_coref_audit_record(memory_id, session_buf, bound ? "bound" : "unbound", "", "llm",
                                bound ? 1.0 : 0.0);
      return;
   }

   /* Heuristic mode: scan prior memories for unambiguous named entity */
   db2_memory_prior_row_t prior[64];
   int prior_max = (int)(sizeof(prior) / sizeof(prior[0]));
   int prior_n = db2_memory_list_prior_in_session(
       session_buf, memory_id, memory_coref_window_effective(&cfg), prior, prior_max);

   char chosen[128] = "";
   int ambiguous = 0;
   for (int i = 0; i < prior_n; i++)
   {
      char names[8][128];
      int count = memory_extract_named_entities(prior[i].content, names, 8);
      if (count == 0)
         count = memory_extract_named_entities(prior[i].key, names, 8);
      if (count == 1)
      {
         snprintf(chosen, sizeof(chosen), "%s", names[0]);
         break;
      }
      if (count > 1)
      {
         chosen[0] = '\0';
         ambiguous = 1;
         break;
      }
   }

   if (chosen[0])
   {
      memory_entity_insert(memory_id, chosen, "coref", 2.8);
      memory_coref_audit_record(memory_id, session_buf, "bound", chosen, "heuristic", 1.0);
   }
   else
   {
      memory_coref_audit_record(memory_id, session_buf, ambiguous ? "ambiguous" : "unbound", "",
                                "heuristic", 0.0);
   }
}

static void memory_refresh_temporal_refs(int64_t memory_id, const char *key, const char *content)
{
   db2_memory_temporal_refs_delete_for_memory(memory_id);

   char norm[2048];
   char tokens[24][64];
   char phrase[128];
   normalize_key(key ? key : "", norm, sizeof(norm));
   int key_count = memory_split_tokens(norm, tokens, 24);
   int created_year = 0, created_month = 0, created_day = 0;
   int has_created =
       memory_parse_created_date(memory_id, &created_year, &created_month, &created_day);
   for (int i = 0; i < key_count; i++)
   {
      if (memory_is_month_token(tokens[i]))
         memory_temporal_insert(memory_id, tokens[i], "month", 1.8);
      else if (memory_is_weekday_token(tokens[i]))
         memory_temporal_insert(memory_id, tokens[i], "weekday", 1.5);
      else if (strlen(tokens[i]) == 4 && isdigit((unsigned char)tokens[i][0]) &&
               isdigit((unsigned char)tokens[i][1]) && isdigit((unsigned char)tokens[i][2]) &&
               isdigit((unsigned char)tokens[i][3]))
         memory_temporal_insert(memory_id, tokens[i], "year", 2.0);
   }

   normalize_key(content ? content : "", norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 24);
   for (int i = 0; i < token_count; i++)
   {
      if (strcmp(tokens[i], "today") == 0 || strcmp(tokens[i], "yesterday") == 0 ||
          strcmp(tokens[i], "tomorrow") == 0 || strcmp(tokens[i], "tonight") == 0 ||
          strcmp(tokens[i], "morning") == 0 || strcmp(tokens[i], "afternoon") == 0 ||
          strcmp(tokens[i], "evening") == 0)
      {
         memory_temporal_insert(memory_id, tokens[i], "relative", 1.8);
         if (has_created &&
             (strcmp(tokens[i], "today") == 0 || strcmp(tokens[i], "yesterday") == 0 ||
              strcmp(tokens[i], "tomorrow") == 0))
         {
            int y = created_year, m = created_month, d = created_day;
            if (strcmp(tokens[i], "yesterday") == 0)
               memory_shift_day(&y, &m, &d, -1);
            else if (strcmp(tokens[i], "tomorrow") == 0)
               memory_shift_day(&y, &m, &d, 1);
            memory_format_date(phrase, sizeof(phrase), y, m, d);
            memory_temporal_insert(memory_id, phrase, "absolute_day", 2.6);
         }
      }
      else if (memory_is_month_token(tokens[i]))
      {
         memory_temporal_insert(memory_id, tokens[i], "month", 2.0);
         if (i + 1 < token_count && strlen(tokens[i + 1]) <= 4)
         {
            memory_alias_join_tokens(phrase, sizeof(phrase), tokens, i, 2);
            if (phrase[0])
               memory_temporal_insert(memory_id, phrase, "date_phrase", 2.3);
            if (has_created)
            {
               int month_num = memory_month_number(tokens[i]);
               int day_num = atoi(tokens[i + 1]);
               if (month_num > 0 && day_num > 0 && day_num <= 31)
               {
                  memory_format_date(phrase, sizeof(phrase), created_year, month_num, day_num);
                  memory_temporal_insert(memory_id, phrase, "absolute_day", 2.8);
               }
            }
         }
      }
      else if (memory_is_weekday_token(tokens[i]))
         memory_temporal_insert(memory_id, tokens[i], "weekday", 1.7);
      else if (strlen(tokens[i]) == 4 && isdigit((unsigned char)tokens[i][0]) &&
               isdigit((unsigned char)tokens[i][1]) && isdigit((unsigned char)tokens[i][2]) &&
               isdigit((unsigned char)tokens[i][3]))
         memory_temporal_insert(memory_id, tokens[i], "year", 2.2);
      else if (strcmp(tokens[i], "before") == 0 || strcmp(tokens[i], "after") == 0 ||
               strcmp(tokens[i], "during") == 0 || strcmp(tokens[i], "first") == 0 ||
               strcmp(tokens[i], "last") == 0 || strcmp(tokens[i], "recently") == 0)
      {
         memory_temporal_insert(memory_id, tokens[i], "relative", 1.2);
         /* "last week", "last month", "last year" => resolve to approximate date range */
         if (strcmp(tokens[i], "last") == 0 && i + 1 < token_count && has_created)
         {
            int y = created_year, m = created_month, d = created_day;
            if (strcmp(tokens[i + 1], "week") == 0)
            {
               memory_shift_day(&y, &m, &d, -7);
               memory_format_date(phrase, sizeof(phrase), y, m, d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.2);
               memory_temporal_insert(memory_id, "last:week", "relative_phrase", 2.0);
            }
            else if (strcmp(tokens[i + 1], "month") == 0)
            {
               memory_temporal_insert(memory_id, "last:month", "relative_phrase", 2.0);
               /* Insert anchor for the first of last month */
               int pm = m - 1, py = y;
               if (pm < 1)
               {
                  pm = 12;
                  py--;
               }
               memory_format_date(phrase, sizeof(phrase), py, pm, 1);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.0);
            }
            else if (strcmp(tokens[i + 1], "year") == 0)
            {
               memory_temporal_insert(memory_id, "last:year", "relative_phrase", 2.0);
               char yr_buf[8];
               snprintf(yr_buf, sizeof(yr_buf), "%04d", y - 1);
               memory_temporal_insert(memory_id, yr_buf, "year", 2.2);
            }
            else if (memory_is_weekday_token(tokens[i + 1]))
            {
               /* "last Tuesday" => resolve to previous occurrence */
               static const char *wd[] = {"monday", "tuesday",  "wednesday", "thursday",
                                          "friday", "saturday", "sunday",    NULL};
               int target_wd = -1;
               for (int w = 0; wd[w]; w++)
                  if (strcmp(tokens[i + 1], wd[w]) == 0)
                  {
                     target_wd = w;
                     break;
                  }
               if (target_wd >= 0)
               {
                  struct tm tmv;
                  memset(&tmv, 0, sizeof(tmv));
                  tmv.tm_year = created_year - 1900;
                  tmv.tm_mon = created_month - 1;
                  tmv.tm_mday = created_day;
                  timegm(&tmv); /* normalise */
                  mktime(&tmv);
                  int cur_wd = tmv.tm_wday ? tmv.tm_wday - 1 : 6; /* 0=Mon */
                  int delta = cur_wd - target_wd;
                  if (delta <= 0)
                     delta += 7;
                  int ly = created_year, lm = created_month, ld = created_day;
                  memory_shift_day(&ly, &lm, &ld, -delta);
                  memory_format_date(phrase, sizeof(phrase), ly, lm, ld);
                  memory_temporal_insert(memory_id, phrase, "absolute_day", 2.4);
               }
            }
         }
         if ((strcmp(tokens[i], "before") == 0 || strcmp(tokens[i], "after") == 0 ||
              strcmp(tokens[i], "during") == 0) &&
             i + 1 < token_count)
         {
            char relation_ref[128];
            snprintf(relation_ref, sizeof(relation_ref), "%s:%s", tokens[i], tokens[i + 1]);
            memory_temporal_insert(memory_id, relation_ref, "ordering", 1.8);
         }
         if (strcmp(tokens[i], "between") == 0 && i + 2 < token_count)
         {
            char range_ref[128];
            snprintf(range_ref, sizeof(range_ref), "between:%s:%s", tokens[i + 1], tokens[i + 2]);
            memory_temporal_insert(memory_id, range_ref, "range", 2.0);
         }
      }
      /* "N days/weeks/months ago" */
      else if (i + 2 < token_count && strcmp(tokens[i + 2], "ago") == 0 && tokens[i][0] >= '1' &&
               tokens[i][0] <= '9' && has_created)
      {
         int n = atoi(tokens[i]);
         if (n > 0)
         {
            int y = created_year, m = created_month, d = created_day;
            if (strcmp(tokens[i + 1], "days") == 0 || strcmp(tokens[i + 1], "day") == 0)
            {
               memory_shift_day(&y, &m, &d, -n);
               memory_format_date(phrase, sizeof(phrase), y, m, d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.5);
               memory_temporal_insert(memory_id, "ago", "relative", 1.0);
            }
            else if (strcmp(tokens[i + 1], "weeks") == 0 || strcmp(tokens[i + 1], "week") == 0)
            {
               memory_shift_day(&y, &m, &d, -(n * 7));
               memory_format_date(phrase, sizeof(phrase), y, m, d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.4);
               memory_temporal_insert(memory_id, "ago", "relative", 1.0);
            }
            else if (strcmp(tokens[i + 1], "months") == 0 || strcmp(tokens[i + 1], "month") == 0)
            {
               int pm = m - n, py = y;
               while (pm < 1)
               {
                  pm += 12;
                  py--;
               }
               memory_format_date(phrase, sizeof(phrase), py, pm, d > 28 ? 28 : d);
               memory_temporal_insert(memory_id, phrase, "absolute_day", 2.4);
               memory_temporal_insert(memory_id, "ago", "relative", 1.0);
            }
         }
      }
   }
}

static void memory_refresh_negation_tokens(int64_t memory_id, const char *key, const char *content)
{
   config_t neg_cfg;
   if (config_load(&neg_cfg) != 0 || !neg_cfg.memory_negation_enabled)
      return;

   /* Combine key + content for tokenisation */
   char combined[3072];
   snprintf(combined, sizeof(combined), "%s %s", key ? key : "", content ? content : "");

   char neg_tokens[2048];
   int count = extract_negation_tokens(combined, neg_tokens, sizeof(neg_tokens));
   if (count <= 0)
      neg_tokens[0] = '\0';

   db2_memory_negation_tokens_update(memory_id, neg_tokens);
}

static void memory_refresh_derived_metadata(int64_t memory_id, const char *key, const char *content)
{
   memory_refresh_aliases(memory_id, key, content);
   memory_refresh_entities(memory_id, key, content);
   memory_refresh_coref_entities(memory_id, content);
   memory_refresh_temporal_refs(memory_id, key, content);
   memory_refresh_summaries(memory_id, key, content);
   memory_refresh_event_frames(memory_id, key, content);
   memory_refresh_chunks(memory_id, content);
   memory_refresh_units_graph(memory_id, key, content);
   memory_refresh_episode_relations(memory_id, key, content);
   memory_refresh_unit_embeddings(memory_id);
   memory_refresh_negation_tokens(memory_id, key, content);
}

static int memory_sync_vec_row(int64_t memory_id, const float *vec, int dim)
{
   if (!vec || dim <= 0)
      return 0;
   if (db2_vector_index_sync_suppressed())
      return 0;

   char *payload_json = db2_memory_build_memory_payload(memory_id);
   if (!payload_json)
   {
      db2_vector_index_op_record(memory_id, pgvec_memory_vector_collection_name(), memory_id, 0,
                                 "payload build failed");
      return -1;
   }
   int rc = pgvec_memory_vector_upsert_memory(memory_id, vec, dim, payload_json);
   free(payload_json);
   db2_vector_index_op_record(memory_id, pgvec_memory_vector_collection_name(), memory_id, rc == 0,
                              rc ? "upsert failed" : NULL);
   return rc;
}

static int memory_sync_unit_vec_row(int64_t unit_id, const float *vec, int dim)
{
   if (!vec || dim <= 0)
      return 0;
   if (db2_vector_index_sync_suppressed())
      return 0;

   int64_t memory_id = 0;
   char *payload_json = db2_memory_build_unit_payload(unit_id, &memory_id);
   int64_t point_id = PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET + unit_id;
   if (!payload_json)
   {
      db2_vector_index_op_record(point_id, pgvec_memory_vector_collection_name(), memory_id, 0,
                                 "payload build failed");
      return -1;
   }
   int rc = pgvec_memory_vector_upsert_unit(unit_id, vec, dim, payload_json);
   free(payload_json);
   db2_vector_index_op_record(point_id, pgvec_memory_vector_collection_name(), memory_id, rc == 0,
                              rc ? "upsert failed" : NULL);
   return rc;
}

static int memory_fetch_row_by_id(int64_t memory_id, memory_t *out)
{
   return db2_memory_get(memory_id, out);
}

static int memory_fetch_row_by_unit_id(int64_t unit_id, memory_t *out)
{
   return db2_memory_get_by_unit_id(unit_id, out);
}

static const char *memory_effective_embedding_cmd(const char *command)
{
   return (command && command[0]) ? command : "builtin";
}

/* Per-recall query-embedding memo.
 *
 * A single memory_find_facts() embeds the *same* query text once per retrieval
 * lane (unit-semantic, memory-semantic, summary/fact lanes), and the whole
 * candidate generator re-runs per HyDE pass and per decomposition sub-query.
 * Every embed forks a subprocess and round-trips to the embedder, so embedding
 * identical text repeatedly dominates recall latency. This thread-local memo
 * embeds each distinct (command,text) once for the duration of a recall and
 * serves the cached vector to the other lanes. Embedding is deterministic for a
 * given (command,text), so a cache hit is byte-identical to a fresh embed.
 *
 * Reset at each top-level recall entry point. Bounded so a pathological query
 * (many distinct sub-queries) can't grow it without limit; an overflow or an
 * over-long key simply falls through to a fresh embed (correct, just no reuse). */
enum
{
   MEMORY_QEMBED_CAP = 32,
   MEMORY_QEMBED_TEXT_MAX = 1024,
   MEMORY_QEMBED_CMD_MAX = 512
};

typedef struct
{
   char command[MEMORY_QEMBED_CMD_MAX];
   char text[MEMORY_QEMBED_TEXT_MAX];
   float vec[EMBED_MAX_DIM];
   int dim;
} memory_qembed_entry_t;

static __thread int s_qembed_count = 0;
static __thread memory_qembed_entry_t s_qembed[MEMORY_QEMBED_CAP];

/* Per-recall counters: total runtime embed requests vs cache misses (= real
 * embedder invocations). requests-minus-misses is the work the memo saved. */
static __thread int s_qembed_requests = 0;
static __thread int s_qembed_misses = 0;

/* Begin a fresh per-recall memo window. Call at every top-level recall entry
 * point before any lane embeds the query. */
static void memory_query_embed_cache_reset(void)
{
   s_qembed_count = 0;
   s_qembed_requests = 0;
   s_qembed_misses = 0;
}

static int memory_embed_text_runtime(const char *text, const char *command, float *out, int max_dim)
{
   const char *effective_cmd = memory_effective_embedding_cmd(command);
   s_qembed_requests++;

   /* Cacheable only when both keys fit their buffers (so a stored key is exact,
    * never a truncated prefix that could false-match a different query). */
   int cacheable = text && command && strlen(text) < MEMORY_QEMBED_TEXT_MAX &&
                   strlen(effective_cmd) < MEMORY_QEMBED_CMD_MAX;

   if (cacheable)
   {
      for (int i = 0; i < s_qembed_count; i++)
      {
         if (s_qembed[i].dim > 0 && s_qembed[i].dim <= max_dim &&
             strcmp(s_qembed[i].command, effective_cmd) == 0 && strcmp(s_qembed[i].text, text) == 0)
         {
            memcpy(out, s_qembed[i].vec, (size_t)s_qembed[i].dim * sizeof(float));
            return s_qembed[i].dim;
         }
      }
   }

   s_qembed_misses++;
   int dim = memory_embed_text(text, effective_cmd, out, max_dim);
   if (dim <= 0 && strcmp(effective_cmd, "builtin") != 0)
   {
      aimee_log(LOG_WARN, "memory",
                "query embedding failed for configured command; retrying with builtin embeddings");
      dim = memory_embed_text(text, "builtin", out, max_dim);
   }

   /* Store on the way out so the other lanes / sub-queries of this same recall
    * reuse the vector instead of re-spawning the embedder. */
   if (cacheable && dim > 0 && dim <= EMBED_MAX_DIM && s_qembed_count < MEMORY_QEMBED_CAP)
   {
      memory_qembed_entry_t *e = &s_qembed[s_qembed_count++];
      snprintf(e->command, sizeof(e->command), "%s", effective_cmd);
      snprintf(e->text, sizeof(e->text), "%s", text);
      memcpy(e->vec, out, (size_t)dim * sizeof(float));
      e->dim = dim;
   }
   return dim;
}

/* Store one (command,text)->vector entry into the per-recall memo (used by the
 * batch prewarm to seed vectors it embedded in bulk). */
static void memory_query_embed_cache_put(const char *command, const char *text, const float *vec,
                                         int dim)
{
   if (!command || !text || !vec || dim <= 0 || dim > EMBED_MAX_DIM)
      return;
   if (strlen(text) >= MEMORY_QEMBED_TEXT_MAX || strlen(command) >= MEMORY_QEMBED_CMD_MAX)
      return;
   for (int i = 0; i < s_qembed_count; i++)
      if (strcmp(s_qembed[i].command, command) == 0 && strcmp(s_qembed[i].text, text) == 0)
         return; /* already cached */
   if (s_qembed_count >= MEMORY_QEMBED_CAP)
      return;
   memory_qembed_entry_t *e = &s_qembed[s_qembed_count++];
   snprintf(e->command, sizeof(e->command), "%s", command);
   snprintf(e->text, sizeof(e->text), "%s", text);
   memcpy(e->vec, vec, (size_t)dim * sizeof(float));
   e->dim = dim;
}

/* Embed-batching: embed up to `n` query texts in ONE batched embedder call and
 * seed them into the per-recall memo, so the retrieval lanes hit the memo
 * instead of making one HTTP round-trip per distinct text. Best-effort: skips
 * builtin (in-process, nothing to batch) and texts already cached / too long;
 * any failure (command error, count mismatch) leaves the memo untouched so the
 * lanes simply embed individually. The embedding command must accept a JSON
 * array of strings on stdin and return a JSON array of float arrays
 * (scripts/embed-remote.py batch mode → embedder /embed_batch). */
static void memory_query_embed_prewarm(const char *const *texts, int n, const char *command)
{
   const char *cmd = memory_effective_embedding_cmd(command);
   if (n <= 0 || !texts || strcmp(cmd, "builtin") == 0)
      return;

   const char *picked[MEMORY_QEMBED_CAP];
   int np = 0;
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return;
   for (int i = 0; i < n && np < MEMORY_QEMBED_CAP; i++)
   {
      const char *t = texts[i];
      if (!t || !t[0] || strlen(t) >= MEMORY_QEMBED_TEXT_MAX)
         continue;
      int dup = 0;
      for (int j = 0; j < s_qembed_count && !dup; j++)
         if (strcmp(s_qembed[j].command, cmd) == 0 && strcmp(s_qembed[j].text, t) == 0)
            dup = 1;
      for (int j = 0; j < np && !dup; j++)
         if (strcmp(picked[j], t) == 0)
            dup = 1;
      if (dup)
         continue;
      picked[np++] = t;
      cJSON_AddItemToArray(arr, cJSON_CreateString(t));
   }
   if (np < 2) /* one text: a single /embed is no slower than a batch of one */
   {
      cJSON_Delete(arr);
      return;
   }

   char *input = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   if (!input)
      return;
   char *buf = NULL;
   size_t buf_len = 0;
   int rc = platform_exec_pipe(cmd, input, strlen(input), &buf, &buf_len);
   free(input);
   if (rc != 0 || !buf || buf_len == 0)
   {
      free(buf);
      return; /* lanes fall back to individual embeds */
   }
   cJSON *resp = cJSON_Parse(buf);
   free(buf);
   if (!cJSON_IsArray(resp) || cJSON_GetArraySize(resp) != np)
   {
      cJSON_Delete(resp);
      return;
   }
   int idx = 0;
   cJSON *vecj;
   cJSON_ArrayForEach(vecj, resp)
   {
      if (cJSON_IsArray(vecj))
      {
         float vec[EMBED_MAX_DIM];
         int dim = 0;
         cJSON *el;
         cJSON_ArrayForEach(el, vecj)
         {
            if (dim >= EMBED_MAX_DIM)
               break;
            if (cJSON_IsNumber(el))
               vec[dim++] = (float)el->valuedouble;
         }
         if (dim > 0)
            memory_query_embed_cache_put(cmd, picked[idx], vec, dim);
      }
      idx++;
   }
   cJSON_Delete(resp);
}

/* Test hooks (declared in memory.h) — expose the static memo to unit tests. */
int memory_query_embed_runtime_test(const char *text, const char *command, float *out, int max_dim)
{
   return memory_embed_text_runtime(text, command, out, max_dim);
}

void memory_query_embed_cache_stats_test(int *requests, int *misses)
{
   if (requests)
      *requests = s_qembed_requests;
   if (misses)
      *misses = s_qembed_misses;
}

void memory_query_embed_cache_reset_test(void)
{
   memory_query_embed_cache_reset();
}

void memory_query_embed_prewarm_test(const char *const *texts, int n, const char *command)
{
   memory_query_embed_prewarm(texts, n, command);
}

static int memory_semantic_query_unavailable(void)
{
   aimee_log(LOG_WARN, "memory", "pgvector semantic query path unavailable");
   return -1;
}

static int memory_vector_ready(void)
{
   return pgvec_memory_vector_collection_exists() > 0;
}

static void memory_embed_unit_row(int64_t unit_id, const char *unit_type, const char *unit_key,
                                  const char *unit_text, double weight, const char *command)
{
   if (unit_id <= 0 || !unit_text || !unit_text[0])
      return;

   char text[2048];
   if (unit_type && strcmp(unit_type, "event") == 0)
      snprintf(text, sizeof(text), "event actor_action %s details %s weight %.2f",
               unit_key ? unit_key : "", unit_text, weight);
   else if (unit_type && strcmp(unit_type, "temporal") == 0)
      snprintf(text, sizeof(text), "time reference %s granularity %s weight %.2f", unit_text,
               unit_key ? unit_key : "", weight);
   else if (unit_type && strcmp(unit_type, "entity") == 0)
      snprintf(text, sizeof(text), "entity role %s value %s weight %.2f", unit_key ? unit_key : "",
               unit_text, weight);
   else if (unit_type && strcmp(unit_type, "summary") == 0)
      snprintf(text, sizeof(text), "summary scope %s text %s weight %.2f", unit_key ? unit_key : "",
               unit_text, weight);
   else if (unit_type && strcmp(unit_type, "chunk") == 0)
      snprintf(text, sizeof(text), "evidence chunk %s text %s weight %.2f",
               unit_key ? unit_key : "", unit_text, weight);
   else
      snprintf(text, sizeof(text), "%s %s %s %.2f", unit_type ? unit_type : "",
               unit_key ? unit_key : "", unit_text, weight);

   float vec[EMBED_MAX_DIM];
   const char *model = (command && command[0]) ? command : "builtin";
   int dim = memory_embed_text(text, model, vec, EMBED_MAX_DIM);
   if (dim <= 0)
      return;

   (void)model;
   memory_sync_unit_vec_row(unit_id, vec, dim);
}

static void memory_refresh_unit_embeddings(int64_t memory_id)
{
   if (memory_id <= 0)
      return;
   config_t cfg;
   config_load(&cfg);
   const char *embed_command = cfg.embedding_command[0] ? cfg.embedding_command : "builtin";

   db2_memory_unit_row_t units[64];
   int unit_max = (int)(sizeof(units) / sizeof(units[0]));
   int n = db2_memory_units_list(memory_id, units, unit_max);
   for (int i = 0; i < n; i++)
   {
      memory_embed_unit_row(units[i].id, units[i].unit_type, units[i].unit_key, units[i].unit_text,
                            units[i].weight, embed_command);
   }
}

static const char *memory_unit_kind_name(memory_unit_kind_t kind)
{
   switch (kind)
   {
   case MEMORY_UNIT_KIND_SEMANTIC:
      return MEMORY_UNIT_KIND_SEMANTIC_STR;
   case MEMORY_UNIT_KIND_PROCEDURAL:
      return MEMORY_UNIT_KIND_PROCEDURAL_STR;
   case MEMORY_UNIT_KIND_EPISODIC:
   default:
      return MEMORY_UNIT_KIND_EPISODIC_STR;
   }
}

static memory_unit_kind_t memory_unit_kind_for_memory(const char *memory_kind,
                                                      const char *unit_type, int has_events,
                                                      int has_temporal)
{
   if (memory_kind &&
       (!strcmp(memory_kind, KIND_PREFERENCE) || !strcmp(memory_kind, KIND_PROCEDURE) ||
        !strcmp(memory_kind, KIND_POLICY) || !strcmp(memory_kind, KIND_WORKFLOW)))
      return MEMORY_UNIT_KIND_PROCEDURAL;
   if (unit_type && (!strcmp(unit_type, "event") || !strcmp(unit_type, "temporal") ||
                     !strcmp(unit_type, "episode")))
      return MEMORY_UNIT_KIND_EPISODIC;
   if (memory_kind && !strcmp(memory_kind, KIND_EPISODE))
      return MEMORY_UNIT_KIND_EPISODIC;
   if (has_events || has_temporal)
      return MEMORY_UNIT_KIND_EPISODIC;
   return MEMORY_UNIT_KIND_SEMANTIC;
}

/* When the cognifier emitted an explicit episodic/semantic/procedural label
 * for the whole memory, that overrides the per-unit inference above. Returns
 * -1 if no explicit kind was emitted so callers fall back to the heuristic. */
static int memory_unit_kind_from_explicit(const char *explicit_kind, memory_unit_kind_t *out)
{
   if (!explicit_kind || !explicit_kind[0] || !out)
      return -1;
   if (strcmp(explicit_kind, MEMORY_UNIT_KIND_EPISODIC_STR) == 0)
   {
      *out = MEMORY_UNIT_KIND_EPISODIC;
      return 0;
   }
   if (strcmp(explicit_kind, MEMORY_UNIT_KIND_SEMANTIC_STR) == 0)
   {
      *out = MEMORY_UNIT_KIND_SEMANTIC;
      return 0;
   }
   if (strcmp(explicit_kind, MEMORY_UNIT_KIND_PROCEDURAL_STR) == 0)
   {
      *out = MEMORY_UNIT_KIND_PROCEDURAL;
      return 0;
   }
   return -1;
}

static memory_unit_kind_t memory_unit_kind_resolve(const char *explicit_kind,
                                                   const char *memory_kind, const char *unit_type,
                                                   int has_events, int has_temporal)
{
   memory_unit_kind_t explicit_out;
   if (memory_unit_kind_from_explicit(explicit_kind, &explicit_out) == 0)
      return explicit_out;
   return memory_unit_kind_for_memory(memory_kind, unit_type, has_events, has_temporal);
}

static double memory_unit_kind_intent_boost(const char *memory_kind, memory_query_intent_t intent)
{
   if (!memory_kind)
      return 0.0;
   if (intent == MEM_QUERY_TEMPORAL)
   {
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_EPISODIC_STR) == 0)
         return 0.18;
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_SEMANTIC_STR) == 0)
         return -0.06;
   }
   else if (intent == MEM_QUERY_ENTITY || intent == MEM_QUERY_GENERAL)
   {
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_SEMANTIC_STR) == 0)
         return 0.12;
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_EPISODIC_STR) == 0)
         return -0.03;
   }
   else if (intent == MEM_QUERY_PROCEDURAL)
   {
      if (strcmp(memory_kind, MEMORY_UNIT_KIND_PROCEDURAL_STR) == 0)
         return 0.22;
      return -0.08;
   }
   return 0.0;
}

static int64_t memory_unit_insert(int64_t memory_id, const char *unit_type,
                                  memory_unit_kind_t memory_kind, const char *unit_key,
                                  const char *unit_text, double weight)
{
   if (memory_id <= 0 || !unit_type || !unit_type[0] || !unit_text || !unit_text[0])
      return 0;

   char norm_key[256];
   char norm_text[512];
   normalize_key(unit_key ? unit_key : "", norm_key, sizeof(norm_key));
   normalize_key(unit_text, norm_text, sizeof(norm_text));
   if (!norm_text[0])
      return 0;

   return db2_memory_unit_insert(memory_id, unit_type, memory_unit_kind_name(memory_kind), norm_key,
                                 norm_text, weight);
}

static void memory_unit_edge_insert(int64_t src_unit_id, int64_t dst_unit_id, const char *edge_type,
                                    double weight)
{
   db2_memory_unit_edge_insert(src_unit_id, dst_unit_id, edge_type, weight);
}

static void memory_refresh_units_graph(int64_t memory_id, const char *key, const char *content)
{
   if (memory_id <= 0)
      return;

   db2_memory_unit_edges_delete_for_memory(memory_id);

   /* Drop the per-unit pgvector points before deleting the rows. */
   {
      int64_t prior_unit_ids[256];
      int prior_n = db2_memory_unit_list_ids(
          memory_id, prior_unit_ids, (int)(sizeof(prior_unit_ids) / sizeof(prior_unit_ids[0])));
      for (int i = 0; i < prior_n; i++)
      {
         int64_t pt = PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET + prior_unit_ids[i];
         pgvec_memory_vector_delete_point(pt);
         db2_vector_index_op_remove(pt);
      }
   }
   db2_memory_units_delete_for_memory(memory_id);

   int64_t summary_units[8];
   int summary_count = 0;
   int64_t event_units[16];
   int event_count = 0;
   int64_t temporal_units[16];
   int temporal_count = 0;
   int64_t entity_units[16];
   int entity_count = 0;
   int64_t chunk_units[16];
   int chunk_count = 0;
   int64_t episode_unit_id = 0;
   char source_session[128];
   char memory_kind[16];
   char explicit_kind[16];
   db2_memory_get_session_kinds(memory_id, source_session, sizeof(source_session), memory_kind,
                                sizeof(memory_kind), explicit_kind, sizeof(explicit_kind));

   {
      db2_memory_summary_row_t summaries[8];
      int n = db2_memory_summaries_list(memory_id, 8, summaries, 8);
      for (int i = 0; i < n && summary_count < 8; i++)
      {
         summary_units[summary_count] = memory_unit_insert(
             memory_id, "summary",
             memory_unit_kind_resolve(explicit_kind, memory_kind, "summary", event_count > 0,
                                      temporal_count > 0),
             summaries[i].scope[0] ? summaries[i].scope : "headline", summaries[i].summary, 3.0);
         if (summary_units[summary_count] > 0)
            summary_count++;
      }
   }
   if (summary_count == 0 && key && key[0])
   {
      summary_units[summary_count] =
          memory_unit_insert(memory_id, "summary",
                             memory_unit_kind_resolve(explicit_kind, memory_kind, "summary",
                                                      event_count > 0, temporal_count > 0),
                             "fallback", key, 2.0);
      if (summary_units[summary_count] > 0)
         summary_count++;
   }

   {
      db2_memory_event_frame_row_t events[16];
      int n = db2_memory_event_frames_list(memory_id, events, 16);
      for (int i = 0; i < n && event_count < 16; i++)
      {
         char unit_key_buf[256];
         char unit_text_buf[512];
         snprintf(unit_key_buf, sizeof(unit_key_buf), "%s %s", events[i].actor, events[i].action);
         snprintf(unit_text_buf, sizeof(unit_text_buf), "%s %s %s %s %s", events[i].actor,
                  events[i].action, events[i].object, events[i].location, events[i].event_time);
         event_units[event_count] = memory_unit_insert(
             memory_id, "event", MEMORY_UNIT_KIND_EPISODIC, unit_key_buf, unit_text_buf, 2.8);
         if (event_units[event_count] > 0)
            event_count++;
      }
   }

   {
      db2_memory_temporal_ref_row_t temporals[16];
      int n = db2_memory_temporal_refs_list(memory_id, temporals, 16);
      for (int i = 0; i < n && temporal_count < 16; i++)
      {
         temporal_units[temporal_count] =
             memory_unit_insert(memory_id, "temporal", MEMORY_UNIT_KIND_EPISODIC,
                                temporals[i].granularity[0] ? temporals[i].granularity : "time",
                                temporals[i].ref_key, 1.6 + temporals[i].weight * 0.4);
         if (temporal_units[temporal_count] > 0)
            temporal_count++;
      }
   }

   {
      db2_memory_entity_row_t entities[16];
      int n = db2_memory_entities_list_weighted(memory_id, entities, 16);
      for (int i = 0; i < n && entity_count < 16; i++)
      {
         entity_units[entity_count] =
             memory_unit_insert(memory_id, "entity",
                                memory_unit_kind_resolve(explicit_kind, memory_kind, "entity",
                                                         event_count > 0, temporal_count > 0),
                                entities[i].role[0] ? entities[i].role : "mention",
                                entities[i].entity, 1.4 + entities[i].weight * 0.3);
         if (entity_units[entity_count] > 0)
            entity_count++;
      }
   }

   {
      db2_memory_chunk_row_t chunks[16];
      int n = db2_memory_chunks_list(memory_id, chunks, 16);
      for (int i = 0; i < n && chunk_count < 16; i++)
      {
         char ck[32];
         snprintf(ck, sizeof(ck), "chunk_%d", chunks[i].chunk_index);
         chunk_units[chunk_count] =
             memory_unit_insert(memory_id, "chunk",
                                memory_unit_kind_resolve(explicit_kind, memory_kind, "chunk",
                                                         event_count > 0, temporal_count > 0),
                                ck, chunks[i].chunk_text, 1.2);
         if (chunk_units[chunk_count] > 0)
            chunk_count++;
      }
   }

   if (content && content[0])
      episode_unit_id = memory_unit_insert(memory_id, "episode", MEMORY_UNIT_KIND_EPISODIC,
                                           key ? key : "episode", content, 1.8);

   for (int i = 0; i < summary_count; i++)
      for (int j = 0; j < event_count; j++)
         memory_unit_edge_insert(summary_units[i], event_units[j], "contains", 1.0);
   for (int i = 0; i < summary_count; i++)
      for (int j = 0; j < chunk_count; j++)
         memory_unit_edge_insert(summary_units[i], chunk_units[j], "contains", 0.8);
   for (int i = 0; i < event_count; i++)
   {
      for (int j = 0; j < temporal_count; j++)
         memory_unit_edge_insert(event_units[i], temporal_units[j], "anchored_by", 1.1);
      for (int j = 0; j < entity_count; j++)
         memory_unit_edge_insert(event_units[i], entity_units[j], "mentions", 1.0);
      for (int j = 0; j < chunk_count; j++)
         memory_unit_edge_insert(chunk_units[j], event_units[i], "supports", 0.7);
   }
   if (episode_unit_id > 0)
   {
      for (int i = 0; i < summary_count; i++)
         memory_unit_edge_insert(episode_unit_id, summary_units[i], "same_episode", 1.0);
      for (int i = 0; i < event_count; i++)
         memory_unit_edge_insert(episode_unit_id, event_units[i], "same_episode", 1.0);
      for (int i = 0; i < chunk_count; i++)
         memory_unit_edge_insert(episode_unit_id, chunk_units[i], "same_episode", 0.8);
   }

   if (episode_unit_id > 0 && source_session[0])
   {
      int64_t other_eps[64];
      int n = db2_memory_episode_unit_ids_for_session(memory_id, source_session, other_eps, 64);
      for (int i = 0; i < n; i++)
      {
         memory_unit_edge_insert(episode_unit_id, other_eps[i], "same_episode", 0.9);
         memory_unit_edge_insert(other_eps[i], episode_unit_id, "same_episode", 0.9);
      }
   }

   if (episode_unit_id > 0)
   {
      int64_t old_eps[64];
      int n = db2_memory_episode_unit_ids_supersedes(memory_id, old_eps, 64);
      for (int i = 0; i < n; i++)
         memory_unit_edge_insert(episode_unit_id, old_eps[i], "supersedes", 1.2);
   }
}

static int64_t memory_episode_insert(int64_t memory_id, const char *episode_key,
                                     const char *episode_text, const char *source_session,
                                     const char *reference_time)
{
   return db2_memory_episode_insert(memory_id, episode_key, episode_text, source_session,
                                    reference_time);
}

static void memory_relation_insert(int64_t memory_id, int64_t episode_id, const char *src_entity,
                                   const char *relation, const char *dst_entity,
                                   const char *fact_text, const char *valid_at,
                                   const char *invalid_at, double weight)
{
   db2_memory_relation_upsert_full(memory_id, episode_id, src_entity, relation, dst_entity,
                                   fact_text, valid_at, invalid_at, weight);
}

static void memory_lookup_primary_entity(int64_t memory_id, char *buf, size_t buf_len)
{
   db2_memory_lookup_primary_entity(memory_id, buf, (int)buf_len);
}

static void memory_lookup_time_bounds(int64_t memory_id, char *valid_at, size_t valid_len,
                                      char *invalid_at, size_t invalid_len)
{
   db2_memory_lookup_time_bounds(memory_id, valid_at, (int)valid_len, invalid_at, (int)invalid_len);
}

static void memory_refresh_episode_relations(int64_t memory_id, const char *key,
                                             const char *content)
{
   if (memory_id <= 0)
      return;

   db2_memory_episodes_delete_for_memory(memory_id);
   db2_memory_relations_delete_for_memory(memory_id);

   char source_session[128];
   char valid_at[64];
   char invalid_at[64];
   source_session[0] = '\0';
   memory_lookup_time_bounds(memory_id, valid_at, sizeof(valid_at), invalid_at, sizeof(invalid_at));
   db2_memory_get_source_session(memory_id, source_session, (int)sizeof(source_session));

   int64_t episode_id = memory_episode_insert(memory_id, key ? key : "", content ? content : "",
                                              source_session, valid_at);

   db2_memory_summary_row_t summaries[2];
   int summary_n = db2_memory_summaries_list(memory_id, 2, summaries, 2);
   for (int i = 0; i < summary_n; i++)
   {
      if (summaries[i].summary[0])
         memory_episode_insert(memory_id,
                               summaries[i].scope[0] ? summaries[i].scope : (key ? key : ""),
                               summaries[i].summary, source_session, valid_at);
   }

   char primary_entity[128];
   memory_lookup_primary_entity(memory_id, primary_entity, sizeof(primary_entity));

   db2_memory_event_frame_row_t events[64];
   int event_n =
       db2_memory_event_frames_list(memory_id, events, (int)(sizeof(events) / sizeof(events[0])));
   for (int i = 0; i < event_n; i++)
   {
      const char *actor = events[i].actor;
      const char *action = events[i].action;
      const char *object = events[i].object;
      const char *location = events[i].location;
      const char *event_time = events[i].event_time;
      char fact[1024];
      snprintf(fact, sizeof(fact), "%s %s %s %s %s", actor, action, object, location, event_time);
      if (actor[0] && action[0] && object[0])
         memory_relation_insert(memory_id, episode_id, actor, action, object, fact,
                                event_time[0] ? event_time : valid_at, invalid_at, 2.4);
      if (actor[0] && location[0])
         memory_relation_insert(memory_id, episode_id, actor, "located_at", location, fact,
                                event_time[0] ? event_time : valid_at, invalid_at, 1.8);
      if (actor[0] && event_time[0])
         memory_relation_insert(memory_id, episode_id, actor, "occurred_at", event_time, fact,
                                event_time, invalid_at, 1.9);
   }

   db2_memory_link_target_row_t links[64];
   int link_n =
       db2_memory_links_with_targets(memory_id, links, (int)(sizeof(links) / sizeof(links[0])));
   for (int i = 0; i < link_n; i++)
   {
      const char *relation = links[i].relation;
      const char *target_key = links[i].target_key;
      const char *target_content = links[i].target_content;
      char fact[1024];
      snprintf(fact, sizeof(fact), "%s %s %s",
               primary_entity[0] ? primary_entity : (key ? key : ""), relation,
               target_content[0] ? target_content : target_key);
      memory_relation_insert(
          memory_id, episode_id, primary_entity[0] ? primary_entity : (key ? key : ""),
          relation[0] ? relation : "related_to", target_key[0] ? target_key : target_content, fact,
          valid_at, invalid_at, 1.3);
   }
}

static void memory_summary_insert(int64_t memory_id, const char *scope, const char *summary)
{
   db2_memory_summary_upsert(memory_id, scope, summary);
}

static int memory_is_summary_noise_token(const char *tok)
{
   if (!tok || !tok[0])
      return 1;
   return strcmp(tok, "user") == 0 || strcmp(tok, "assistant") == 0 || strcmp(tok, "system") == 0 ||
          strcmp(tok, "speaker") == 0;
}

static void memory_chunk_insert(int64_t memory_id, int chunk_index, const char *chunk)
{
   db2_memory_chunk_upsert(memory_id, chunk_index, chunk);
}

static void memory_refresh_summaries(int64_t memory_id, const char *key, const char *content)
{
   db2_memory_summaries_delete_for_memory(memory_id);

   char norm[2048];
   char tokens[32][64];
   char summary[512];
   normalize_key(content && content[0] ? content : key, norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 32);
   if (token_count <= 0)
      return;

   int take = token_count < 20 ? token_count : 20;
   memory_alias_join_tokens(summary, sizeof(summary), tokens, 0, take);
   if (summary[0])
      memory_summary_insert(memory_id, "headline", summary);

   char concepts[512];
   char seen[24][64];
   int seen_count = 0;
   size_t used = 0;
   concepts[0] = '\0';
   int scan = token_count < 96 ? token_count : 96;
   for (int i = 0; i < scan && seen_count < 24; i++)
   {
      if (!memory_is_signal_token(tokens[i]) || memory_is_summary_noise_token(tokens[i]))
         continue;
      char canonical[64];
      memory_canonicalize_term(tokens[i], canonical, sizeof(canonical));
      const char *term = canonical[0] ? canonical : tokens[i];
      if (strlen(term) < 4)
         continue;
      int dup = 0;
      for (int j = 0; j < seen_count; j++)
      {
         if (strcmp(seen[j], term) == 0)
         {
            dup = 1;
            break;
         }
      }
      if (dup)
         continue;
      snprintf(seen[seen_count], sizeof(seen[seen_count]), "%s", term);
      seen_count++;
      int wrote = snprintf(concepts + used, sizeof(concepts) - used, "%s%s", used ? " " : "", term);
      if (wrote < 0 || (size_t)wrote >= sizeof(concepts) - used)
      {
         concepts[sizeof(concepts) - 1] = '\0';
         break;
      }
      used += (size_t)wrote;
   }
   if (concepts[0])
      memory_summary_insert(memory_id, "signals", concepts);
}

static void memory_event_frame_insert(int64_t memory_id, const char *actor, const char *action,
                                      const char *object, const char *location,
                                      const char *event_time, const char *evidence_kind)
{
   db2_memory_event_frame_insert(memory_id, actor, action, object, location, event_time,
                                 evidence_kind);
}

static int memory_extract_event_frame_fields(const char *text, const char *fallback_key,
                                             char *actor, size_t actor_len, char *action,
                                             size_t action_len, char *object, size_t object_len,
                                             char *location, size_t location_len, char *event_time,
                                             size_t event_time_len)
{
   char norm[2048];
   char tokens[32][64];
   int action_idx = -1;
   if (actor && actor_len)
      actor[0] = '\0';
   if (action && action_len)
      action[0] = '\0';
   if (object && object_len)
      object[0] = '\0';
   if (location && location_len)
      location[0] = '\0';
   if (event_time && event_time_len)
      event_time[0] = '\0';

   normalize_key(text && text[0] ? text : fallback_key, norm, sizeof(norm));
   int token_count = memory_split_tokens(norm, tokens, 32);
   if (token_count <= 0)
      return 0;

   for (int i = 0; i < token_count; i++)
   {
      if (action && !action[0] && memory_is_likely_action_token(tokens[i]))
      {
         snprintf(action, action_len, "%s", tokens[i]);
         action_idx = i;
      }

      if (location && !location[0] && memory_is_probable_location_token(tokens[i]))
         snprintf(location, location_len, "%s", tokens[i]);
      if (location && !location[0] && memory_is_relation_token(tokens[i]) && i + 1 < token_count)
      {
         int take = (i + 3 <= token_count && !memory_is_relation_token(tokens[i + 2])) ? 2 : 1;
         memory_alias_join_tokens(location, location_len, tokens, i + 1, take);
      }

      if (event_time && !event_time[0])
      {
         if (memory_is_month_token(tokens[i]) && i + 1 < token_count &&
             strlen(tokens[i + 1]) <= 4 && isdigit((unsigned char)tokens[i + 1][0]))
            memory_alias_join_tokens(event_time, event_time_len, tokens, i, 2);
         else if (memory_is_month_token(tokens[i]) || memory_is_weekday_token(tokens[i]) ||
                  strcmp(tokens[i], "today") == 0 || strcmp(tokens[i], "yesterday") == 0 ||
                  strcmp(tokens[i], "tomorrow") == 0 ||
                  (strlen(tokens[i]) == 4 && isdigit((unsigned char)tokens[i][0])))
            snprintf(event_time, event_time_len, "%s", tokens[i]);
      }
   }

   if (action_idx > 0)
   {
      for (int i = action_idx - 1; i >= 0; i--)
      {
         if (memory_is_stopword_token(tokens[i]) || memory_is_month_token(tokens[i]) ||
             memory_is_weekday_token(tokens[i]) || memory_is_relation_token(tokens[i]))
            continue;
         if (actor && !actor[0])
         {
            if (i > 0 && strlen(tokens[i - 1]) >= 3 && !memory_is_stopword_token(tokens[i - 1]) &&
                !memory_is_relation_token(tokens[i - 1]))
               memory_alias_join_tokens(actor, actor_len, tokens, i - 1, 2);
            else
               snprintf(actor, actor_len, "%s", tokens[i]);
         }
         break;
      }
      if (object && action_idx + 1 < token_count)
      {
         int start = action_idx + 1;
         while (start < token_count && (memory_is_stopword_token(tokens[start]) ||
                                        memory_is_relation_token(tokens[start])))
            start++;
         if (start < token_count && !memory_is_month_token(tokens[start]) &&
             !memory_is_weekday_token(tokens[start]))
         {
            int take = 1;
            if (start + 1 < token_count && !memory_is_stopword_token(tokens[start + 1]) &&
                !memory_is_relation_token(tokens[start + 1]) &&
                !memory_is_month_token(tokens[start + 1]))
               take = 2;
            memory_alias_join_tokens(object, object_len, tokens, start, take);
         }
      }
   }

   if (actor && !actor[0] && fallback_key && fallback_key[0])
      snprintf(actor, actor_len, "%s", fallback_key);
   if (action && !action[0])
      snprintf(action, action_len, "%s", "state");
   if (object && !object[0] && fallback_key && fallback_key[0])
      snprintf(object, object_len, "%s", fallback_key);
   return 1;
}

static void memory_refresh_event_frames(int64_t memory_id, const char *key, const char *content)
{
   db2_memory_event_frames_delete_for_memory(memory_id);

   if (!content || !content[0])
   {
      char actor[128], action[128], object[256], location[128], event_time[128];
      if (memory_extract_event_frame_fields(key, key, actor, sizeof(actor), action, sizeof(action),
                                            object, sizeof(object), location, sizeof(location),
                                            event_time, sizeof(event_time)))
         memory_event_frame_insert(memory_id, actor, action, object, location, event_time,
                                   "derived");
      return;
   }

   char chunk[512];
   int frame_count = 0;
   size_t used = 0;
   for (size_t i = 0;; i++)
   {
      char ch = content[i];
      if (ch == '\0' || ch == '.' || ch == '!' || ch == '?' || ch == ';' || ch == '\n')
      {
         while (used > 0 && isspace((unsigned char)chunk[used - 1]))
            used--;
         chunk[used] = '\0';
         if (used > 0)
         {
            char actor[128], action[128], object[256], location[128], event_time[128];
            if (memory_extract_event_frame_fields(chunk, key, actor, sizeof(actor), action,
                                                  sizeof(action), object, sizeof(object), location,
                                                  sizeof(location), event_time, sizeof(event_time)))
            {
               memory_event_frame_insert(memory_id, actor, action, object, location, event_time,
                                         frame_count == 0 ? "derived" : "chunk");
               frame_count++;
            }
         }
         used = 0;
         if (ch == '\0' || frame_count >= 8)
            break;
         continue;
      }
      if (used + 1 < sizeof(chunk))
         chunk[used++] = ch;
   }
}

/* Token-window chunker: splits content into ~50-token windows with 10-token
 * overlap.  Punctuation (. ! ? ; \n) is used as a preferred break point but
 * is not required.  Falls back to a hard split at the window boundary when
 * there is no sentence break within the window.  Hard limit: 8 chunks. */
static void memory_refresh_chunks(int64_t memory_id, const char *content)
{
   db2_memory_chunks_delete_for_memory(memory_id);

   if (!content || !content[0])
      return;

   enum
   {
      CHUNK_TOKENS = 50,
      OVERLAP_TOKENS = 10,
      MAX_CHUNKS = 8
   };
   /* Rough token estimate: 4 chars per token */
   enum
   {
      CHUNK_CHARS = CHUNK_TOKENS * 4,
      OVERLAP_CHARS = OVERLAP_TOKENS * 4
   };
   char chunk[512];
   int ci = 0;
   size_t clen = strlen(content);
   size_t pos = 0; /* current read position in content */

   while (pos < clen && ci < MAX_CHUNKS)
   {
      /* Fill up to CHUNK_CHARS characters; prefer a sentence break */
      size_t end = pos + CHUNK_CHARS;
      if (end >= clen)
         end = clen;

      /* Look for a preferred break (punctuation) within [pos, end] */
      size_t break_at = end;
      if (end < clen)
      {
         /* Scan backwards from end for punctuation */
         for (size_t k = end; k > pos; k--)
         {
            char c = content[k - 1];
            if (c == '.' || c == '!' || c == '?' || c == ';' || c == '\n')
            {
               break_at = k;
               break;
            }
         }
      }

      /* Extract chunk text */
      size_t chunk_len = break_at - pos;
      if (chunk_len == 0)
         break;
      if (chunk_len > sizeof(chunk) - 1)
         chunk_len = sizeof(chunk) - 1;

      memcpy(chunk, content + pos, chunk_len);
      /* Trim trailing whitespace */
      while (chunk_len > 0 && isspace((unsigned char)chunk[chunk_len - 1]))
         chunk_len--;
      chunk[chunk_len] = '\0';

      if (chunk_len > 0)
         memory_chunk_insert(memory_id, ci++, chunk);

      /* Advance, keeping overlap */
      if (break_at >= clen)
         break;
      pos = break_at > OVERLAP_CHARS ? break_at - OVERLAP_CHARS : 0;
      /* Ensure forward progress */
      if (pos <= (break_at > CHUNK_CHARS ? break_at - CHUNK_CHARS : 0))
         pos = break_at;
   }

   if (ci == 0)
      memory_chunk_insert(memory_id, 0, content);
}

static void memory_refresh_aliases(int64_t memory_id, const char *key, const char *content)
{
   if (memory_id <= 0)
      return;
   db2_memory_aliases_delete_for_memory(memory_id);

   if (key && key[0])
      memory_alias_insert(memory_id, key, 4.0);

   if (!content || !content[0])
      return;

   char norm_content[2048];
   char tokens[16][64];
   normalize_key(content, norm_content, sizeof(norm_content));
   int token_count = memory_split_tokens(norm_content, tokens, 16);
   if (token_count <= 0)
      return;

   char alias[256];
   int emitted = 0;

   for (int span = token_count >= 6 ? 6 : token_count; span >= 3 && emitted < 24; span--)
   {
      memory_alias_join_tokens(alias, sizeof(alias), tokens, 0, span);
      if (alias[0])
      {
         memory_alias_insert(memory_id, alias, 3.0 + (double)span * 0.1);
         emitted++;
      }
   }

   for (int i = 0; i < token_count - 1 && emitted < 24; i++)
   {
      if (!memory_alias_is_useful_token(tokens[i]) || !memory_alias_is_useful_token(tokens[i + 1]))
         continue;
      memory_alias_join_tokens(alias, sizeof(alias), tokens, i, 2);
      if (alias[0])
      {
         memory_alias_insert(memory_id, alias, 2.0);
         emitted++;
      }
   }

   for (int i = 0; i < token_count - 2 && emitted < 24; i++)
   {
      if (!memory_alias_is_useful_token(tokens[i]) ||
          !memory_alias_is_useful_token(tokens[i + 1]) ||
          !memory_alias_is_useful_token(tokens[i + 2]))
         continue;
      memory_alias_join_tokens(alias, sizeof(alias), tokens, i, 3);
      if (alias[0])
      {
         memory_alias_insert(memory_id, alias, 2.5);
         emitted++;
      }
   }

   for (int i = 0; i < token_count && emitted < 24; i++)
   {
      if (!memory_alias_is_useful_token(tokens[i]))
         continue;
      memory_alias_insert(memory_id, tokens[i], 1.0);
      emitted++;
   }
}
