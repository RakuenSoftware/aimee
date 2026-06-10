/* --- Workspace Scoping --- */

const char *memory_scope_level_name(memory_scope_level_t level)
{
   switch (level)
   {
   case MEMORY_SCOPE_GLOBAL:
      return "global";
   case MEMORY_SCOPE_WORKSPACE:
      return "workspace";
   case MEMORY_SCOPE_PROJECT:
      return "project";
   default:
      return "none";
   }
}

static int memory_has_any_canonical_scope(int64_t memory_id)
{
   if (db2_memory_has_scope_type(memory_id, "global") ||
       db2_memory_has_scope_type(memory_id, "workspace") ||
       db2_memory_has_scope_type(memory_id, "project"))
      return 1;
   return db2_memory_has_any_workspace_tag(memory_id);
}

int memory_collect_scopes(int64_t memory_id, memory_scope_tag_t *out, int max)
{
   if (memory_id <= 0 || !out || max <= 0)
      return 0;
   db2_memory_scope_tag_row_t rows[16];
   int cap = (int)(sizeof(rows) / sizeof(rows[0]));
   if (cap > max)
      cap = max;
   int got = db2_memory_scopes_list(memory_id, rows, cap);
   for (int i = 0; i < got; i++)
   {
      snprintf(out[i].type, sizeof(out[i].type), "%s", rows[i].type);
      snprintf(out[i].value, sizeof(out[i].value), "%s", rows[i].value);
   }
   if (got == 0)
   {
      snprintf(out[0].type, sizeof(out[0].type), "%s", "global");
      snprintf(out[0].value, sizeof(out[0].value), "%s", "_global");
      return 1;
   }
   return got;
}

memory_scope_level_t memory_primary_scope(int64_t memory_id, char *value, size_t value_len)
{
   if (value && value_len > 0)
      value[0] = '\0';
   if (memory_id <= 0)
      return MEMORY_SCOPE_NONE;
   if (db2_memory_has_scope_type(memory_id, "project"))
   {
      memory_scope_tag_t tags[8];
      int count = memory_collect_scopes(memory_id, tags, 8);
      for (int i = 0; i < count; i++)
      {
         if (strcmp(tags[i].type, "project") == 0)
         {
            if (value && value_len > 0)
               snprintf(value, value_len, "%s", tags[i].value);
            return MEMORY_SCOPE_PROJECT;
         }
      }
   }
   if (db2_memory_has_scope_type(memory_id, "workspace"))
   {
      memory_scope_tag_t tags[8];
      int count = memory_collect_scopes(memory_id, tags, 8);
      for (int i = 0; i < count; i++)
      {
         if (strcmp(tags[i].type, "workspace") == 0)
         {
            if (value && value_len > 0)
               snprintf(value, value_len, "%s", tags[i].value);
            return MEMORY_SCOPE_WORKSPACE;
         }
      }
   }
   if (value && value_len > 0)
      snprintf(value, value_len, "%s", "_global");
   return MEMORY_SCOPE_GLOBAL;
}

int memory_scope_visibility_rank(int64_t memory_id, const char *workspace, const char *project)
{
   if (memory_id <= 0)
      return 0;
   if (project && project[0] && memory_scope_matches(memory_id, "project", project))
      return 3;
   if (workspace && workspace[0] && memory_scope_matches(memory_id, "workspace", workspace))
      return 2;
   if (memory_scope_matches(memory_id, "global", "_global") ||
       memory_scope_matches(memory_id, "workspace", SHARED_WORKSPACE) ||
       !memory_has_any_canonical_scope(memory_id))
      return 1;
   return 0;
}

int memory_tag_global(int64_t memory_id)
{
   return memory_tag_scope(memory_id, "global", "_global");
}

int memory_tag_project(int64_t memory_id, const char *project)
{
   return memory_tag_scope(memory_id, "project", project);
}

int memory_tag_scope(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   if (memory_id <= 0 || !scope_type || !scope_type[0] || !scope_value || !scope_value[0])
      return -1;
   db2_memory_scope_tag_insert(memory_id, scope_type, scope_value);
   if (strcmp(scope_type, "workspace") == 0)
      return memory_tag_workspace(memory_id, scope_value);
   return 0;
}

int memory_tag_workspace(int64_t memory_id, const char *workspace)
{
   if (memory_id <= 0 || !workspace || !workspace[0])
      return -1;
   db2_memory_workspace_tag_insert(memory_id, workspace);
   db2_memory_scope_tag_insert(memory_id, "workspace", workspace);
   return 0;
}

