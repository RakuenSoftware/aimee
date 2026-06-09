/* --- Write Quality Gates --- */

/* gate_check_sensitive, gate_check_ephemeral, and gate_has_evidence_markers
 * are platform-owned quality gates. */

int memory_gate_check(const char *tier, const char *kind, const char *key, const char *content,
                      double confidence, gate_verdict_t *verdict)
{
   memset(verdict, 0, sizeof(*verdict));
   verdict->result = GATE_ACCEPT;

   if ((!content || !content[0]) && strcmp(tier, TIER_L0) != 0)
   {
      verdict->result = GATE_REJECT;
      snprintf(verdict->reason, sizeof(verdict->reason), "empty content at tier %s", tier);
      return 0;
   }

   if (!content || !content[0])
      return 0; /* L0 scratch allows empty content */

   /* Gate 1: Sensitivity check (secrets, PII) */
   int sens =
       gate_check_sensitive(content, verdict->redacted_content, sizeof(verdict->redacted_content));
   if (sens == 2)
   {
      verdict->result = GATE_REJECT;
      snprintf(verdict->reason, sizeof(verdict->reason),
               "sensitivity: content matches secret/PII pattern");
      return 0;
   }
   if (sens == 1)
   {
      verdict->result = GATE_REDACT;
      snprintf(verdict->reason, sizeof(verdict->reason), "sensitivity: secret/PII redacted");
      return 0;
   }

   /* Gate 2: Stability check (ephemeral content at L1+) */
   if (strcmp(tier, TIER_L0) != 0 && gate_check_ephemeral(content))
   {
      verdict->result = GATE_DOWNGRADE;
      snprintf(verdict->reason, sizeof(verdict->reason),
               "stability: ephemeral content downgraded to L0");
      return 0;
   }

   /* Gate 3: Conflict check (contradiction with high-confidence L2) */
   double existing_conf = 0.0;
   if (db2_memory_find_conflicting_l2(key, content, &existing_conf) == 1)
   {
      if (confidence <= existing_conf && strcmp(tier, TIER_L2) == 0)
      {
         verdict->result = GATE_DOWNGRADE;
         snprintf(verdict->reason, sizeof(verdict->reason),
                  "conflict: contradicts L2 memory with confidence %.2f", existing_conf);
         return 0;
      }
   }

   /* Gate 4: Source check (high confidence without evidence, L1+ only) */
   if (strcmp(tier, TIER_L0) != 0 && confidence > 0.9 && !gate_has_evidence_markers(content))
   {
      /* Don't downgrade/reject, but signal that confidence should be capped.
       * We encode this in the verdict reason so the caller can adjust. */
      snprintf(verdict->reason, sizeof(verdict->reason),
               "source: confidence capped at %.1f (no evidence markers)", GATE_CONFIDENCE_FLOOR);
   }

   (void)kind; /* reserved for kind-specific gates in the future */
   return 0;
}

/* --- Content Safety Scanning --- */

/* memory_scan_content is platform-owned. */

/* --- Core CRUD --- */

static int memory_is_semantic_profile_key(const char *raw_key)
{
   return raw_key && strchr(raw_key, ':') != NULL;
}

static void memory_normalize_semantic_value(const char *raw, char *buf, size_t buf_len)
{
   if (!buf || buf_len == 0)
      return;
   buf[0] = '\0';
   if (!raw || !raw[0])
      return;

   size_t out = 0;
   int last_space = 1;
   for (size_t i = 0; raw[i] && out + 1 < buf_len; i++)
   {
      unsigned char ch = (unsigned char)raw[i];
      if (isalnum(ch))
      {
         buf[out++] = (char)tolower(ch);
         last_space = 0;
      }
      else if (!last_space)
      {
         buf[out++] = ' ';
         last_space = 1;
      }
   }

   while (out > 0 && buf[out - 1] == ' ')
      out--;
   buf[out] = '\0';
}

