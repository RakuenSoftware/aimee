/* --- Search --- */

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

int memory_search(char **clusters, int cluster_count, int limit, search_result_t *out, int max)
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

static void memory_expand_synonyms(const char *query, char *out, size_t out_len)
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

static int memory_find_facts_like(const char *query, int limit, memory_t *out, int max)
{
   return db2_memory_find_facts_like(query, limit, out, max);
}

static int memory_append_unique(memory_t *out, int count, int max, const memory_t *candidate)
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
   memory_t scratch[64];
   int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
   int got = collect(term, limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

static int memory_collect_alias_matches(const char *alias, int limit, memory_t *out, int count,
                                        int max)
{
   return memory_collect_via(db2_memory_collect_alias_matches, alias, limit, out, count, max);
}

static int memory_collect_entity_matches(const char *term, int limit, memory_t *out, int count,
                                         int max)
{
   return memory_collect_via(db2_memory_collect_entity_matches, term, limit, out, count, max);
}

static int memory_collect_temporal_matches(const char *term, int limit, memory_t *out, int count,
                                           int max)
{
   return memory_collect_via(db2_memory_collect_temporal_matches, term, limit, out, count, max);
}

static int memory_collect_summary_matches(const char *term, int limit, memory_t *out, int count,
                                          int max)
{
   return memory_collect_via(db2_memory_collect_summary_matches, term, limit, out, count, max);
}

static int memory_collect_event_frame_matches(const char *term, int limit, memory_t *out, int count,
                                              int max)
{
   return memory_collect_via(db2_memory_collect_event_frame_matches, term, limit, out, count, max);
}

/* Forward declarations for the pgvector-routing helpers defined later in
 * the file (next to the other vector-search helpers). */
static int memory_collect_memory_matches_via_vector(const char *query, const char *embed_cmd,
                                                    int limit, memory_t *out, int max);
static int memory_collect_unit_matches_via_vector(const char *query, const char *embed_cmd,
                                                  int limit, memory_t *out, int max);

/* Chunks are not separately indexed in pgvector yet, so chunk_matches falls
 * back to memory-level semantic search via memory_collect_memory_matches_via_vector —
 * the caller still tags the hits with MEM_SOURCE_CHUNK. Future work: upsert
 * chunk embeddings to pgvector under a record_type='chunk' key for true
 * chunk-level retrieval. */
static int memory_collect_chunk_matches(const char *query, int limit, memory_t *out, int count,
                                        int max)
{
   if (!query || !query[0] || !out || count >= max)
      return count;
   memory_t scratch[64];
   int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
   config_t cm_cfg;
   config_load(&cm_cfg);
   const char *embed_cmd = cm_cfg.embedding_command[0] ? cm_cfg.embedding_command : "builtin";
   int got = memory_collect_memory_matches_via_vector(query, embed_cmd, limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

static int memory_collect_unit_matches(const char *query, int limit, memory_t *out, int count,
                                       int max)
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
   memory_t scratch[64];
   int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
   config_t mc_cfg;
   config_load(&mc_cfg);
   const char *embed_cmd = mc_cfg.embedding_command[0] ? mc_cfg.embedding_command : "builtin";
   int got = memory_collect_unit_matches_via_vector(query, embed_cmd, limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

static void memory_build_semantic_query_text(const char *raw_query, memory_query_intent_t intent,
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

/* Visitor state for the ephemeral-mode in-memory cosine scan. The visitor
 * keeps the top hits whose sim >= 0.62, deduplicating by memory_id and
 * resolving the memory row inline. The fixed cap matches the original local
 * array size; once we run out of slots we drop further matches. */
static int memory_collect_semantic_matches(const char *query, const char *command, int limit,
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

   if (qdim == 384)
   {
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
            if (sim < 0.62)
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

static int memory_collect_unit_semantic_matches(const char *query, const char *command, int limit,
                                                memory_t *out, int count, int max,
                                                int64_t *semantic_ids, double *semantic_scores,
                                                int *semantic_hit_count, int semantic_max)
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

   if (qdim == 384)
   {
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

static double memory_semantic_bonus_lookup(int64_t memory_id, const int64_t *semantic_ids,
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

static memory_query_shape_t memory_query_shape(const char *raw_query, const char *norm_query)
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

static memory_query_route_t memory_query_route(const char *raw_query, const char *norm_query,
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

static void memory_record_query_plan_metrics(const memory_query_plan_t *plan, double elapsed_ms)
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

static double memory_query_elapsed_ms(const struct timespec *start, const struct timespec *end)
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

static memory_query_intent_t memory_query_intent(const char *raw_query, const char *norm_query)
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

static int memory_query_has_term(const char *norm_query, const char *term)
{
   return norm_query && term && term[0] && memory_token_in_norm(norm_query, term);
}

static int memory_query_wants_recent(const char *norm_query)
{
   static const char *terms[] = {"latest", "recent", "newest", "current", "today", "now", NULL};
   for (int i = 0; terms[i]; i++)
      if (memory_query_has_term(norm_query, terms[i]))
         return 1;
   return 0;
}

static int memory_query_wants_past(const char *norm_query)
{
   static const char *terms[] = {"before", "earlier", "earliest", "previous", "older", NULL};
   for (int i = 0; terms[i]; i++)
      if (memory_query_has_term(norm_query, terms[i]))
         return 1;
   return 0;
}

static int memory_query_wants_future(const char *norm_query)
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

static int memory_parse_temporal_ref_date(const char *ref_key, const char *granularity,
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

static int memory_parse_temporal_constraint(const char *norm_query, char qtokens[][64], int nq,
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

static double memory_temporal_constraint_score(const memory_temporal_constraint_t *constraint,
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

static int memory_temporal_value_conflict(const char *a, const char *b)
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

static int memory_temporal_query_has_explicit_date(char qtokens[][64], int nq)
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

static int memory_temporal_ref_matches_query(const char *ref_key, char qtokens[][64], int nq)
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

static int memory_add_expanded_term(char terms[][64], int count, int max_terms, const char *term)
{
   if (!term || !term[0] || count >= max_terms)
      return count;
   for (int i = 0; i < count; i++)
      if (strcmp(terms[i], term) == 0)
         return count;
   snprintf(terms[count], 64, "%s", term);
   return count + 1;
}

static int memory_expand_query_terms(const char *norm_query, char base_tokens[][64], int base_count,
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

static double memory_entity_bonus(int64_t memory_id, char qtokens[][64], int nq)
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
static double memory_speaker_bonus(int64_t memory_id, const char *raw_query)
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

static double memory_temporal_bonus(int64_t memory_id, memory_query_intent_t intent,
                                    char qtokens[][64], int nq)
{
   db2_memory_temporal_ref_row_t rows[64];
   int nrows =
       db2_memory_temporal_refs_list(memory_id, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   if (nrows <= 0)
      return 0.0;

   double bonus = 0.0;
   char norm_query[512];
   size_t used = 0;
   norm_query[0] = '\0';
   for (int i = 0; i < nq && used + 65 < sizeof(norm_query); i++)
      used += (size_t)snprintf(norm_query + used, sizeof(norm_query) - used, "%s%s",
                               used ? " " : "", qtokens[i]);
   int explicit_date = memory_temporal_query_has_explicit_date(qtokens, nq);
   int wants_recent = memory_query_wants_recent(norm_query);
   int wants_past = memory_query_wants_past(norm_query);
   int wants_future = memory_query_wants_future(norm_query);
   memory_temporal_constraint_t constraint;
   int has_constraint = memory_parse_temporal_constraint(norm_query, qtokens, nq, &constraint);
   for (int r = 0; r < nrows; r++)
   {
      const char *ref_key = rows[r].ref_key;
      const char *granularity = rows[r].granularity;
      double weight = rows[r].weight;
      int matched_ref = 0;
      if (!ref_key[0])
         continue;

      if (intent == MEM_QUERY_TEMPORAL)
         bonus += 0.42 + weight * 0.18;

      if (memory_temporal_ref_matches_query(ref_key, qtokens, nq))
      {
         bonus += 0.34 + weight * 0.12;
         matched_ref = 1;
      }

      if (!matched_ref && granularity[0] && strcmp(granularity, "date_phrase") == 0 &&
          intent == MEM_QUERY_TEMPORAL)
         bonus += 0.15;
      if (intent == MEM_QUERY_TEMPORAL && granularity[0] &&
          (strcmp(granularity, "ordering") == 0 || strcmp(granularity, "range") == 0))
         bonus += 0.25;
      if (explicit_date && granularity[0] &&
          (strcmp(granularity, "absolute_day") == 0 || strcmp(granularity, "date_phrase") == 0))
         bonus += 0.18;
      if (wants_recent && granularity[0] &&
          (strcmp(granularity, "absolute_day") == 0 || strcmp(granularity, "date_phrase") == 0))
         bonus += 0.08 + weight * 0.03;
      if ((wants_past || wants_future) && granularity[0] && strcmp(granularity, "ordering") == 0)
         bonus += 0.14;
      if (has_constraint)
      {
         memory_parsed_date_t candidate_date;
         if (memory_parse_temporal_ref_date(ref_key, granularity, &candidate_date))
            bonus += memory_temporal_constraint_score(&constraint, &candidate_date);
      }
   }
   return bonus;
}

static double memory_evidence_bonus(int64_t memory_id)
{
   double evidence = 0.0;
   int observations = 0;
   if (!db2_memory_get_evidence_fields(memory_id, &evidence, &observations))
      return 0.0;
   double bonus = evidence * 0.8;
   if (observations > 1)
      bonus += (double)(observations - 1) * 0.05;
   return bonus;
}

static double memory_salience_bonus(int64_t memory_id)
{
   if (!memory_salience_enabled())
      return 0.0;
   double salience = db2_memory_get_salience(memory_id, 0.5);
   return (salience - 0.5) * memory_salience_weight();
}

static double memory_surprise_bonus(int64_t memory_id)
{
   if (!memory_surprise_enabled())
      return 0.0;
   double surprise = db2_memory_get_surprise(memory_id, 0.5);
   return (surprise - 0.5) * memory_surprise_weight();
}

static double memory_unit_graph_bonus(int64_t memory_id, char qtokens[][64], int nq)
{
   db2_memory_unit_row_t units[64];
   int nunits = db2_memory_units_list(memory_id, units, (int)(sizeof(units) / sizeof(units[0])));
   if (nunits <= 0)
      return 0.0;

   int64_t matched_ids[16];
   int matched_count = 0;
   int saw_summary = 0, saw_event = 0, saw_temporal = 0, saw_entity = 0, saw_chunk = 0;
   double bonus = 0.0;
   int signal_count = 0;
   for (int u = 0; u < nunits; u++)
   {
      int64_t unit_id = units[u].id;
      const char *unit_type = units[u].unit_type;
      const char *unit_key = units[u].unit_key;
      const char *unit_text = units[u].unit_text;
      double weight = units[u].weight;
      int matched_tokens = 0;
      for (int i = 0; i < nq; i++)
      {
         if (!memory_is_signal_token(qtokens[i]))
            continue;
         char canonical_q[64];
         memory_canonicalize_term(qtokens[i], canonical_q, sizeof(canonical_q));
         if ((canonical_q[0] && ((unit_key[0] && memory_token_in_norm(unit_key, canonical_q)) ||
                                 (unit_text[0] && memory_token_in_norm(unit_text, canonical_q)))) ||
             (unit_key[0] && memory_token_in_norm(unit_key, qtokens[i])) ||
             (unit_text[0] && memory_token_in_norm(unit_text, qtokens[i])))
            matched_tokens++;
      }
      if (matched_tokens <= 0)
         continue;
      int allow_single =
          unit_type[0] && (strcmp(unit_type, "entity") == 0 || strcmp(unit_type, "temporal") == 0 ||
                           strcmp(unit_type, "event") == 0);
      if (matched_tokens < 2 && !allow_single)
         continue;
      signal_count++;
      bonus += 0.08 + weight * 0.04 + (double)(matched_tokens > 2 ? 2 : matched_tokens) * 0.03;
      if (matched_count < 16)
         matched_ids[matched_count++] = unit_id;
      if (unit_type[0])
      {
         if (strcmp(unit_type, "summary") == 0)
            saw_summary = 1;
         else if (strcmp(unit_type, "event") == 0)
            saw_event = 1;
         else if (strcmp(unit_type, "temporal") == 0)
            saw_temporal = 1;
         else if (strcmp(unit_type, "entity") == 0)
            saw_entity = 1;
         else if (strcmp(unit_type, "chunk") == 0)
            saw_chunk = 1;
      }
   }

   if (signal_count == 0)
      return 0.0;

   if ((saw_event && saw_temporal) || (saw_event && saw_entity))
      bonus += 0.14;
   if (saw_summary && (saw_event || saw_chunk))
      bonus += 0.08;

   for (int i = 0; i < matched_count; i++)
   {
      for (int j = i + 1; j < matched_count; j++)
      {
         if (db2_memory_unit_edge_exists(matched_ids[i], matched_ids[j]))
            bonus += 0.04;
      }
   }
   if (bonus > 0.85)
      bonus = 0.85;
   return bonus;
}

static double memory_state_penalty(int64_t memory_id)
{
   int has_valid_until = 0;
   int observations = 0;
   int use_count = 0;
   if (!db2_memory_get_state_fields(memory_id, &has_valid_until, &observations, &use_count))
      return 0.0;
   double penalty = 0.0;
   if (has_valid_until)
      penalty -= 1.8;
   if (observations <= 1)
      penalty -= 0.15;
   if (use_count == 0)
      penalty -= 0.05;
   return penalty;
}

static memory_ranker_input_t memory_ranker_input_from(const memory_t *m)
{
   memory_ranker_input_t r;
   memset(&r, 0, sizeof(r));
   if (!m)
      return r;
   r.id = m->id;
   snprintf(r.kind, sizeof(r.kind), "%s", m->kind);
   snprintf(r.key, sizeof(r.key), "%s", m->key);
   snprintf(r.content, sizeof(r.content), "%s", m->content);
   snprintf(r.use_cases, sizeof(r.use_cases), "%s", m->use_cases);
   return r;
}

static void memory_compute_score_parts(const char *raw_query, const char *norm_query,
                                       const memory_ranker_input_t *m, const int64_t *semantic_ids,
                                       const double *semantic_scores, int semantic_hit_count,
                                       const memory_pagerank_score_t *pagerank_scores,
                                       int pagerank_count, memory_score_parts_t *parts)
{
   if (!parts)
      return;
   memset(parts, 0, sizeof(*parts));

   char qtokens[32][64];
   char norm_key[sizeof(m->key)];
   char norm_content[sizeof(m->content)];
   char norm_use_cases[sizeof(m->use_cases)];
   int nq = memory_split_tokens(norm_query, qtokens, 32);
   if (nq <= 0)
      return;

   memory_rank_weights_t w = memory_rank_weights();
   memory_query_intent_t intent = memory_query_intent(raw_query, norm_query);
   normalize_key(m->key, norm_key, sizeof(norm_key));
   normalize_key(m->content, norm_content, sizeof(norm_content));
   normalize_key(m->use_cases, norm_use_cases, sizeof(norm_use_cases));

   int matched = 0;
   for (int i = 0; i < nq; i++)
   {
      int in_key = memory_token_in_norm(norm_key, qtokens[i]);
      int in_content = memory_token_in_norm(norm_content, qtokens[i]);
      int in_use_cases = memory_token_in_norm(norm_use_cases, qtokens[i]);
      if (in_key || in_content || in_use_cases)
      {
         double v = in_key ? w.lexical_key : w.lexical_content;
         size_t len = strlen(qtokens[i]);
         if (len >= 6)
            v += w.lexical_long_token_bonus;
         parts->lexical += v;
         matched++;
      }
   }
   parts->entity = memory_entity_bonus(m->id, qtokens, nq) * w.entity +
                   memory_speaker_bonus(m->id, raw_query) * w.speaker;
   parts->temporal = memory_temporal_bonus(m->id, intent, qtokens, nq) * w.temporal;
   parts->evidence =
       (memory_evidence_bonus(m->id) + memory_unit_graph_bonus(m->id, qtokens, nq)) * w.evidence;
   parts->semantic =
       memory_semantic_bonus_lookup(m->id, semantic_ids, semantic_scores, semantic_hit_count) *
       w.semantic;
   parts->state = memory_state_penalty(m->id) * w.state;
   parts->coverage = ((double)matched / (double)nq) * w.coverage;
   parts->salience = memory_salience_bonus(m->id) * w.salience;
   parts->surprise = memory_surprise_bonus(m->id) * w.surprise;
   parts->pagerank = memory_pagerank_bonus_lookup(pagerank_scores, pagerank_count, m->id);
   parts->intent = 0.0;
   if (intent == MEM_QUERY_PROCEDURAL && strcmp(m->kind, KIND_WORKFLOW) == 0)
      parts->intent += w.workflow_intent;
   else if (intent == MEM_QUERY_ENTITY &&
            (strcmp(m->kind, KIND_FACT) == 0 || strcmp(m->kind, KIND_PREFERENCE) == 0))
      parts->intent += w.entity_intent;
   else if (intent == MEM_QUERY_TEMPORAL &&
            (strcmp(m->kind, KIND_FACT) == 0 || strcmp(m->kind, KIND_EPISODE) == 0))
      parts->intent += w.temporal_intent;

   if (matched == 0)
   {
      double support = parts->semantic + parts->entity + parts->temporal + parts->evidence;
      double min_support = memory_env_weight("AIMEE_MEMORY_NO_LEXICAL_MIN_SUPPORT", 0.85);
      double no_lexical_penalty = memory_env_weight("AIMEE_MEMORY_NO_LEXICAL_PENALTY", 0.4);
      if (support < min_support)
      {
         memset(parts, 0, sizeof(*parts));
         return;
      }
      parts->state -= no_lexical_penalty;
   }

   /* parts->confidence is NOT set here: confidence is a calibration artifact,
    * not a retrieval-relevance signal.  Callers populate it post-ranking from
    * the full memory_t for display/trace only. */
   /* Graph-code fusion contribution. When the recall path staged graph-vector
    * expansions for this request (graph_code_fusion_state == "on"), this
    * populates parts->graph_score / parts->code_proximity for the memory; when
    * fusion is off the hook is a no-op, graph_score stays 0, the term below adds
    * 0, and the score is byte-identical to the pre-fusion behaviour. */
   memory_fusion_expansions_apply(parts, m->id);
   parts->total = parts->lexical + parts->coverage + parts->entity + parts->temporal +
                  parts->evidence + parts->semantic + parts->state + parts->intent +
                  parts->salience + parts->surprise + parts->pagerank +
                  memory_env_weight("AIMEE_MEMORY_GRAPH_WEIGHT", 0.30) * parts->graph_score;
}

static int memory_token_in_norm(const char *norm, const char *token)
{
   if (!norm || !token || !token[0])
      return 0;
   size_t tlen = strlen(token);
   const char *p = norm;
   while ((p = strstr(p, token)) != NULL)
   {
      int left_ok = (p == norm) || p[-1] == ' ';
      int right_ok = p[tlen] == '\0' || p[tlen] == ' ';
      if (left_ok && right_ok)
         return 1;
      p += tlen;
   }
   return 0;
}

static int memory_split_tokens(const char *norm, char tokens[][64], int max_tokens)
{
   if (!norm || !norm[0] || !tokens || max_tokens <= 0)
      return 0;
   int count = 0;
   const char *p = norm;
   while (*p && count < max_tokens)
   {
      while (*p == ' ')
         p++;
      if (!*p)
         break;
      int len = 0;
      while (p[len] && p[len] != ' ' && len < 63)
      {
         tokens[count][len] = p[len];
         len++;
      }
      tokens[count][len] = '\0';
      count++;
      p += len;
      while (*p && *p != ' ')
         p++;
   }
   return count;
}

static void memory_build_signal_query(char tokens[][64], int token_count, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!tokens || token_count <= 0)
      return;

   size_t used = 0;
   for (int i = 0; i < token_count; i++)
   {
      if (!memory_is_signal_token(tokens[i]))
         continue;
      int wrote = snprintf(out + used, out_len - used, "%s%s", used ? " " : "", tokens[i]);
      if (wrote < 0)
         break;
      if ((size_t)wrote >= out_len - used)
      {
         out[out_len - 1] = '\0';
         break;
      }
      used += (size_t)wrote;
   }
}

static int memory_query_has_separator(char tokens[][64], int token_count, int idx)
{
   if (!tokens || idx < 0 || idx >= token_count)
      return 0;
   return strcmp(tokens[idx], "and") == 0 || strcmp(tokens[idx], "or") == 0 ||
          strcmp(tokens[idx], "vs") == 0 || strcmp(tokens[idx], "versus") == 0 ||
          strcmp(tokens[idx], "then") == 0 || strcmp(tokens[idx], "before") == 0 ||
          strcmp(tokens[idx], "after") == 0;
}

static int memory_compact_signal_tokens(char tokens[][64], int start, int end, char *out,
                                        size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!tokens || start < 0 || end <= start)
      return 0;

   size_t used = 0;
   int kept = 0;
   for (int i = start; i < end; i++)
   {
      if (!memory_is_signal_token(tokens[i]) || memory_query_has_separator(tokens, end, i))
         continue;
      int wrote = snprintf(out + used, out_len - used, "%s%s", used ? " " : "", tokens[i]);
      if (wrote < 0 || (size_t)wrote >= out_len - used)
         break;
      used += (size_t)wrote;
      kept++;
   }
   return kept;
}

static int memory_build_query_decomposition(const char *norm_query, char subqueries[][128],
                                            int max_subqueries)
{
   if (!norm_query || !norm_query[0] || !subqueries || max_subqueries <= 0)
      return 0;

   char tokens[32][64];
   int token_count = memory_split_tokens(norm_query, tokens, 32);
   if (token_count < 4)
      return 0;

   int count = 0;
   for (int i = 0; i < token_count && count + 1 < max_subqueries; i++)
   {
      if (!memory_query_has_separator(tokens, token_count, i))
         continue;
      if (memory_compact_signal_tokens(tokens, 0, i, subqueries[count], sizeof(subqueries[0])) >= 2)
         count++;
      if (memory_compact_signal_tokens(tokens, i + 1, token_count, subqueries[count],
                                       sizeof(subqueries[0])) >= 2)
         count++;
      break;
   }

   if (count == 0)
   {
      char signal_tokens[32][64];
      int signal_count = 0;
      for (int i = 0; i < token_count && signal_count < 32; i++)
      {
         if (!memory_is_signal_token(tokens[i]))
            continue;
         snprintf(signal_tokens[signal_count], sizeof(signal_tokens[signal_count]), "%s",
                  tokens[i]);
         signal_count++;
      }
      if (signal_count >= 5)
      {
         int window = signal_count >= 8 ? 4 : 3;
         if (memory_compact_signal_tokens(signal_tokens, 0, window, subqueries[count],
                                          sizeof(subqueries[0])) >= 2)
            count++;
         if (count < max_subqueries &&
             memory_compact_signal_tokens(signal_tokens, signal_count - window, signal_count,
                                          subqueries[count], sizeof(subqueries[0])) >= 2)
            count++;
      }
   }

   for (int i = 0; i < count; i++)
   {
      for (int j = i + 1; j < count; j++)
      {
         if (strcmp(subqueries[i], subqueries[j]) == 0)
            subqueries[j][0] = '\0';
      }
   }

   int compacted = 0;
   for (int i = 0; i < count; i++)
   {
      if (!subqueries[i][0])
         continue;
      if (compacted != i)
         snprintf(subqueries[compacted], sizeof(subqueries[compacted]), "%s", subqueries[i]);
      compacted++;
   }
   return compacted;
}

static double memory_session_cluster_bonus(const memory_t *matches, int count, int idx)
{
   if (!matches || count <= 1 || idx < 0 || idx >= count || !matches[idx].source_session[0])
      return 0.0;
   int same_session = 0;
   int kind_variety = 0;
   int has_episode = 0;
   for (int i = 0; i < count; i++)
   {
      if (i == idx || strcmp(matches[i].source_session, matches[idx].source_session) != 0)
         continue;
      same_session++;
      if (strcmp(matches[i].kind, matches[idx].kind) != 0)
         kind_variety = 1;
      if (strcmp(matches[i].kind, KIND_EPISODE) == 0)
         has_episode = 1;
   }
   double bonus = 0.0;
   if (same_session > 0)
      bonus += (double)(same_session > 3 ? 3 : same_session) * 0.12;
   if (kind_variety)
      bonus += 0.08;
   if (has_episode)
      bonus += 0.10;
   if (strcmp(matches[idx].kind, KIND_EPISODE) == 0 && same_session > 0)
      bonus += 0.05;
   return bonus;
}

/* Scene cluster bonus: boost candidates that share a scene.
 * Looks up scene memberships for all candidates in one SQL query per candidate,
 * then counts scene co-membership and adds a bonus for popular scenes. */
static double memory_scene_cluster_bonus(const memory_t *matches, int count, int idx)
{
   if (!matches || count <= 1 || idx < 0 || idx >= count)
      return 0.0;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.memory_scenes_enabled)
      return 0.0;

   db2_memory_scene_membership_t memberships[16];
   int scene_count = db2_memory_scene_memberships_for_memory(
       matches[idx].id, memberships, (int)(sizeof(memberships) / sizeof(memberships[0])));
   if (scene_count <= 0)
      return 0.0;

   int co_members = 0;
   double best_strength = 0.0;
   for (int i = 0; i < count && co_members < 3; i++)
   {
      if (i == idx)
         continue;
      for (int s = 0; s < scene_count; s++)
      {
         if (db2_memory_scene_member_exists(matches[i].id, memberships[s].scene_id))
         {
            co_members++;
            if (memberships[s].membership_strength > best_strength)
               best_strength = memberships[s].membership_strength;
         }
         if (co_members >= 3)
            break;
      }
   }

   double bonus = co_members * 0.10 + best_strength * 0.05;
   return bonus;
}

typedef struct
{
   int64_t memory_id;
   unsigned int source_mask;
} memory_candidate_source_t;

enum
{
   MEM_SOURCE_LEXICAL = 1u << 0,
   MEM_SOURCE_ALIAS = 1u << 1,
   MEM_SOURCE_ENTITY = 1u << 2,
   MEM_SOURCE_SUMMARY = 1u << 3,
   MEM_SOURCE_EVENT = 1u << 4,
   MEM_SOURCE_CHUNK = 1u << 5,
   MEM_SOURCE_UNIT = 1u << 6,
   MEM_SOURCE_TEMPORAL = 1u << 7,
   MEM_SOURCE_SEMANTIC = 1u << 8,
   MEM_SOURCE_LIKE = 1u << 9,
   MEM_SOURCE_CODE = 1u << 10,
   MEM_SOURCE_GRAPH = 1u << 11
};

static void memory_note_candidate_sources(memory_candidate_source_t *stats, int *stats_count,
                                          int stats_max, const memory_t *matches, int start,
                                          int end, unsigned int source_mask)
{
   if (!stats || !stats_count || !matches || source_mask == 0)
      return;
   if (start < 0)
      start = 0;
   if (end < start)
      return;
   for (int i = start; i < end; i++)
   {
      int found = -1;
      for (int j = 0; j < *stats_count; j++)
      {
         if (stats[j].memory_id == matches[i].id)
         {
            found = j;
            break;
         }
      }
      if (found >= 0)
      {
         stats[found].source_mask |= source_mask;
         continue;
      }
      if (*stats_count >= stats_max)
         continue;
      stats[*stats_count].memory_id = matches[i].id;
      stats[*stats_count].source_mask = source_mask;
      (*stats_count)++;
   }
}

static double memory_source_fusion_bonus(const memory_candidate_source_t *stats, int stats_count,
                                         int64_t memory_id)
{
   double bonus = 0.0;
   for (int i = 0; i < stats_count; i++)
   {
      if (stats[i].memory_id != memory_id)
         continue;
      unsigned int mask = stats[i].source_mask;
      int sources = 0;
      while (mask)
      {
         if (mask & 1u)
            sources++;
         mask >>= 1u;
      }
      if (sources > 1)
         bonus += (double)(sources - 1) * 0.10;
      if ((stats[i].source_mask & MEM_SOURCE_SEMANTIC) &&
          (stats[i].source_mask & MEM_SOURCE_LEXICAL))
         bonus += 0.18;
      if (stats[i].source_mask & MEM_SOURCE_GRAPH)
         bonus += 0.14;
      if ((stats[i].source_mask & MEM_SOURCE_EVENT) || (stats[i].source_mask & MEM_SOURCE_TEMPORAL))
         bonus += 0.08;
      break;
   }
   return bonus;
}

static void memory_record_query_stage_metric(const memory_query_plan_t *plan,
                                             const char *stage_name)
{
   char key[128];
   if (!stage_name || !stage_name[0])
      return;
   snprintf(key, sizeof(key), "memory.query.stage.%s", stage_name);
   memory_runtime_state_increment(key, 1);
   if (plan)
   {
      snprintf(key, sizeof(key), "memory.query.route.%s.stage.%s",
               memory_query_route_name(plan->route), stage_name);
      memory_runtime_state_increment(key, 1);
   }
}

static int memory_rank_position(const double *values, int count, int idx, double min_value)
{
   if (!values || idx < 0 || idx >= count || values[idx] < min_value)
      return 0;
   int rank = 1;
   for (int i = 0; i < count; i++)
   {
      if (i == idx || values[i] < min_value)
         continue;
      if (values[i] > values[idx] + 1e-9)
         rank++;
   }
   return rank;
}

static double memory_rrf_bonus(const memory_score_parts_t *parts, const double *phrase_bonus,
                               const double *fusion_bonus, int count, int idx)
{
   if (!parts || idx < 0 || idx >= count)
      return 0.0;

   double lexical_signal[128];
   double semantic_signal[128];
   double entity_signal[128];
   double temporal_signal[128];
   double evidence_signal[128];
   double source_signal[128];
   int n = count > 128 ? 128 : count;
   for (int i = 0; i < n; i++)
   {
      lexical_signal[i] =
          parts[i].lexical + parts[i].coverage + (phrase_bonus ? phrase_bonus[i] : 0.0);
      semantic_signal[i] = parts[i].semantic;
      entity_signal[i] = parts[i].entity;
      temporal_signal[i] = parts[i].temporal;
      evidence_signal[i] = parts[i].evidence;
      source_signal[i] = fusion_bonus ? fusion_bonus[i] : 0.0;
   }

   static const double rank_k = 20.0;
   static const double scale = 3.0;
   double sum = 0.0;
   int rank = memory_rank_position(lexical_signal, n, idx, 0.30);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(semantic_signal, n, idx, 0.18);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(entity_signal, n, idx, 0.12);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(temporal_signal, n, idx, 0.12);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(evidence_signal, n, idx, 0.12);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   rank = memory_rank_position(source_signal, n, idx, 0.10);
   if (rank > 0)
      sum += 1.0 / (rank_k + (double)rank);
   return sum * scale;
}

static double memory_phrase_bonus(const char *raw_query, const char *norm_query,
                                  const memory_ranker_input_t *m)
{
   if (!m)
      return 0.0;

   char norm_key[512];
   char norm_content[2048];
   normalize_key(m->key, norm_key, sizeof(norm_key));
   normalize_key(m->content, norm_content, sizeof(norm_content));

   double bonus = 0.0;
   if (norm_query && norm_query[0])
   {
      if (strstr(norm_key, norm_query))
         bonus += 1.15;
      if (strstr(norm_content, norm_query))
         bonus += 0.95;
   }

   if (raw_query && raw_query[0] && strstr(m->content, raw_query))
      bonus += 0.35;

   return bonus;
}

/* Cross-encoder reranker: calls an external command with a JSON array of
 * [query, candidate_text] pairs on stdin; expects a JSON float array of scores on stdout.
 * Returns the number of scores populated in out_scores (≤ count).
 * On any failure (command missing, parse error, timeout) returns 0 so the caller
 * can fall back to hybrid ordering. */
static int memory_cross_encoder_scores(const char *command, const char *query,
                                       const memory_t *matches, int count, double *out_scores)
{
   if (!command || !command[0] || !query || !query[0] || !matches || count <= 0 || !out_scores)
      return 0;

   /* Build JSON input: [[query, text0], [query, text1], ...] */
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return 0;

   for (int i = 0; i < count; i++)
   {
      /* Candidate text = "key: content" (same format used for embeddings) */
      char cand[3072];
      snprintf(cand, sizeof(cand), "%s: %s", matches[i].key, matches[i].content);

      cJSON *pair = cJSON_CreateArray();
      if (!pair)
      {
         cJSON_Delete(arr);
         return 0;
      }
      cJSON_AddItemToArray(pair, cJSON_CreateString(query));
      cJSON_AddItemToArray(pair, cJSON_CreateString(cand));
      cJSON_AddItemToArray(arr, pair);
   }

   char *input_json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   if (!input_json)
      return 0;

   char *buf = NULL;
   size_t buf_len = 0;
   int rc = platform_exec_pipe(command, input_json, strlen(input_json), &buf, &buf_len);
   free(input_json);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "memory", "cross-encoder command failed (exit %d)", rc);
      free(buf);
      return 0;
   }
   if (!buf || buf_len == 0)
   {
      free(buf);
      return 0;
   }

   cJSON *scores = cJSON_Parse(buf);
   free(buf);
   if (!scores || !cJSON_IsArray(scores))
   {
      cJSON_Delete(scores);
      aimee_log(LOG_WARN, "memory", "cross-encoder returned invalid JSON; falling back");
      return 0;
   }

   int n = 0;
   cJSON *el;
   cJSON_ArrayForEach(el, scores)
   {
      if (n >= count)
         break;
      out_scores[n++] = cJSON_IsNumber(el) ? el->valuedouble : 0.0;
   }
   cJSON_Delete(scores);
   return n;
}

/* Return the effective cross-encoder command, or NULL if disabled.
 * AIMEE_MEMORY_RERANK_FORCE_OFF overrides config — used by the
 * benchmark harness' --compare-rerank mode to run the same corpus
 * twice and surface the delta. */
static const char *memory_cross_encoder_command(const config_t *cfg)
{
   if (!cfg || !cfg->memory_rerank_enabled)
      return NULL;
   const char *force_off = getenv("AIMEE_MEMORY_RERANK_FORCE_OFF");
   if (force_off && force_off[0] && force_off[0] != '0')
      return NULL;
   if (!cfg->memory_rerank_command[0])
      return NULL;
   return cfg->memory_rerank_command;
}

static int memory_rerank_matches(const char *raw_query, memory_t *matches, int count, int limit,
                                 const int64_t *semantic_ids, const double *semantic_scores,
                                 int semantic_hit_count,
                                 const memory_candidate_source_t *source_stats,
                                 int source_stats_count)
{
   if (!raw_query || !matches || count <= 1)
      return count < limit ? count : limit;

   char norm_query[512];
   normalize_key(raw_query, norm_query, sizeof(norm_query));
   if (!norm_query[0])
      snprintf(norm_query, sizeof(norm_query), "%s", raw_query);

   double scores[128];
   double phrase_bonus[128];
   double fusion_bonus[128];
   memory_score_parts_t parts[128];
   memory_pagerank_score_t pagerank_scores[128];
   memory_pagerank_config_t pagerank_cfg;
   if (count > (int)(sizeof(scores) / sizeof(scores[0])))
      count = (int)(sizeof(scores) / sizeof(scores[0]));
   memory_load_pagerank_config(&pagerank_cfg);
   if (pagerank_cfg.enabled)
      count = memory_append_linked_neighbors(matches, count, 128, pagerank_cfg.relations);
   int pagerank_count =
       memory_compute_pagerank_scores(matches, count, &pagerank_cfg, pagerank_scores, 128);
   for (int i = 0; i < count; i++)
   {
      memory_ranker_input_t ri = memory_ranker_input_from(&matches[i]);
      memory_compute_score_parts(raw_query, norm_query, &ri, semantic_ids, semantic_scores,
                                 semantic_hit_count, pagerank_scores, pagerank_count, &parts[i]);
      scores[i] = parts[i].total;
      double fusion = memory_source_fusion_bonus(source_stats, source_stats_count, matches[i].id);
      if (scores[i] < 3.5)
         fusion *= 0.15;
      else if (scores[i] < 5.5)
         fusion *= 0.45;
      fusion_bonus[i] = fusion;
      /* Phase 6: surface the applied multi-source fusion bonus in score parts
       * so `memory diagnose` can explain the fused contribution. */
      parts[i].source_fusion = fusion;
      phrase_bonus[i] = 0.0;
      scores[i] += fusion_bonus[i];
      scores[i] += memory_session_cluster_bonus(matches, count, i);
      scores[i] += memory_scene_cluster_bonus(matches, count, i);
   }

   if (memory_rerank_is_slow())
   {
      int slow_top = count > 16 ? 16 : count;
      for (int i = 0; i < slow_top; i++)
      {
         memory_ranker_input_t pb_ri = memory_ranker_input_from(&matches[i]);
         phrase_bonus[i] = memory_phrase_bonus(raw_query, norm_query, &pb_ri);
         scores[i] += phrase_bonus[i];
      }
   }

   for (int i = 0; i < count; i++)
   {
      double rrf = memory_rrf_bonus(parts, phrase_bonus, fusion_bonus, count, i);
      if (scores[i] < 3.5)
         rrf *= 0.35;
      scores[i] += rrf;
   }

   /* Capture the pre-rerank hybrid score on every candidate so the explain
    * surface can differentiate hybrid vs blended even for rows outside the
    * cross-encoder top_k window. When the CE pass runs, it overwrites
    * blended_total + rerank_mix for its window; rows beyond it keep a zero
    * rerank_mix (which the explain serializer reads as "not reranked"). */
   for (int i = 0; i < count; i++)
   {
      parts[i].hybrid_total = scores[i];
      parts[i].blended_total = scores[i];
      parts[i].rerank_mix = 0.0;
      matches[i].retrieval_score = scores[i];
      matches[i].hybrid_rank = 0;
   }

   /* Cross-encoder second pass: score top-K candidates with a local cross-encoder model
    * (external command), then blend with the hybrid score.
    * Falls back silently to hybrid ordering if the command is not configured or fails. */
   config_t ce_cfg;
   config_load(&ce_cfg);
   const char *ce_cmd = memory_cross_encoder_command(&ce_cfg);
   if (ce_cmd)
   {
      int top_k = ce_cfg.memory_rerank_top_k > 0 ? ce_cfg.memory_rerank_top_k : 50;
      if (top_k > count)
         top_k = count;
      double mix = (ce_cfg.memory_rerank_mix > 0.0 && ce_cfg.memory_rerank_mix <= 1.0)
                       ? ce_cfg.memory_rerank_mix
                       : 0.7;

      double ce_scores[128] = {0};
      int ce_count = memory_cross_encoder_scores(ce_cmd, raw_query, matches, top_k, ce_scores);
      if (ce_count == top_k)
      {
         /* Normalise cross-encoder scores to [0, 1] for blending */
         double ce_min = ce_scores[0], ce_max = ce_scores[0];
         for (int i = 1; i < ce_count; i++)
         {
            if (ce_scores[i] < ce_min)
               ce_min = ce_scores[i];
            if (ce_scores[i] > ce_max)
               ce_max = ce_scores[i];
         }
         double ce_range = ce_max - ce_min;

         /* Normalise hybrid scores for the same pool */
         double h_min = scores[0], h_max = scores[0];
         for (int i = 1; i < top_k; i++)
         {
            if (scores[i] < h_min)
               h_min = scores[i];
            if (scores[i] > h_max)
               h_max = scores[i];
         }
         double h_range = h_max - h_min;

         for (int i = 0; i < ce_count; i++)
         {
            double ce_norm = (ce_range > 0) ? (ce_scores[i] - ce_min) / ce_range : 0.5;
            double h_norm = (h_range > 0) ? (scores[i] - h_min) / h_range : 0.5;
            double blended = mix * ce_norm + (1.0 - mix) * h_norm;
            parts[i].cross_encoder = ce_scores[i];
            parts[i].hybrid_total = scores[i];
            parts[i].rerank_mix = mix;
            scores[i] = blended * 10.0; /* rescale to ~same magnitude as hybrid */
            parts[i].blended_total = scores[i];
         }
      }
      else if (ce_count > 0)
      {
         /* Partial result — warn and fall back */
         aimee_log(LOG_WARN, "memory",
                   "cross-encoder returned %d scores for %d candidates; using hybrid ordering",
                   ce_count, top_k);
      }
   }

   /* Contradiction-aware reranking: penalize older memories that contradict a
    * higher-ranked candidate.  Only applied to the top-16 pool to keep cost
    * proportional.  A memory that is dominated by a newer conflicting fact
    * receives a score reduction of 30% of the dominator's score gap. */
   {
      int contra_top = count > 16 ? 16 : count;
      for (int i = 0; i < contra_top; i++)
      {
         for (int j = i + 1; j < contra_top; j++)
         {
            if (!matches[i].content[0] || !matches[j].content[0])
               continue;
            if (!is_contradiction(matches[i].content, matches[j].content))
               continue;
            if (matches[i].created_at[0] && matches[j].created_at[0])
            {
               int newer = strcmp(matches[i].created_at, matches[j].created_at);
               if (newer == 0)
                  continue;
               int newer_idx = newer > 0 ? i : j;
               int older_idx = newer > 0 ? j : i;
               double gap = fabs(scores[newer_idx] - scores[older_idx]);
               double shift = 0.18 + gap * 0.22;
               scores[newer_idx] += shift;
               scores[older_idx] -= shift;
            }
         }
      }
   }

   for (int i = 0; i < count - 1; i++)
   {
      int best = i;
      for (int j = i + 1; j < count; j++)
      {
         if (scores[j] > scores[best])
            best = j;
      }
      if (best != i)
      {
         double ts = scores[i];
         memory_t tm = matches[i];
         memory_score_parts_t tp = parts[i];
         scores[i] = scores[best];
         matches[i] = matches[best];
         parts[i] = parts[best];
         scores[best] = ts;
         matches[best] = tm;
         parts[best] = tp;
      }
   }

   /* Ordering is fixed.  Now reattach confidence from the full memory record
    * for display/trace surfaces — it must not have entered the score above. */
   for (int i = 0; i < count; i++)
      parts[i].confidence = matches[i].confidence;

   for (int i = 0; i < count; i++)
   {
      int rank = 1;
      for (int j = 0; j < count; j++)
      {
         if (i != j && matches[j].retrieval_score > matches[i].retrieval_score)
            rank++;
      }
      matches[i].hybrid_rank = rank;
   }

   return count < limit ? count : limit;
}

static int memory_scope_matches(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   if (!scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return 1;
   if (db2_memory_scope_matches(memory_id, scope_type, scope_value))
      return 1;
   if (strcmp(scope_type, "workspace") == 0 && db2_memory_workspace_matches(memory_id, scope_value))
      return 1;
   return 0;
}

static int memory_filter_scope(memory_t *matches, int count, const char *scope_type,
                               const char *scope_value)
{
   if (!scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return count;
   int kept = 0;
   for (int i = 0; i < count; i++)
   {
      if (!memory_scope_matches(matches[i].id, scope_type, scope_value))
         continue;
      if (kept != i)
         matches[kept] = matches[i];
      kept++;
   }
   return kept;
}

static int memory_find_facts_lexical_fallback(const char *query, const char *scope_type,
                                              const char *scope_value, int limit, memory_t *out,
                                              int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   memory_t scratch[64];
   int fetch_limit = limit * 4;
   int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
   if (fetch_limit < limit)
      fetch_limit = limit;
   if (fetch_limit > cap)
      fetch_limit = cap;

   int count = 0;
   int got = memory_find_facts_like(query, fetch_limit, scratch, cap);
   if (got < 0)
      return got;
   got = memory_filter_scope(scratch, got, scope_type, scope_value);
   for (int i = 0; i < got && count < limit; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);

   char norm_query[512];
   char tokens[24][64];
   normalize_key(query, norm_query, sizeof(norm_query));
   int token_count = memory_split_tokens(norm_query, tokens, 24);
   for (int t = 0; t < token_count && count < limit; t++)
   {
      if (!memory_is_signal_token(tokens[t]))
         continue;
      got = memory_find_facts_like(tokens[t], fetch_limit, scratch, cap);
      if (got < 0)
         return got;
      got = memory_filter_scope(scratch, got, scope_type, scope_value);
      for (int i = 0; i < got && count < limit; i++)
         count = memory_append_unique(out, count, max, &scratch[i]);
   }

   return count;
}

static int memory_find_facts_visible_lexical_fallback(const char *query, const char *workspace,
                                                      const char *project, int limit, memory_t *out,
                                                      int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   memory_t scratch[64];
   int fetch_limit = limit * 4;
   int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
   if (fetch_limit < limit)
      fetch_limit = limit;
   if (fetch_limit > cap)
      fetch_limit = cap;

   int count = 0;
   int got = memory_find_facts_like(query, fetch_limit, scratch, cap);
   if (got < 0)
      return got;
   for (int i = 0; i < got && count < cap; i++)
      count = memory_append_unique(scratch, count, cap, &scratch[i]);

   char norm_query[512];
   char tokens[24][64];
   normalize_key(query, norm_query, sizeof(norm_query));
   int token_count = memory_split_tokens(norm_query, tokens, 24);
   for (int t = 0; t < token_count && count < cap; t++)
   {
      if (!memory_is_signal_token(tokens[t]))
         continue;
      memory_t term_matches[64];
      int term_cap = (int)(sizeof(term_matches) / sizeof(term_matches[0]));
      got = memory_find_facts_like(tokens[t], fetch_limit, term_matches, term_cap);
      if (got < 0)
         return got;
      for (int i = 0; i < got && count < cap; i++)
         count = memory_append_unique(scratch, count, cap, &term_matches[i]);
   }

   int kept = 0;
   int scope_rank[64];
   for (int i = 0; i < count; i++)
   {
      int rank = memory_scope_visibility_rank(scratch[i].id, workspace, project);
      if (rank <= 0)
         continue;
      if (kept != i)
         scratch[kept] = scratch[i];
      scope_rank[kept] = rank;
      kept++;
   }

   for (int i = 0; i < kept - 1; i++)
   {
      int best = i;
      for (int j = i + 1; j < kept; j++)
         if (scope_rank[j] > scope_rank[best])
            best = j;
      if (best != i)
      {
         memory_t tmp_mem = scratch[i];
         int tmp_rank = scope_rank[i];
         scratch[i] = scratch[best];
         scope_rank[i] = scope_rank[best];
         scratch[best] = tmp_mem;
         scope_rank[best] = tmp_rank;
      }
   }

   if (kept > limit)
      kept = limit;
   for (int i = 0; i < kept; i++)
      out[i] = scratch[i];
   return kept;
}

static int memory_query_is_code_like(const char *query)
{
   if (!query || !query[0])
      return 0;
   int upper = 0;
   int lower = 0;
   int has_space = 0;
   int camel_case = 0;
   unsigned char prev = '\0';
   for (const unsigned char *p = (const unsigned char *)query; *p; p++)
   {
      if (isspace(*p))
         has_space = 1;
      if (*p == '_' || *p == '/' || *p == '.' || *p == ':' || *p == '-' || *p == '#')
         return 1;
      if (prev && islower(prev) && isupper(*p))
         camel_case = 1;
      if (isupper(*p))
         upper++;
      else if (islower(*p))
         lower++;
      prev = *p;
   }
   if (camel_case)
      return 1;
   return !has_space && upper > 1 && lower > 1;
}

/* Code-shaped queries used to hit separate lexical indexes. With direct text
 * recall routed through pgvector, code_matches now runs the same
 * memory-level vector search; the caller still tags hits with MEM_SOURCE_CODE
 * so retrieval-source diagnostics keep working. */
static int memory_collect_code_matches(const char *query, int fetch_limit, memory_t *out, int count,
                                       int max)
{
   if (!query || !query[0] || !out || count >= max || fetch_limit <= 0)
      return count;
   memory_t scratch[64];
   int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
   config_t code_cfg;
   config_load(&code_cfg);
   const char *embed_cmd = code_cfg.embedding_command[0] ? code_cfg.embedding_command : "builtin";
   int got = memory_collect_memory_matches_via_vector(query, embed_cmd, fetch_limit, scratch, cap);
   for (int i = 0; i < got && count < max; i++)
      count = memory_append_unique(out, count, max, &scratch[i]);
   return count;
}

/* Populate an expanded_terms[][] buffer from semantically-near memory
 * keys.  Parallels memory_expand_query_terms but uses the embedding
 * space instead of a hardcoded synonym table.
 *
 * Walks the top-k vector hits, extracts signal tokens from each hit's
 * normalised key, and appends them to `expanded` up to `max_terms`.
 * Caller is responsible for seeding `expanded` with the base tokens
 * first — this function only augments. Returns the new count, or the
 * original count when semantic expansion is unavailable. */
/* Search pgvector for unit points matching the embedded query, group hits by
 * parent memory_id, require ≥2 corroborating units per memory, and fill
 * `out` with the parent memory_t rows ordered by hit_count DESC then best
 * score DESC. Replaces the old unit-text lookup path with a vector-native
 * corroboration filter. */
static int memory_collect_unit_matches_via_vector(const char *query, const char *embed_cmd,
                                                  int limit, memory_t *out, int max)
{
   if (!query || !query[0] || !out || limit <= 0 || max <= 0)
      return 0;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   /* Over-fetch unit hits so the corroboration filter (≥2 distinct units per
    * memory) has room to find groups. 4× the requested limit, capped at 256. */
   int fetch_k = limit * 4;
   if (fetch_k > 256)
      fetch_k = 256;
   int64_t point_ids[256];
   double scores[256];
   int hits =
       pgvec_memory_vector_search_record_type("unit", qvec, qdim, fetch_k, point_ids, scores, 256);
   if (hits <= 0)
      return 0;

   /* Group by parent memory_id. Linear scan is fine — fetch_k ≤ 256 and the
    * number of distinct memories is even smaller. */
   struct
   {
      int64_t memory_id;
      int hit_count;
      double best_score;
   } groups[64];
   int n_groups = 0;
   for (int i = 0; i < hits; i++)
   {
      int64_t unit_id = point_ids[i] - PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET;
      if (unit_id <= 0)
         continue;
      memory_t parent;
      if (db2_memory_get_by_unit_id(unit_id, &parent) != 0)
         continue;
      int gi = -1;
      for (int g = 0; g < n_groups; g++)
         if (groups[g].memory_id == parent.id)
         {
            gi = g;
            break;
         }
      if (gi < 0)
      {
         if (n_groups >= (int)(sizeof(groups) / sizeof(groups[0])))
            continue;
         gi = n_groups++;
         groups[gi].memory_id = parent.id;
         groups[gi].hit_count = 0;
         groups[gi].best_score = scores[i];
      }
      groups[gi].hit_count++;
      if (scores[i] > groups[gi].best_score)
         groups[gi].best_score = scores[i];
   }

   /* Sort by hit_count DESC, then best_score DESC. n_groups is small. */
   for (int i = 0; i < n_groups - 1; i++)
   {
      int best = i;
      for (int j = i + 1; j < n_groups; j++)
      {
         if (groups[j].hit_count > groups[best].hit_count ||
             (groups[j].hit_count == groups[best].hit_count &&
              groups[j].best_score > groups[best].best_score))
            best = j;
      }
      if (best != i)
      {
         typeof(groups[0]) tmp = groups[i];
         groups[i] = groups[best];
         groups[best] = tmp;
      }
   }

   int n = 0;
   for (int g = 0; g < n_groups && n < max; g++)
   {
      if (groups[g].hit_count < 2)
         continue;
      memory_t mem;
      if (memory_fetch_row_by_id(groups[g].memory_id, &mem) == 0)
         out[n++] = mem;
   }
   return n;
}

/* Embed `query`, search the pgvector memory index, and fill `out` with the
 * resulting memory_t rows in score order. Dense memory retrieval lives in
 * pgvector inside DB2 (no separate tier). */
static int memory_collect_memory_matches_via_vector(const char *query, const char *embed_cmd,
                                                    int limit, memory_t *out, int max)
{
   if (!query || !query[0] || !out || limit <= 0 || max <= 0)
      return 0;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   int fetch_k = limit < max ? limit : max;
   if (fetch_k > 64)
      fetch_k = 64;
   int64_t ids[64];
   double scores[64];
   int hits = pgvec_memory_vector_search_record_type("memory", qvec, qdim, fetch_k, ids, scores,
                                                     (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits <= 0)
      return 0;

   int n = 0;
   for (int i = 0; i < hits && n < max; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) == 0)
         out[n++] = mem;
   }
   return n;
}

/* Two-lane retrieval: lane membership communicated from memory_generate_candidates
 * to the rerank callers (memory_recall, memory_find_facts_visible).  Written on
 * every memory_generate_candidates call (zeroed when lanes are disabled). */
static int64_t s_lane_summary_ids[96];
static int s_lane_summary_count;
static int64_t s_lane_fact_ids[96];
static int s_lane_fact_count;

/* Parse a comma-separated kinds string into an array of pointers into `buf`.
 * Returns the number of non-empty entries written to `ptrs[]`. */
static int memory_parse_kinds_csv(const char *csv, char buf[][16], const char **ptrs, int max)
{
   if (!csv || !csv[0] || !buf || !ptrs || max <= 0)
      return 0;
   int n = 0;
   const char *p = csv;
   while (*p && n < max)
   {
      while (*p == ',' || *p == ' ')
         p++;
      if (!*p)
         break;
      int i = 0;
      while (*p && *p != ',' && i < 15)
         buf[n][i++] = *p++;
      buf[n][i] = '\0';
      if (buf[n][0])
      {
         ptrs[n] = buf[n];
         n++;
      }
   }
   return n;
}

/* Kind-filtered variant: embeds `query`, issues a pgvector search restricted to
 * memories whose `kind` field matches one of the entries in `kinds[0..n_kinds)`,
 * and fills `out` with the resulting rows. A NULL/empty kinds list falls back to
 * the unfiltered path. */
static int memory_collect_memory_matches_via_vector_with_kinds(const char *query,
                                                               const char *embed_cmd, int limit,
                                                               const char *const *kinds,
                                                               int n_kinds, memory_t *out, int max)
{
   if (!query || !query[0] || !out || limit <= 0 || max <= 0)
      return 0;
   if (!kinds || n_kinds <= 0)
      return memory_collect_memory_matches_via_vector(query, embed_cmd, limit, out, max);

   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   int fetch_k = limit < max ? limit : max;
   if (fetch_k > 64)
      fetch_k = 64;
   int64_t ids[64];
   double scores[64];
   int hits = pgvec_memory_vector_search_with_kinds(qvec, qdim, kinds, n_kinds, fetch_k, ids,
                                                    scores, (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits <= 0)
      return 0;

   int n = 0;
   for (int i = 0; i < hits && n < max; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) == 0)
         out[n++] = mem;
   }
   return n;
}

/* Post-rerank lane floor: after `matches[0..count)` have been sorted by score,
 * ensure at least `floor_n` of them belong to lane `lane_ids[0..lane_count)`.
 * If fewer than `floor_n` lane members are already in the top `total` results,
 * swap in the highest-ranked missing lane member from the tail.
 *
 * Returns the new effective top-window size after promotion.  Callers that
 * apply floors for multiple lanes in sequence MUST pass the return value of
 * each call as `total` to the next call — this prevents the second floor from
 * displacing items promoted by the first. */
static int memory_apply_lane_floor(memory_t *matches, int count, const int64_t *lane_ids,
                                   int lane_count, int floor_n, int total)
{
   if (floor_n <= 0 || count <= 0 || lane_count <= 0 || total <= 0)
      return total;
   if (total > count)
      total = count;

   /* Count how many of the top `total` are already from this lane. */
   int lane_in_top = 0;
   for (int i = 0; i < total; i++)
   {
      for (int j = 0; j < lane_count; j++)
         if (matches[i].id == lane_ids[j])
         {
            lane_in_top++;
            break;
         }
   }
   if (lane_in_top >= floor_n)
      return total;

   /* Scan the tail (positions >= total) for lane members and swap them in. */
   int needed = floor_n - lane_in_top;
   for (int tail = total; tail < count && needed > 0; tail++)
   {
      int is_lane = 0;
      for (int j = 0; j < lane_count; j++)
         if (matches[tail].id == lane_ids[j])
         {
            is_lane = 1;
            break;
         }
      if (!is_lane)
         continue;
      /* Swap the last top-`total` slot with this tail entry. */
      int victim = total - 1;
      memory_t tmp = matches[victim];
      matches[victim] = matches[tail];
      matches[tail] = tmp;
      total--; /* the swapped-in lane member is now "protected" — shrink top window */
      needed--;
   }
   return total;
}

static int memory_expand_query_terms_semantic(const char *query, const char *embed_cmd, int k,
                                              char expanded[][64], int count, int max_terms)
{
   if (!query || !query[0] || !expanded || count >= max_terms)
      return count;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);

   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return count;

   int fetch_k = (k > 0 && k < 16) ? k : 5;
   int64_t ids[16];
   double scores[16];
   int hits = pgvec_memory_vector_search_record_type("memory", qvec, qdim, fetch_k, ids, scores,
                                                     (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits <= 0)
      return count;

   for (int i = 0; i < hits && count < max_terms; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) != 0 || !mem.key[0])
         continue;
      char key_norm[256];
      normalize_key(mem.key, key_norm, sizeof(key_norm));
      char ktokens[8][64];
      int kn = memory_split_tokens(key_norm, ktokens, 8);
      for (int j = 0; j < kn && count < max_terms; j++)
      {
         if (strlen(ktokens[j]) < 3)
            continue;
         if (!memory_is_signal_token(ktokens[j]))
            continue;
         count = memory_add_expanded_term(expanded, count, max_terms, ktokens[j]);
      }
   }
   return count;
}

/* Semantic query expansion: find semantically similar memory keys via pgvector
 * and append their signal tokens to the lexical query string. */
static int memory_expand_query_semantic(const char *query, const char *embed_cmd, int k,
                                        char *lexical_out, size_t lexical_out_len)
{
   if (!query || !query[0] || !lexical_out || lexical_out_len == 0)
      return 0;
   const char *effective_cmd = memory_effective_embedding_cmd(embed_cmd);

   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text_runtime(query, effective_cmd, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   int fetch_k = (k > 0 && k < 16) ? k : 5;
   int64_t ids[16];
   double scores[16];
   int hits = pgvec_memory_vector_search_record_type("memory", qvec, qdim, fetch_k, ids, scores,
                                                     (int)(sizeof(ids) / sizeof(ids[0])));
   if (hits < 0)
      return memory_semantic_query_unavailable();
   if (hits <= 0)
      return 0;

   /* Append signal tokens from the top-k keys to the lexical query. */
   size_t used = strlen(lexical_out);
   for (int i = 0; i < hits; i++)
   {
      memory_t mem;
      if (memory_fetch_row_by_id(ids[i], &mem) != 0 || !mem.key[0])
         continue;
      char key_norm[256];
      normalize_key(mem.key, key_norm, sizeof(key_norm));
      char ktokens[8][64];
      int kn = memory_split_tokens(key_norm, ktokens, 8);
      for (int j = 0; j < kn && used + 72 < lexical_out_len; j++)
      {
         if (memory_is_signal_token(ktokens[j]))
            used += snprintf(lexical_out + used, lexical_out_len - used, " OR %s", ktokens[j]);
      }
   }
   return 0;
}

static int memory_collect_variant_candidates(const char *raw_query, const char *norm_variant,
                                             memory_query_intent_t intent, int fetch_limit,
                                             memory_t *out, int count, int max,
                                             memory_candidate_source_t *source_stats,
                                             int *source_stats_count)
{
   if (!raw_query || !raw_query[0] || !norm_variant || !norm_variant[0] || !out || max <= 0)
      return count;

   char qtokens[32][64];
   char expanded_terms[48][64];
   int qtoken_count = memory_split_tokens(norm_variant, qtokens, 32);
   char signal_query[256];
   memory_build_signal_query(qtokens, qtoken_count, signal_query, sizeof(signal_query));

   char expanded_signal[1024];
   memory_expand_synonyms(signal_query[0] ? signal_query : norm_variant, expanded_signal,
                          sizeof(expanded_signal));

   /* Query expansion modes:
    *   "lexical"  (default) — hardcoded synonym table only
    *   "semantic"          — embedding-space neighbors only
    *   "hybrid"            — both (expansion budget is shared)
    * Semantic mode augments BOTH the lexical query (for unit/memory matches)
    * AND the expanded_terms[][] buffer (for alias/entity/summary/chunk paths),
    * so the two retrieval legs see the same expansion footprint. */
   config_t qe_cfg;
   config_load(&qe_cfg);
   const char *qe_mode =
       qe_cfg.memory_query_expansion_mode[0] ? qe_cfg.memory_query_expansion_mode : "lexical";
   const int qe_semantic =
       (strcmp(qe_mode, "semantic") == 0 || strcmp(qe_mode, "hybrid") == 0) ? 1 : 0;
   const int qe_lexical =
       (strcmp(qe_mode, "lexical") == 0 || strcmp(qe_mode, "hybrid") == 0) ? 1 : 0;

   if (qe_semantic)
   {
      const char *embed_cmd = qe_cfg.embedding_command[0] ? qe_cfg.embedding_command : "builtin";
      int k = qe_cfg.memory_query_expansion_k > 0 ? qe_cfg.memory_query_expansion_k : 5;
      if (memory_expand_query_semantic(signal_query[0] ? signal_query : norm_variant, embed_cmd, k,
                                       expanded_signal, sizeof(expanded_signal)) < 0)
         return -1;
   }

   const char *lexical_query = expanded_signal;
   const char *structured_query = signal_query[0] ? signal_query : norm_variant;
   int expanded_count = 0;
   if (qe_lexical)
      expanded_count =
          memory_expand_query_terms(norm_variant, qtokens, qtoken_count, expanded_terms, 48);
   else
   {
      /* semantic-only: seed the base tokens then augment with embedding-
       * derived signal terms so the alias/entity/summary/chunk/unit/
       * temporal legs see the same neighborhood as the lexical query. */
      for (int i = 0; i < qtoken_count && expanded_count < 48; i++)
         expanded_count = memory_add_expanded_term(expanded_terms, expanded_count, 48, qtokens[i]);
   }
   if (qe_semantic)
   {
      const char *embed_cmd = qe_cfg.embedding_command[0] ? qe_cfg.embedding_command : "builtin";
      int k = qe_cfg.memory_query_expansion_k > 0 ? qe_cfg.memory_query_expansion_k : 5;
      expanded_count =
          memory_expand_query_terms_semantic(signal_query[0] ? signal_query : norm_variant,
                                             embed_cmd, k, expanded_terms, expanded_count, 48);
   }

   {
      int before = count;
      count = memory_collect_unit_matches(lexical_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_UNIT);
   }

   {
      memory_t lexical_scratch[64];
      int lexical_cap = (int)(sizeof(lexical_scratch) / sizeof(lexical_scratch[0]));
      const char *lexical_embed_cmd =
          qe_cfg.embedding_command[0] ? qe_cfg.embedding_command : "builtin";
      config_t lanes_cfg;
      config_load(&lanes_cfg);
      if (lanes_cfg.memory_recall_lanes_enabled)
      {
         char sum_buf[16][16], fact_buf[16][16];
         const char *sum_ptrs[16], *fact_ptrs[16];
         int n_sum = memory_parse_kinds_csv(lanes_cfg.memory_recall_lanes_summary_kinds[0]
                                                ? lanes_cfg.memory_recall_lanes_summary_kinds
                                                : "episode",
                                            sum_buf, sum_ptrs, 16);
         int n_fact = memory_parse_kinds_csv(lanes_cfg.memory_recall_lanes_fact_kinds[0]
                                                 ? lanes_cfg.memory_recall_lanes_fact_kinds
                                                 : "fact,preference",
                                             fact_buf, fact_ptrs, 16);
         int k_sum = lanes_cfg.memory_recall_lanes_k_summary > 0
                         ? lanes_cfg.memory_recall_lanes_k_summary
                         : 40;
         int k_fact =
             lanes_cfg.memory_recall_lanes_k_fact > 0 ? lanes_cfg.memory_recall_lanes_k_fact : 40;

         /* Summary lane */
         int got_sum = memory_collect_memory_matches_via_vector_with_kinds(
             lexical_query, lexical_embed_cmd, k_sum, sum_ptrs, n_sum, lexical_scratch,
             lexical_cap);
         s_lane_summary_count = got_sum < 96 ? got_sum : 96;
         for (int s = 0; s < s_lane_summary_count; s++)
            s_lane_summary_ids[s] = lexical_scratch[s].id;
         for (int s = 0; s < got_sum && count < max; s++)
         {
            int before = count;
            count = memory_append_unique(out, count, max, &lexical_scratch[s]);
            memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                          MEM_SOURCE_LEXICAL);
         }

         /* Fact lane — reuse scratch */
         int got_fact = memory_collect_memory_matches_via_vector_with_kinds(
             lexical_query, lexical_embed_cmd, k_fact, fact_ptrs, n_fact, lexical_scratch,
             lexical_cap);
         s_lane_fact_count = got_fact < 96 ? got_fact : 96;
         for (int s = 0; s < s_lane_fact_count; s++)
            s_lane_fact_ids[s] = lexical_scratch[s].id;
         for (int s = 0; s < got_fact && count < max; s++)
         {
            int before = count;
            count = memory_append_unique(out, count, max, &lexical_scratch[s]);
            memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                          MEM_SOURCE_LEXICAL);
         }
         aimee_log(LOG_DEBUG, "memory", "two-lane: summary=%d fact=%d merged_total=%d", got_sum,
                   got_fact, count);
      }
      else
      {
         s_lane_summary_count = 0;
         s_lane_fact_count = 0;
         int got = memory_collect_memory_matches_via_vector(
             lexical_query, lexical_embed_cmd, fetch_limit, lexical_scratch, lexical_cap);
         for (int s = 0; s < got && count < max; s++)
         {
            int before = count;
            count = memory_append_unique(out, count, max, &lexical_scratch[s]);
            memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                          MEM_SOURCE_LEXICAL);
         }
      }
   }

   if (memory_query_is_code_like(raw_query) || memory_query_is_code_like(structured_query))
   {
      int before = count;
      count = memory_collect_code_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_CODE);
   }

   {
      int before = count;
      count = memory_collect_alias_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ALIAS);
   }
   for (int i = 0; i < expanded_count && count < max; i++)
   {
      if (strlen(expanded_terms[i]) < 3)
         continue;
      int before = count;
      count = memory_collect_alias_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ALIAS);
      before = count;
      count = memory_collect_entity_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ENTITY);
      before = count;
      count = memory_collect_summary_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_SUMMARY);
      before = count;
      count = memory_collect_event_frame_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_EVENT);
      before = count;
      count = memory_collect_chunk_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_CHUNK);
      before = count;
      count = memory_collect_unit_matches(expanded_terms[i], fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_UNIT);
      if (intent == MEM_QUERY_TEMPORAL)
      {
         before = count;
         count = memory_collect_temporal_matches(expanded_terms[i], fetch_limit, out, count, max);
         memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                       MEM_SOURCE_TEMPORAL);
      }
   }

   if (intent == MEM_QUERY_ENTITY || intent == MEM_QUERY_GENERAL)
   {
      int before = count;
      count = memory_collect_entity_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_ENTITY);
   }
   {
      int before = count;
      count = memory_collect_summary_matches(structured_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_SUMMARY);
   }
   {
      int before = count;
      count = memory_collect_event_frame_matches(norm_variant, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_EVENT);
   }
   {
      int before = count;
      count = memory_collect_chunk_matches(lexical_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_CHUNK);
   }
   {
      int before = count;
      count = memory_collect_unit_matches(lexical_query, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_UNIT);
   }
   if (intent == MEM_QUERY_TEMPORAL)
   {
      int before = count;
      count = memory_collect_temporal_matches(norm_variant, fetch_limit, out, count, max);
      memory_note_candidate_sources(source_stats, source_stats_count, 128, out, before, count,
                                    MEM_SOURCE_TEMPORAL);
   }

   return count;
}

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

      memory_t scratch[32];
      int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
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