/* Keywords that indicate shared/cross-cutting infrastructure knowledge */
static const char *shared_keywords[] = {
    "network", "deploy",   "auth",    "datastore", "spire",    "proxmox", "cert",     "tls",
    "ssl",     "firewall", "gateway", "dns",       "database", "backup",  "security", NULL};

int memory_auto_tag_workspace(int64_t memory_id, const char *key, const char *content)
{
   if (memory_id <= 0)
      return -1;

   /* Tag with current workspace from cwd */
   char cwd[MAX_PATH_LEN];
   if (getcwd(cwd, sizeof(cwd)))
   {
      config_t cfg;
      config_load(&cfg);
      for (int i = 0; i < cfg.workspace_count; i++)
      {
         size_t wlen = strlen(cfg.workspaces[i]);
         if (wlen > 0 && strncmp(cwd, cfg.workspaces[i], wlen) == 0 &&
             (cwd[wlen] == '/' || cwd[wlen] == '\0'))
         {
            const char *slash = strrchr(cfg.workspaces[i], '/');
            const char *ws_name = slash ? slash + 1 : cfg.workspaces[i];
            memory_tag_workspace(memory_id, ws_name);
            break;
         }
      }
   }

   /* Check for shared keywords in key and content */
   char lower_buf[1024];
   int li = 0;
   const char *texts[] = {key, content, NULL};
   for (int t = 0; texts[t] && li < (int)sizeof(lower_buf) - 1; t++)
   {
      for (int i = 0; texts[t][i] && li < (int)sizeof(lower_buf) - 1; i++)
         lower_buf[li++] = (char)tolower((unsigned char)texts[t][i]);
      lower_buf[li++] = ' ';
   }
   lower_buf[li] = '\0';

   for (int i = 0; shared_keywords[i]; i++)
   {
      if (strstr(lower_buf, shared_keywords[i]))
      {
         memory_tag_workspace(memory_id, SHARED_WORKSPACE);
         return 0;
      }
   }

   return 0;
}

int64_t memory_upsert_workflow(const char *workspace, const char *signal_type, const char *rule,
                               double observed_confidence, const char *session_id)
{
   if (!workspace || !workspace[0] || !signal_type || !signal_type[0] || !rule || !rule[0])
      return -1;

   char key[512];
   snprintf(key, sizeof(key), "workflow:%s:%s", workspace, signal_type);

   /* memory_insert() normalizes keys (lowercase, filler-stripped). Match that
    * here so the confidence-bump lookup finds the existing row. */
   char norm_key[512];
   normalize_key(key, norm_key, sizeof(norm_key));

   /* Look up existing row by exact key so we can bump confidence on repeat
    * observation.  memory_insert() already merges on key, but it uses MAX on
    * confidence — for auto-observed workflows we want to step the confidence
    * up each time, so we apply a bump here before calling insert. */
   double target_conf = observed_confidence > 0.0 ? observed_confidence : 0.6;
   {
      double old_conf = 0.0;
      if (db2_memory_get_confidence_by_key(norm_key, &old_conf))
      {
         /* Step toward 1.0: +0.2 per repeated observation, capped. */
         double bumped = old_conf + 0.2;
         if (bumped > 1.0)
            bumped = 1.0;
         if (bumped > target_conf)
            target_conf = bumped;
      }
   }

   memory_t out;
   int rc = memory_insert(TIER_L1, KIND_WORKFLOW, key, rule, target_conf,
                          session_id ? session_id : "workflow_learning", &out);
   if (rc != 0)
      return -1;

   /* Ensure the row is tagged to this workspace even when cwd-based auto-tag
    * did not match (e.g. fork/exec with a different cwd, or fresh insert on a
    * machine where the workspace name differs from any configured path). */
   memory_tag_workspace(out.id, workspace);

   return out.id;
}

/* --- Embedding Retrieval --- */

double cosine_similarity(const float *a, const float *b, int dim)
{
   double dot = 0.0, na = 0.0, nb = 0.0;
   for (int i = 0; i < dim; i++)
   {
      dot += (double)a[i] * (double)b[i];
      na += (double)a[i] * (double)a[i];
      nb += (double)b[i] * (double)b[i];
   }
   double denom = sqrt(na) * sqrt(nb);
   return denom > 1e-9 ? dot / denom : 0.0;
}

static uint32_t memory_embed_hash_token(const char *tok)
{
   uint32_t h = 2166136261u;
   for (size_t i = 0; tok && tok[i]; i++)
   {
      h ^= (uint32_t)(unsigned char)tok[i];
      h *= 16777619u;
   }
   return h;
}