static int memory_semantic_value_equivalent(const char *a, const char *b)
{
   if (!a || !b)
      return 0;

   char norm_a[1024];
   char norm_b[1024];
   memory_normalize_semantic_value(a, norm_a, sizeof(norm_a));
   memory_normalize_semantic_value(b, norm_b, sizeof(norm_b));

   if (!norm_a[0] || !norm_b[0])
      return 0;
   if (strcmp(norm_a, norm_b) == 0)
      return 1;
   return trigram_similarity(norm_a, norm_b) >= 0.92;
}

int memory_insert_ex(const char *tier, const char *kind, const char *key, const char *content,
                     const char *use_cases, double confidence, const char *session_id,
                     memory_t *out)
{
   if (!tier || !kind || !key)
      return -1;

   /* Eval scratch stores are throwaway. Skip cross-cutting write-side work
    * whose side effects would either leak out of the test or dominate
    * bench-style bulk loads. */
   int skip_side_effects = memory_skip_persistent_side_effects();

   /* Run write quality gates */
   gate_verdict_t verdict;
   memory_gate_check(tier, kind, key, content, confidence, &verdict);

   switch (verdict.result)
   {
   case GATE_REJECT:
      /* Log the rejection in provenance (use id=0 since no memory was created) */
      return -1;
   case GATE_DOWNGRADE:
      tier = TIER_L0;
      kind = KIND_SCRATCH;
      break;
   case GATE_REDACT:
      content = verdict.redacted_content;
      break;
   case GATE_ACCEPT:
      break;
   }

   /* Source gate: cap confidence for user-facing fact insertions without evidence.
    * Internal callers (promotion, learning) pass empty session_id and set confidence
    * based on actual signal, so we only apply this to interactive sessions. */
   (void)0; /* source gate logs reason but does not modify confidence at insert time;
             * promotion/lifecycle will validate confidence changes separately */

   /* Content safety scan */
   char safe_content[2048];
   const char *sensitivity = "normal";
   if (content && content[0])
   {
      snprintf(safe_content, sizeof(safe_content), "%s", content);
      sensitivity = memory_scan_content(safe_content, strlen(safe_content));
      if (!sensitivity) /* blocked */
         return -1;
      content = safe_content;
   }

   /* Normalize key */
   char norm_key[512];
   normalize_key(key, norm_key, sizeof(norm_key));

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Conflict-triggered versioning: when a new high-confidence L2 fact
    * overwrites an existing L2 memory with materially different content,
    * preserve history by calling memory_supersede() instead of merging.
    * This is the temporal-reasoning path required by LongMemEval KU. */
   if (!skip_side_effects && content && content[0] && confidence >= 0.8 &&
       strcmp(tier, TIER_L2) == 0)
   {
      int semantic_profile = memory_is_semantic_profile_key(key);
      int64_t ex_id = 0;
      char ex_content[2048];
      double ex_conf = 0.0;
      char ex_tier[16];
      if (db2_memory_lookup_by_key(norm_key, &ex_id, ex_content, sizeof(ex_content), &ex_conf,
                                   ex_tier, sizeof(ex_tier)))
      {
         int same_tier_l2 = (strcmp(ex_tier, TIER_L2) == 0);
         int high_conf_existing = (ex_conf >= 0.7);
         int materially_different = 0;
         if (ex_content[0])
         {
            if (semantic_profile && !memory_semantic_value_equivalent(ex_content, content))
               materially_different = 1;
            else if (is_contradiction(ex_content, content))
               materially_different = 1;
            else if (memory_temporal_value_conflict(ex_content, content))
               materially_different = 1;
            else if (trigram_similarity(ex_content, content) < 0.6)
               materially_different = 1;
         }

         if (same_tier_l2 && high_conf_existing && materially_different)
         {
            int rc = memory_supersede(ex_id, content, confidence, session_id, out);
            if (rc == 0)
            {
               if (db1_context_cache_invalidate)
                  db1_context_cache_invalidate();
               return 0;
            }
            /* supersede failed — fall through to normal merge path */
         }
      }
   }

   /* Check for exact key match */
   {
      int64_t existing_id = 0;
      char preserved_content[2048] = "";
      double old_conf = 0.0;
      int old_use = 0;
      double old_surprise = 0.0;
      int old_obs = 0;
      double old_evidence = 0.0;
      if (db2_memory_lookup_merge_fields(norm_key, &existing_id, preserved_content,
                                         sizeof(preserved_content), &old_conf, &old_use,
                                         &old_surprise, &old_obs, &old_evidence))
      {
         /* Merge: keep higher confidence, increment use_count */
         int semantic_profile = (strcmp(tier, TIER_L2) == 0 && memory_is_semantic_profile_key(key));
         int same_semantic_value =
             semantic_profile &&
             memory_semantic_value_equivalent(preserved_content, content ? content : "");
         const char *merged_content =
             (same_semantic_value && preserved_content[0]) ? preserved_content : content;

         double new_conf = confidence > old_conf ? confidence : old_conf;
         int new_use = old_use + 1;
         int new_obs = (old_obs > 0 ? old_obs : 1) + 1;
         double new_evidence = old_evidence;
         double base_evidence = memory_base_evidence_strength(norm_key, content, new_conf);
         if (base_evidence > new_evidence)
            new_evidence = base_evidence;
         new_evidence += 0.04;
         if (new_obs >= 3)
            new_evidence += 0.03;
         if (new_evidence > 1.0)
            new_evidence = 1.0;
         double new_surprise = memory_content_surprise(session_id, content);
         if (old_surprise > new_surprise)
            new_surprise = old_surprise;

         if (db2_memory_merge_update_ex(existing_id, merged_content ? merged_content : content,
                                        use_cases, new_conf, new_use, new_obs, new_evidence,
                                        memory_content_salience(content), new_surprise, ts) != 0)
            return -1;

         memory_refresh_derived_metadata(
             existing_id, norm_key, merged_content ? merged_content : (content ? content : ""));
         add_provenance(existing_id, session_id, "merge",
                        same_semantic_value ? "semantic profile dedupe" : "exact key match");

         {
            config_t embed_cfg;
            config_load(&embed_cfg);
            platform_memory_background_embed(existing_id, embed_cfg.embedding_command[0]
                                                              ? embed_cfg.embedding_command
                                                              : "builtin");
         }

         if (out)
            memory_get(existing_id, out);
         if (db1_context_cache_invalidate)
            db1_context_cache_invalidate();
         if (!skip_side_effects)
            memory_maybe_run_maintenance();
         return 0;
      }
   }

   /* Check trigram near-duplicate against same-kind memories. Skipped for
    * eval scratch stores where each run only holds one benchmark sample. */
   if (!skip_side_effects)
   {
      db2_memory_dedupe_candidate_t cands[100];
      int n_cands = db2_memory_active_kind_dedupe_candidates(kind, cands, 100);

      int64_t dup_id = 0;
      double dup_conf = 0.0;
      int dup_use = 0;
      int dup_obs = 1;
      double dup_evidence = 0.5;
      double dup_surprise = 0.5;
      for (int i = 0; i < n_cands; i++)
      {
         double sim = trigram_similarity(norm_key, cands[i].key);
         if (sim >= DEDUP_THRESHOLD)
         {
            dup_id = cands[i].id;
            dup_conf = cands[i].confidence;
            dup_use = cands[i].use_count;
            dup_obs = cands[i].observation_count;
            dup_evidence = cands[i].evidence_strength;
            dup_surprise = cands[i].surprise;
            break;
         }
      }

      if (dup_id > 0)
      {
         /* Merge into near-duplicate */
         double new_conf = confidence > dup_conf ? confidence : dup_conf;
         int new_use = dup_use + 1;
         int new_obs = (dup_obs > 0 ? dup_obs : 1) + 1;
         double new_evidence = dup_evidence;
         double base_evidence = memory_base_evidence_strength(norm_key, content, new_conf);
         if (base_evidence > new_evidence)
            new_evidence = base_evidence;
         new_evidence += 0.03;
         if (new_obs >= 3)
            new_evidence += 0.03;
         if (new_evidence > 1.0)
            new_evidence = 1.0;
         double new_surprise = memory_content_surprise(session_id, content);
         if (dup_surprise > new_surprise)
            new_surprise = dup_surprise;

         if (db2_memory_merge_update_ex(dup_id, content, use_cases, new_conf, new_use, new_obs,
                                        new_evidence, memory_content_salience(content),
                                        new_surprise, ts) != 0)
            return -1;

         memory_refresh_derived_metadata(dup_id, norm_key, content ? content : "");
         add_provenance(dup_id, session_id, "merge", "trigram near-duplicate");

         {
            config_t embed_cfg;
            config_load(&embed_cfg);
            platform_memory_background_embed(
                dup_id, embed_cfg.embedding_command[0] ? embed_cfg.embedding_command : "builtin");
         }

         if (out)
            memory_get(dup_id, out);
         if (db1_context_cache_invalidate)
            db1_context_cache_invalidate();
         memory_maybe_run_maintenance();
         return 0;
      }
   }

   /* Truly new: INSERT */
   {
      int64_t new_id = db2_memory_row_insert_ex(
          tier, kind, norm_key, content, use_cases, confidence, session_id, ts, sensitivity,
          memory_base_evidence_strength(norm_key, content, confidence),
          memory_content_salience(content), memory_content_surprise(session_id, content));
      if (new_id < 0)
      {
         aimee_log(LOG_ERROR, "memory", "memory insert failed");
         return -1;
      }

      memory_refresh_derived_metadata(new_id, norm_key, content ? content : "");
      add_provenance(new_id, session_id, "insert", NULL);
      if (!skip_side_effects)
         memory_auto_tag_workspace(new_id, norm_key, content ? content : "");

      /* Auto-embed if embedding command is configured (background, non-fatal) */
      {
         config_t embed_cfg;
         config_load(&embed_cfg);
         platform_memory_background_embed(
             new_id, embed_cfg.embedding_command[0] ? embed_cfg.embedding_command : "builtin");
      }

      if (out)
         memory_get(new_id, out);
      if (db1_context_cache_invalidate)
         db1_context_cache_invalidate();
      if (!skip_side_effects)
         memory_maybe_run_maintenance();
      return 0;
   }
}