static int
memory_generate_candidates(const char *query, const char *norm_query, memory_query_intent_t intent,
                           const memory_query_plan_t *plan, int fetch_limit, memory_t *out, int max,
                           int64_t *semantic_ids, double *semantic_scores, int *semantic_hit_count,
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
   const char *embed_cmd = embed_cfg.embedding_command[0] ? embed_cfg.embedding_command : "builtin";
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
            memory_t scratch[64];
            int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));
            const char *embed_cmd =
                neg_cfg.embedding_command[0] ? neg_cfg.embedding_command : "builtin";
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
   memory_t like_matches[128];
   int like_count = memory_find_facts_like(norm_query, fetch_limit, like_matches,
                                           (int)(sizeof(like_matches) / sizeof(like_matches[0])));
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
   if (!cfg->memory_rewrite_command[0])
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

   /* Build input JSON */
   cJSON *input = cJSON_CreateObject();
   if (!input)
      return;
   cJSON_AddStringToObject(input, "query", query);
   cJSON_AddBoolToObject(input, "hyde", cfg->memory_rewrite_hyde);
   cJSON_AddBoolToObject(input, "decompose", cfg->memory_rewrite_decompose);
   int max_sub = cfg->memory_rewrite_max_subqueries > 0 ? cfg->memory_rewrite_max_subqueries : 4;
   cJSON_AddNumberToObject(input, "max_subqueries", max_sub);
   char *input_str = cJSON_PrintUnformatted(input);
   cJSON_Delete(input);
   if (!input_str)
      return;

   char *resp = NULL;
   size_t resp_len = 0;
   int rc = platform_exec_pipe(cfg->memory_rewrite_command, input_str, strlen(input_str), &resp,
                               &resp_len);
   free(input_str);
   if (rc != 0 || !resp || resp_len == 0)
   {
      free(resp);
      aimee_log(LOG_WARN, "memory_rewrite", "rewrite command failed (exit %d)", rc);
      return;
   }

   /* Parse response */
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   if (!j)
   {
      aimee_log(LOG_WARN, "memory_rewrite", "rewrite command returned invalid JSON");
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
   int n = memory_find_facts_scoped(query, NULL, NULL, limit, out, max);
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

      memory_t scratch[32];
      int cap = (int)(sizeof(scratch) / sizeof(scratch[0]));

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
      if (config_load(&cfg) == 0 && cfg.memory_rewrite_enabled && cfg.memory_rewrite_command[0])
         memory_query_rewrite(query, &cfg, &rewrite);
   }

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
      memory_t extra[MEMORY_RERANK_BUFFER];
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
      memory_t sub_cands[MEMORY_RERANK_BUFFER / 2];
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
   memory_t candidates[MEMORY_RERANK_BUFFER];
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
   const char *embed_cmd = embed_cfg.embedding_command[0] ? embed_cfg.embedding_command : "builtin";
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
   const char *embed_cmd = embed_cfg.embedding_command[0] ? embed_cfg.embedding_command : "builtin";
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