static void memory_embed_add_feature(float *out, int dim, const char *feature, float mag)
{
   if (!out || dim <= 0 || !feature || !feature[0] || fabsf(mag) < 1e-6f)
      return;
   uint32_t h = memory_embed_hash_token(feature);
   int bucket = (int)(h % (uint32_t)dim);
   float sign = (h & 1u) ? 1.0f : -1.0f;
   out[bucket] += sign * mag;
   if (bucket + 1 < dim)
      out[bucket + 1] += sign * mag * 0.20f;
   if (bucket > 0)
      out[bucket - 1] += sign * mag * 0.10f;
}

static int memory_embed_text_builtin(const char *text, float *out, int max_dim)
{
   if (!text || !out || max_dim <= 0)
      return 0;
   int dim = max_dim < 384 ? max_dim : 384;
   for (int i = 0; i < dim; i++)
      out[i] = 0.0f;

   char norm[4096];
   char tokens[64][64];
   normalize_key(text, norm, sizeof(norm));
   int count = memory_split_tokens(norm, tokens, 64);
   if (count <= 0)
      return 0;

   for (int i = 0; i < count; i++)
   {
      size_t len = strlen(tokens[i]);
      float mag = len >= 8 ? 1.8f : (len >= 5 ? 1.2f : 0.8f);
      if (memory_is_stopword_token(tokens[i]))
         mag *= 0.15f;
      memory_embed_add_feature(out, dim, tokens[i], mag);

      if (i + 1 < count)
      {
         char bigram[144];
         snprintf(bigram, sizeof(bigram), "%s_%s", tokens[i], tokens[i + 1]);
         memory_embed_add_feature(out, dim, bigram, mag * 0.85f);
      }
      if (i + 2 < count && !memory_is_stopword_token(tokens[i + 1]))
      {
         char skipgram[192];
         snprintf(skipgram, sizeof(skipgram), "%s_%s_%s", tokens[i], tokens[i + 1], tokens[i + 2]);
         memory_embed_add_feature(out, dim, skipgram, mag * 0.40f);
      }

      if (len >= 5)
      {
         for (size_t j = 0; j + 2 < len; j++)
         {
            char trigram[8];
            trigram[0] = tokens[i][j];
            trigram[1] = tokens[i][j + 1];
            trigram[2] = tokens[i][j + 2];
            trigram[3] = '\0';
            memory_embed_add_feature(out, dim, trigram, 0.18f);
         }
      }
   }

   double norm_sq = 0.0;
   for (int i = 0; i < dim; i++)
      norm_sq += (double)out[i] * (double)out[i];
   if (norm_sq > 1e-9)
   {
      float scale = (float)(1.0 / sqrt(norm_sq));
      for (int i = 0; i < dim; i++)
         out[i] *= scale;
   }
   return dim;
}

/* Run embedding command: pipes text on stdin, reads JSON float array from stdout. */
int memory_embed_text(const char *text, const char *command, float *out, int max_dim)
{
   if (!text || !out || max_dim <= 0)
      return 0;

   if (!command || !command[0] || strcmp(command, "builtin") == 0)
      return memory_embed_text_builtin(text, out, max_dim);

   char *buf = NULL;
   size_t buf_len = 0;
   int rc = platform_exec_pipe(command, text, strlen(text), &buf, &buf_len);
   if (rc != 0)
   {
      aimee_log(LOG_WARN, "memory", "embedding command failed (exit %d)", rc);
      free(buf);
      return 0;
   }
   if (!buf || buf_len == 0)
   {
      free(buf);
      return 0;
   }

   /* Parse JSON array of floats: [0.1, 0.2, ...] */
   cJSON *arr = cJSON_Parse(buf);
   free(buf);
   if (!arr || !cJSON_IsArray(arr))
   {
      cJSON_Delete(arr);
      aimee_log(LOG_WARN, "memory", "embedding command returned invalid JSON");
      return 0;
   }

   int dim = 0;
   cJSON *el;
   cJSON_ArrayForEach(el, arr)
   {
      if (dim >= max_dim)
         break;
      if (cJSON_IsNumber(el))
         out[dim++] = (float)el->valuedouble;
   }
   cJSON_Delete(arr);
   return dim;
}