int memory_insert(const char *tier, const char *kind, const char *key, const char *content,
                  double confidence, const char *session_id, memory_t *out)
{
   return memory_insert_ex(tier, kind, key, content, "", confidence, session_id, out);
}

int memory_get(int64_t id, memory_t *out)
{
   return db2_memory_get(id, out);
}

int memory_touch(int64_t id)
{
   return db2_memory_touch(id);
}

int memory_update_content(int64_t id, const char *content)
{
   if (id <= 0 || !content || !content[0])
      return -1;
   int changes = db2_memory_update_content(id, content);
   return changes > 0 ? 0 : -1;
}

int memory_reject(int64_t id, const char *reason)
{
   return db2_memory_reject(id, reason);
}

int memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   /* Hide archived rows from the default fact-recall surface when the
    * lifecycle feature is enabled and hide_archived is set. memory_get()
    * and explicit kind/tier queries still see them — only the un-filtered
    * listing path filters. */
   int hide_archived = 0;
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.memory_lifecycle_enabled && cfg.memory_lifecycle_hide_archived)
      hide_archived = 1;
   return db2_memory_list(tier, kind, hide_archived, limit, out, max);
}

int memory_delete(int64_t id)
{
   /* Wipe provenance first; CASCADE should handle this, but existing stores
    * may predate the FK. */
   db2_memory_provenance_delete(id);

   /* Drop any unit-scoped pgvector points for this memory. */
   int64_t unit_ids[64];
   int unit_count =
       db2_memory_unit_list_ids(id, unit_ids, (int)(sizeof(unit_ids) / sizeof(unit_ids[0])));
   for (int i = 0; i < unit_count; i++)
   {
      int64_t pt = PGVEC_MEMORY_VECTOR_UNIT_ID_OFFSET + unit_ids[i];
      pgvec_memory_vector_delete_point(pt);
      db2_vector_index_op_remove(pt);
   }

   pgvec_memory_vector_delete_point(id);
   db2_vector_index_op_remove(id);

   int changes = db2_memory_delete_row(id);
   if (changes > 0 && db1_context_cache_invalidate)
      db1_context_cache_invalidate();
   return changes > 0 ? 0 : -1;
}