/* Embed a single memory and sync it to pgvector. */
int memory_embed(int64_t memory_id, const char *command)
{
   if (memory_id <= 0)
      return -1;

   char key[512];
   char content[2560];
   if (db2_memory_get_key_content(memory_id, key, sizeof(key), content, sizeof(content)) != 0)
      return -1;

   char text[3072];
   snprintf(text, sizeof(text), "%s: %s", key, content);

   float vec[EMBED_MAX_DIM];
   const char *model = (command && command[0]) ? command : "builtin";
   int dim = memory_embed_text(text, model, vec, EMBED_MAX_DIM);
   if (dim <= 0)
      return -1;

   return memory_sync_vec_row(memory_id, vec, dim);
}

/* Deep tier: re-embed one memory with the larger deep model and write it to the
 * halfvec embedding_deep column (the live `embedding` is untouched). Mirrors
 * memory_embed but uses the deep command + a larger buffer. Requires a real
 * command (no builtin fallback — the deep tier is an opt-in external model). */
int memory_embed_deep(int64_t memory_id, const char *command)
{
   if (memory_id <= 0 || !command || !command[0])
      return -1;

   char key[512];
   char content[2560];
   if (db2_memory_get_key_content(memory_id, key, sizeof(key), content, sizeof(content)) != 0)
      return -1;

   char text[3072];
   snprintf(text, sizeof(text), "%s: %s", key, content);

   float vec[DEEP_EMBED_MAX_DIM];
   int dim = memory_embed_text(text, command, vec, DEEP_EMBED_MAX_DIM);
   if (dim <= 0)
      return -1;

   return pgvec_memory_vector_deep_update(memory_id, vec, dim);
}

/* Deep tier backfill: re-embed up to max_rows memories that still lack a deep
 * embedding. Throttled by the caller (one bounded batch per maintenance cycle) so
 * a large corpus fills in over time without blocking. Returns the number of rows
 * successfully deep-embedded; a per-row failure is logged and skipped (it is
 * retried on a later cycle). */
int memory_deep_backfill(const char *command, int max_rows)
{
   if (!command || !command[0] || max_rows <= 0)
      return 0;
   if (max_rows > 1000)
      max_rows = 1000;

   int64_t ids[1000];
   int n = pgvec_memory_vector_deep_candidates(ids, max_rows);
   if (n <= 0)
      return 0;

   int done = 0;
   for (int i = 0; i < n; i++)
   {
      if (memory_embed_deep(ids[i], command) == 0)
         done++;
      else
         aimee_log(LOG_WARN, "memory", "deep backfill skipped memory %lld", (long long)ids[i]);
   }
   return done;
}

/* Deep recall: embed the query with the deep model and search the embedding_deep
 * halfvec index for the highest-quality nearest neighbours. Opt-in / occasional —
 * a deep query embed is slow (~seconds on CPU), so this is never on the default
 * per-turn recall path. Returns the hit count, or -1 on error. */
int memory_deep_search(const char *command, const char *query, int limit, int64_t *ids,
                       double *scores, int max)
{
   if (!command || !command[0] || !query || !query[0] || !ids || !scores || max <= 0)
      return -1;
   float qvec[DEEP_EMBED_MAX_DIM];
   int qdim = memory_embed_text(query, command, qvec, DEEP_EMBED_MAX_DIM);
   if (qdim <= 0)
      return -1;
   return pgvec_memory_vector_deep_search(qvec, qdim, limit, ids, scores, max);
}

/* Deep recall as full memory rows: self-contained entry for the recall handler.
 * Loads config to honour the gate + resolve the deep command (mirrors how the
 * live recall reads embedding_command via config_load), deep-searches, then
 * fetches each hit as a memory_t. Returns the row count, 0 if the deep tier is
 * off / no deep embeddings exist, or -1 on error. */
int memory_deep_search_facts(const char *query, int limit, memory_t *facts, int max)
{
   if (!query || !query[0] || !facts || max <= 0)
      return -1;
   config_t cfg;
   if (config_load(&cfg) != 0)
      return -1;
   if (!cfg.memory_deep_embedding_enabled || !cfg.memory_deep_embedding_command[0])
      return 0; /* deep tier off — caller renders an empty result */

   int cap = max < 64 ? max : 64;
   if (limit <= 0 || limit > cap)
      limit = cap;
   int64_t ids[64];
   double scores[64];
   int n = memory_deep_search(cfg.memory_deep_embedding_command, query, limit, ids, scores, cap);
   if (n < 0)
      return -1;

   int out = 0;
   for (int i = 0; i < n && out < max; i++)
   {
      if (memory_get(ids[i], &facts[out]) == 0)
         out++;
   }
   return out;
}

/* memory_episode_row and memory_relation_row used to live here; the row
 * mappers moved into db2 with db2_memory_episodes_search and
 * db2_memory_relations_search. */

int memory_list_episodes(const char *query, int limit, memory_episode_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   return db2_memory_episodes_search(query, limit, out, max);
}

int memory_get_episode(const char *episode_key, memory_episode_t *out)
{
   if (!episode_key || !episode_key[0] || !out)
      return -1;
   memory_episode_t matches[1];
   if (memory_list_episodes(episode_key, 1, matches, 1) <= 0)
      return -1;
   *out = matches[0];
   return 0;
}

int memory_search_graph(const char *query, int limit, memory_relation_t *out, int max)
{
   return db2_memory_relations_search(query, limit, out, max);
}

int memory_get_entity_profile(const char *entity, memory_entity_profile_t *out)
{
   if (!entity || !entity[0] || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   char norm[128];
   normalize_key(entity, norm, sizeof(norm));
   snprintf(out->entity, sizeof(out->entity), "%s", norm[0] ? norm : entity);
   if (!db2_memory_entity_profile_stats(out->entity, &out->mention_count, &out->relation_count,
                                        out->latest_episode, sizeof(out->latest_episode),
                                        out->summary, sizeof(out->summary)))
      return -1;
   return 0;
}

int memory_get_entity_edges(const char *entity, int limit, memory_relation_t *out, int max)
{
   return db2_memory_relations_for_entity(entity, limit, out, max);
}

char *memory_get_context_block(const char *query, const char *block_type, int limit)
{
   if (!query || !query[0])
      return NULL;
   if (!block_type || !block_type[0] || strcmp(block_type, "general") == 0)
      return memory_assemble_context(query);

   if (limit <= 0)
      limit = 5;
   char *buf = malloc(16384);
   if (!buf)
      return NULL;
   int pos = 0;

   if (strcmp(block_type, "timeline") == 0 || strcmp(block_type, "episode") == 0)
   {
      memory_episode_t episodes[16];
      int count = memory_list_episodes(query, limit < 16 ? limit : 16, episodes, 16);
      pos += snprintf(buf + pos, 16384 - pos, "# %s Context\n",
                      strcmp(block_type, "timeline") == 0 ? "Timeline" : "Episode");
      for (int i = 0; i < count && pos < 15000; i++)
         pos += snprintf(buf + pos, 16384 - pos, "- [%s] %s\n",
                         episodes[i].reference_time[0] ? episodes[i].reference_time
                                                       : episodes[i].source_session,
                         episodes[i].episode_text);
   }
   else if (strcmp(block_type, "entity") == 0)
   {
      memory_entity_profile_t profile;
      memory_relation_t edges[12];
      int edge_count = memory_get_entity_edges(query, limit < 12 ? limit : 12, edges, 12);
      if (memory_get_entity_profile(query, &profile) != 0)
      {
         free(buf);
         return strdup("No entity profile found.");
      }
      pos += snprintf(buf + pos, 16384 - pos, "# Entity Dossier\n%s\nMentions: %d\nRelations: %d\n",
                      profile.entity, profile.mention_count, profile.relation_count);
      if (profile.latest_episode[0])
         pos += snprintf(buf + pos, 16384 - pos, "Latest episode: %s\n", profile.latest_episode);
      if (profile.summary[0])
         pos += snprintf(buf + pos, 16384 - pos, "Summary: %s\n", profile.summary);
      for (int i = 0; i < edge_count && pos < 15000; i++)
         pos += snprintf(buf + pos, 16384 - pos, "- %s [%s] %s\n", edges[i].src_entity,
                         edges[i].relation, edges[i].dst_entity);
   }
   else if (strcmp(block_type, "relationship") == 0)
   {
      memory_relation_t rels[16];
      int count = memory_search_graph(query, limit < 16 ? limit : 16, rels, 16);
      pos += snprintf(buf + pos, 16384 - pos, "# Relationship Summary\n");
      for (int i = 0; i < count && pos < 15000; i++)
         pos += snprintf(buf + pos, 16384 - pos, "- %s [%s] %s (%s)\n", rels[i].src_entity,
                         rels[i].relation, rels[i].dst_entity,
                         rels[i].valid_at[0] ? rels[i].valid_at : "undated");
   }
   else
   {
      free(buf);
      return memory_assemble_context(query);
   }

   if (pos == 0)
      snprintf(buf, 16384, "No context found.");
   return buf;
}