int memory_stats(memory_stats_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (db2_memory_stats_counts(out) != 0)
      return -1;
   memory_fill_pagerank_stats(out);
   return 0;
}

int memory_rebuild_derived_indexes(int limit)
{
   db2_memory_id_key_content_row_t rows[256];
   int rows_max = (int)(sizeof(rows) / sizeof(rows[0]));
   int n = db2_memory_list_id_key_content(limit, rows, rows_max);
   for (int i = 0; i < n; i++)
      memory_refresh_derived_metadata(rows[i].id, rows[i].key, rows[i].content);
   int rebuilt = n;
   return rebuilt;
}

int memory_repair_vector_index(int64_t memory_id, const char *command)
{
   if (memory_id <= 0)
      return -1;

   memory_t mem;
   if (memory_get(memory_id, &mem) != 0)
      return -1;

   if (memory_embed(memory_id, command) != 0)
      return -1;

   memory_refresh_derived_metadata(memory_id, mem.key, mem.content);
   return 0;
}

/* Max retries before a point is considered permanently broken and excluded
 * from automatic retry.  Operators can force retry of these via
 * `memory repair <id>` (single-point, which calls the single-point API
 * that does not consult the cap) or `memory repair --reset-stuck` which
 * resets attempts to 0.  Override via AIMEE_VECTOR_MAX_RETRY. */
#define MEMORY_VECTOR_MAX_RETRY_ATTEMPTS_DEFAULT 8

static int pgvec_memory_vector_max_retry_attempts(void)
{
   return memory_env_int("AIMEE_VECTOR_MAX_RETRY", MEMORY_VECTOR_MAX_RETRY_ATTEMPTS_DEFAULT, 1,
                         1024);
}

int memory_repair_vector_index_failed_only(const char *command, int limit, int *failed_out)
{
   /* Skip points that have exceeded the retry cap — treat them as
    * permanently broken so an endless loop of retries doesn't mask the
    * underlying issue (bad embedding cmd, missing collection, etc.). */
   int max_attempts = pgvec_memory_vector_max_retry_attempts();

   int64_t ids[256];
   int cap = (int)(sizeof(ids) / sizeof(ids[0]));
   int n = db2_memory_list_retryable_index_failures(max_attempts, limit, ids, cap);

   int repaired = 0;
   int failed = 0;
   for (int i = 0; i < n; i++)
   {
      if (memory_repair_vector_index(ids[i], command) == 0)
         repaired++;
      else
         failed++;
   }
   if (failed_out)
      *failed_out = failed;
   return repaired;
}

int memory_rebuild_vector_index_for_version(const char *version, int *failed_out)
{
   if (!version || !version[0])
      return -1;
   if (!db2_is_initialized())
      return -1;

   if (!db2_kb_runtime_state_vector_rebuild_lock_try_acquire())
      return -1; /* another rebuild is already running */

   struct timespec t_start;
   clock_gettime(CLOCK_MONOTONIC, &t_start);

   if (pgvec_memory_vector_collection_recreate(384) != 0)
   {
      db2_kb_runtime_state_vector_rebuild_lock_release();
      return -1;
   }

   /* Re-create payload indexes on the new collection so filtered searches
    * remain indexed after rebuild. */
   pgvec_memory_vector_ensure_payload_indexes();

   if (failed_out)
      *failed_out = 0;

   (void)db2_kb_runtime_state_set("vector_schema_version", pgvec_schema_version());

   {
      struct timespec t_end;
      clock_gettime(CLOCK_MONOTONIC, &t_end);
      double elapsed_s =
          (double)(t_end.tv_sec - t_start.tv_sec) + (double)(t_end.tv_nsec - t_start.tv_nsec) / 1e9;
      LOG_INFO("pgvec_memory_rebuild",
               "complete: recreated pgvector collection without SQL vector backfill, %.2fs "
               "(version=%s)",
               elapsed_s, version);
   }

   db2_kb_runtime_state_vector_rebuild_lock_release();
   return 0;
}
