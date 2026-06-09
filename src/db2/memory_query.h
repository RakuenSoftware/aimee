/* db2/memory_query.h: read-only SELECT primitives over memories +
 * memory_units. DB2-owned.
 *
 * memory_t lives in headers/memory.h; callers must include "aimee.h"
 * before this header for the type to resolve. */
#ifndef DEC_DB2_MEMORY_QUERY_H
#define DEC_DB2_MEMORY_QUERY_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Fetch memory rows whose key matches `normalized_key` exactly OR
    * matches `normalized_key#v%` (versioned siblings), most recent
    * first. Caller must pass the already-normalized key — db2 does not
    * touch normalize_key. Returns count written. */
   int db2_memory_fact_history(const char *normalized_key, memory_t *out, int max);

   /* Fetch up to `max` episode-card content strings for a session.
    * Each output slot receives a strdup'd content string (or "" on
    * NULL). Caller frees. Returns count written. */
   int db2_memory_episode_cards_query(const char *source_session, char **out, int max);

   /* Search L1+L2 memories whose key OR content matches `pattern`
    * (caller supplies the SQL LIKE pattern, e.g. "%foo%"). Limit 10
    * rows in SQL; orchestrator handles dedup + seed-skip + scoring. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
   } db2_memory_search_match_t;

   int db2_memory_search_by_pattern(const char *pattern, db2_memory_search_match_t *out, int max);

   /* LIKE-based fact lookup over memories.{key,content}, used as the last-resort
    * fallback in the search pipeline when lexical / semantic / alias / entity all
    * miss. Returns up to min(limit, max) rows ranked by exact-key, exact-content,
    * key-prefix, then tier (L3>L2>L1) and use_count. */
   int db2_memory_find_facts_like(const char *query, int limit, memory_t *out, int max);

   /* List memory ids whose vector indexing failed but remains retryable.
    * Vector index status storage is DB2-internal; memory repair callers use
    * this domain helper instead of importing that table API directly. */
   int db2_memory_list_retryable_index_failures(int max_attempts, int limit, int64_t *out, int max);

   /* Collect memory rows whose memory_aliases.alias matches `alias` exactly,
    * as a prefix, or as an infix (in priority order). Up to min(limit, max)
    * rows. Caller does any de-dup against an existing result set. */
   int db2_memory_collect_alias_matches(const char *alias, int limit, memory_t *out, int max);

   /* Collect memory rows whose memory_entities.entity matches `term` exactly,
    * as a prefix, or as an infix (in priority order). Up to min(limit, max)
    * rows. Caller does any de-dup against an existing result set. */
   int db2_memory_collect_entity_matches(const char *term, int limit, memory_t *out, int max);

   /* Collect memory rows joined to memory_temporal_refs whose ref_key matches
    * `term` exactly or as a prefix. Up to min(limit, max) rows. */
   int db2_memory_collect_temporal_matches(const char *term, int limit, memory_t *out, int max);

   /* Collect memory rows joined to memory_summaries where summary contains
    * `term` (case-insensitive). Up to min(limit, max) rows. */
   int db2_memory_collect_summary_matches(const char *term, int limit, memory_t *out, int max);

   /* Collect memory rows joined to memory_event_frames where any of
    * actor/action/object/location/event_time matches `term`. Up to
    * min(limit, max) rows. */
   int db2_memory_collect_event_frame_matches(const char *term, int limit, memory_t *out, int max);

   /* db2_memory_collect_chunk_matches moved out of the db2 surface: chunk-
    * level recall now falls back to pgvector memory-level semantic search
    * until chunks get their own vector entries. memory_core_search.inc still
    * exposes memory_collect_chunk_matches as the wrapper that tags hits
    * with MEM_SOURCE_CHUNK. */

   /* db2_memory_collect_unit_matches moved out of the db2 surface: unit-level
    * memory recall now routes through pgvector
    * (memory_core_search.inc::memory_collect_unit_matches_via_vector). The ≥2
    * unit-hits-per-memory corroboration filter and the hit_count / best_score
    * sort are reproduced client-side over the vector search results. */

   /* Read memories.evidence_strength and observation_count for the given id.
    * Returns 1 on hit (and writes to *evidence and *observations), 0 on miss
    * (outputs unchanged). Used by the evidence-bonus scorer. */
   int db2_memory_get_evidence_fields(int64_t memory_id, double *evidence, int *observations);

   /* Read memories.salience for the given id. Returns the row value on hit,
    * or `default_value` when the row is missing. */
   double db2_memory_get_salience(int64_t memory_id, double default_value);

   /* Read memories.surprise for the given id. Returns the row value on hit,
    * or `default_value` when the row is missing. */
   double db2_memory_get_surprise(int64_t memory_id, double default_value);

   /* Fields used by the state-penalty scorer in one shot.
    *  - has_valid_until: 1 iff valid_until is non-NULL and non-empty.
    *  - observations:    memories.observation_count.
    *  - use_count:       memories.use_count.
    * Returns 1 on hit, 0 on miss (outputs unchanged). */
   int db2_memory_get_state_fields(int64_t memory_id, int *has_valid_until, int *observations,
                                   int *use_count);

   /* Returns 1 iff memory_unit_edges has an edge in either direction between
    * unit_id_a and unit_id_b, 0 otherwise. */
   int db2_memory_unit_edge_exists(int64_t unit_id_a, int64_t unit_id_b);

   /* Returns 1 iff memory_scopes has a row matching (memory_id, scope_type,
    * scope_value), 0 otherwise. Used for scope filtering during search. */
   int db2_memory_scope_matches(int64_t memory_id, const char *scope_type, const char *scope_value);

   /* Returns 1 iff memory_workspaces has a row matching (memory_id, workspace),
    * 0 otherwise. Legacy fallback for workspace-scope filtering. */
   int db2_memory_workspace_matches(int64_t memory_id, const char *workspace);

   /* Returns 1 iff memory_scopes has any row for `memory_id` with the given
    * `scope_type` (e.g. 'global', 'workspace', 'project'), 0 otherwise. */
   int db2_memory_has_scope_type(int64_t memory_id, const char *scope_type);

   /* Returns 1 iff memory_workspaces has any row for `memory_id`, 0 otherwise.
    * Used as the legacy-fallback companion to db2_memory_has_scope_type when
    * deciding whether a memory has any canonical workspace tag at all. */
   int db2_memory_has_any_workspace_tag(int64_t memory_id);

   /* (scope_type, scope_value) pair from memory_scopes for one memory.
    * Returned ordered: project, workspace, global, then anything else; values
    * sorted ASC within each type. */
   typedef struct
   {
      char type[16];
      char value[128];
   } db2_memory_scope_tag_row_t;

   /* List up to `max` (scope_type, scope_value) rows for memory_id, in the
    * priority order described above. Returns count written. */
   int db2_memory_scopes_list(int64_t memory_id, db2_memory_scope_tag_row_t *out, int max);

   /* INSERT OR IGNORE into memory_scopes. Best-effort. */
   void db2_memory_scope_tag_insert(int64_t memory_id, const char *scope_type,
                                    const char *scope_value);

   /* INSERT OR IGNORE into memory_workspaces. Best-effort. */
   void db2_memory_workspace_tag_insert(int64_t memory_id, const char *workspace);

   /* List memory_episodes rows whose key/text/source_session contains `query`
    * (or all rows when `query` is empty). Ordered by exact-key match,
    * has-reference-time, then created_at DESC. Up to min(limit, max) rows.
    * memory_episode_t lives in headers/memory.h. */
   int db2_memory_episodes_search(const char *query, int limit, memory_episode_t *out, int max);

   /* Look up memories.confidence for the row whose key matches exactly.
    * Returns 1 on hit (writes *confidence_out), 0 on miss. */
   int db2_memory_get_confidence_by_key(const char *key, double *confidence_out);

   /* (id, content) pair returned by db2_memory_list_by_key. */
   typedef struct
   {
      int64_t id;
      char content[2048];
   } db2_memory_id_content_row_t;

   /* List up to `max` (id, content) rows where memories.key = `key` exactly
    * (no normalization). Used by callers that need to scan all duplicates of
    * a key to look for contradictions. Returns count written. */
   int db2_memory_list_by_key(const char *key, db2_memory_id_content_row_t *out, int max);

   /* (key, content) pair returned by db2_memory_session_l0_list. */
   typedef struct
   {
      char key[512];
      char content[2048];
   } db2_memory_session_l0_row_t;

   /* List the L0 memories for one session, ordered created_at ASC. Used by
    * the session-fold checkpoint builder. Returns count written. */
   int db2_memory_session_l0_list(const char *session_id, db2_memory_session_l0_row_t *out,
                                  int max);

   /* (id, content) pair from `memories` for one session, in id-asc order.
    * Used by the episode-card cognifier to assemble the conversation feed. */
   int db2_memory_session_id_content_list(const char *session_id, int limit,
                                          db2_memory_id_content_row_t *out, int max);

   /* UPDATE memories SET source_session = ? WHERE id = ?. Best-effort. */
   void db2_memory_set_source_session(int64_t memory_id, const char *session_id);

   /* INSERT OR IGNORE a memory_unit row for an episode card. The hardcoded
    * shape: unit_type='episode_card', memory_kind='episodic', weight=2.0,
    * is_episode_card=1. Best-effort. */
   void db2_memory_unit_episode_card_insert(int64_t memory_id, const char *unit_key,
                                            const char *unit_text);

   /* SELECT id FROM memory_units WHERE memory_id = ? AND unit_type =
    * 'episode_card' LIMIT 1. Returns the unit id on hit, 0 on miss. */
   int64_t db2_memory_unit_first_episode_card_id(int64_t memory_id);

   /* Look up (id, content, confidence, tier) for the row whose key matches
    * exactly. Used by memory_insert's conflict-versioning probe. Returns 1
    * on hit, 0 on miss. The text outputs are NUL-terminated and truncated
    * to fit. */
   int db2_memory_lookup_by_key(const char *key, int64_t *id_out, char *content_out,
                                int content_len, double *confidence_out, char *tier_out,
                                int tier_len);

   /* Merge-on-key probe: reads the fields memory_insert needs for the
    * exact-key merge path. Returns 1 on hit (writes all outputs), 0 on miss.
    * `content_out` is truncated to `content_len`. */
   int db2_memory_lookup_merge_fields(const char *key, int64_t *id_out, char *content_out,
                                      int content_len, double *confidence_out, int *use_count_out,
                                      double *surprise_out, int *observation_count_out,
                                      double *evidence_out);

   /* Row shape for db2_memory_active_kind_dedupe_candidates: live (no
    * valid_until), non-versioned memories of one kind. Caller runs trigram
    * comparison against `key` to pick a near-duplicate. */
   typedef struct
   {
      int64_t id;
      char key[512];
      double confidence;
      int use_count;
      int observation_count;
      double evidence_strength;
      double surprise;
   } db2_memory_dedupe_candidate_t;

   /* Fetch up to `max` (LIMIT 100 in SQL) live, non-versioned memories with
    * `kind`. Used by memory_insert's trigram-near-dup probe. Returns count
    * written. */
   int db2_memory_active_kind_dedupe_candidates(const char *kind,
                                                db2_memory_dedupe_candidate_t *out, int max);

   /* UPDATE the standard merge fields: content, confidence, use_count,
    * observation_count, evidence_strength, salience, surprise,
    * last_used_at, updated_at WHERE id = ?. Best-effort. Returns 0 on
    * success, -1 on SQL error. */
   int db2_memory_merge_update(int64_t memory_id, const char *content, double confidence,
                               int use_count, int observation_count, double evidence_strength,
                               double salience, double surprise, const char *ts);

   /* INSERT a new memories row with the truly-new defaults (use_count=1,
    * observation_count=1) plus the caller-supplied tier/kind/key/content/
    * confidence/sensitivity/evidence/salience/surprise. `ts` is used for
    * last_used_at, created_at, updated_at. Returns the new rowid on
    * success, -1 on SQL error. */
   int64_t db2_memory_row_insert(const char *tier, const char *kind, const char *key,
                                 const char *content, double confidence, const char *session_id,
                                 const char *ts, const char *sensitivity, double evidence_strength,
                                 double salience, double surprise);

   /* Returns the count of memories whose key matches `key_prefix#v%`. Used by
    * supersede to derive the next version number. */
   int db2_memory_count_versions(const char *key_prefix);

   /* Rename a memory's key to `versioned_key`, stamp valid_until = ts, and
    * stamp updated_at = ts. Returns 0 on success, -1 on SQL error. */
   int db2_memory_set_versioned_key(int64_t memory_id, const char *versioned_key, const char *ts);

   /* UPDATE memories SET valid_from = ? WHERE id = ?. Best-effort. */
   void db2_memory_set_valid_from(int64_t memory_id, const char *ts);

   /* Row shape for db2_memory_list_low_effectiveness. */
   typedef struct
   {
      int64_t id;
      char tier[4];
      char kind[16];
      char key[512];
      double effectiveness;
      int use_count;
   } db2_memory_low_eff_row_t;

   /* List memories with effectiveness < threshold, ordered ascending. Returns
    * the count written, or 0 on error. */
   int db2_memory_list_low_effectiveness(double threshold, int limit,
                                         db2_memory_low_eff_row_t *rows, int max);

   /* Row shape for db2_memory_list_unused_l2. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char tier[4];
      char kind[16];
      double confidence;
   } db2_memory_unused_l2_row_t;

   /* List L2 memories with use_count = 0 created more than `days` ago, ordered
    * by created_at ascending. Returns the count written. */
   int db2_memory_list_unused_l2(int days, db2_memory_unused_l2_row_t *rows, int max);

   /* Row shape for db2_memory_list_superseded_keys. */
   typedef struct
   {
      char base_key[512];
      int versions;
   } db2_memory_superseded_row_t;

   /* List base keys with `min_versions` or more #v variants, ordered by
    * version count descending. Returns the count written. */
   int db2_memory_list_superseded_keys(int min_versions, db2_memory_superseded_row_t *rows,
                                       int max);

   /* UPDATE memories SET artifact_type, artifact_ref, artifact_hash WHERE id=?.
    * `hash` may be NULL. Returns 1 if a row was updated, 0 if not (no such id
    * or SQL error). */
   int db2_memory_set_artifact(int64_t memory_id, const char *artifact_type,
                               const char *artifact_ref, const char *artifact_hash);

   /* UPDATE memories SET cognified_memory_kind = ? WHERE id = ?. Best-effort. */
   void db2_memory_set_cognified_kind(int64_t memory_id, const char *kind);

   /* SELECT content FROM memories WHERE id = ?. Writes up to `out_len` bytes
    * (NUL-terminated) into `out`. Returns 1 on hit, 0 on miss / SQL error. */
   int db2_memory_get_content(int64_t memory_id, char *out, int out_len);

   /* Row shape for db2_memory_summarise_clusters. */
   typedef struct
   {
      char session_id[128];
      int count;
      double avg_confidence;
   } db2_memory_summary_cluster_t;

   /* Group L1 fact memories by source_session where confidence<=max_confidence
    * and merged_into=0, returning sessions with at least `min_count` rows.
    * Returns the number of clusters written. */
   int db2_memory_summarise_clusters(double max_confidence, int min_count,
                                     db2_memory_summary_cluster_t *rows, int max);

   /* UPDATE memories SET merged_into = ? WHERE tier='L1' AND kind='fact'
    * AND merged_into=0 AND source_session=? AND confidence<=?. Best-effort. */
   void db2_memory_mark_merged_into(int64_t merged_into, const char *session_id,
                                    double max_confidence);

   /* Row shape for db2_memory_list_artifact_hashed. */
   typedef struct
   {
      int64_t id;
      char artifact_type[32];
      char artifact_ref[256];
      char artifact_hash[65];
   } db2_memory_artifact_row_t;

   /* List up to `max` (capped at 50) memories that have artifact_type and
    * artifact_hash both non-NULL. Returns the count written. */
   int db2_memory_list_artifact_hashed(db2_memory_artifact_row_t *rows, int max);

   /* UPDATE memories SET confidence = confidence * 0.7 WHERE id = ?. Used by
    * the artifact-staleness reducer when a file's recomputed hash diverges
    * from the recorded one. Best-effort. */
   void db2_memory_decay_confidence(int64_t memory_id);

   /* Row shape for db2_memory_list_kv_section. */
   typedef struct
   {
      char key[512];
      char content[2048];
   } db2_memory_kv_row_t;

   /* Sections used by memory_assemble_context's static-section path. */
   typedef enum
   {
      DB2_MEM_SECTION_ACTIVE_TASKS = 1,
      DB2_MEM_SECTION_RECENT_CONTEXT,
      DB2_MEM_SECTION_CONSTRAINTS,
      DB2_MEM_SECTION_PROCEDURES,
      DB2_MEM_SECTION_FAILURE_WARNINGS,
   } db2_memory_section_t;

   /* Fetch (key, content) tuples for a fixed context section. Returns the
    * count written. */
   int db2_memory_list_kv_section(db2_memory_section_t section, db2_memory_kv_row_t *rows, int max);

   /* List up to `max` (capped at 4) episode-card content strings, ordered by
    * memories.confidence DESC, memories.id DESC. Each row gets its content
    * (truncated to fit) written into rows[i]. Returns the count written. */
   typedef struct
   {
      char content[2048];
   } db2_memory_episode_card_row_t;

   int db2_memory_list_episode_cards(db2_memory_episode_card_row_t *rows, int max);

   /* List up to `max` global-constraint memories: kind IN ('preference',
    * 'policy') AND tier IN ('L1', 'L2', 'L3', 'L4'), ordered by confidence
    * DESC, use_count DESC. Returns the count written. */
   int db2_memory_list_global_constraints(db2_memory_kv_row_t *rows, int max);

   /* (id, key, content) row used by the workspace-scoped section helpers. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
   } db2_memory_id_kv_row_t;

   /* Sections used by memory_assemble_context_ws (the workspace-aware path). */
   typedef enum
   {
      DB2_MEM_WS_KEY_FACTS = 1,
      DB2_MEM_WS_ACTIVE_TASKS,
      DB2_MEM_WS_RECENT_CONTEXT,
      DB2_MEM_WS_CONSTRAINTS,
      DB2_MEM_WS_PROCEDURES,
      DB2_MEM_WS_CROSS_WORKSPACE,
   } db2_memory_ws_section_t;

   /* Fetch (id, key, content) rows for a fixed workspace-scoped section.
    * Returns the count written. */
   int db2_memory_list_id_kv_ws_section(db2_memory_ws_section_t section,
                                        db2_memory_id_kv_row_t *rows, int max);

   /* SELECT COUNT(*), MAX(updated_at) FROM memories. Writes the count to
    * `*out_count` and the timestamp (NUL-terminated, may be empty) to `out_ts`.
    * Returns 1 on hit, 0 on SQL error or empty table. */
   int db2_memory_count_and_max_updated(int *out_count, char *out_ts, int out_ts_len);

   /* Result of db2_memory_supersede_lookup: data describing the (old, new)
    * pair where `new` supersedes `old`. Strings are truncated to fit. */
   typedef struct
   {
      int64_t old_id;
      char old_content[2048];
      char valid_until[32];
      double old_confidence;
      int64_t new_id;
      char new_content[2048];
      double new_confidence;
   } db2_memory_supersede_pair_t;

   /* Look up the (old, new) supersede pair for `new_memory_id` via memory_links
    * relation = 'supersedes'. Returns 1 on hit, 0 otherwise. */
   int db2_memory_supersede_lookup(int64_t new_memory_id, db2_memory_supersede_pair_t *out);

   /* Row shape for db2_memory_list_key_facts_with_provenance. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
      char supersede_date[32];
      char provenance[256];
   } db2_memory_key_fact_row_t;

   /* List up to `max` L2/L3 fact-or-preference memories with their latest
    * supersede timestamp and most-recent provenance summary. Ordered by
    * confidence DESC, use_count DESC. Returns the count written. */
   int db2_memory_list_key_facts_with_provenance(db2_memory_key_fact_row_t *rows, int max);

   /* Fetch up to `max` (capped at 3) keys of memories that `memory_id`
    * depends_on via memory_links. Each key is written into rows[i] (truncated
    * to fit) and the count is returned. */
   typedef struct
   {
      char key[512];
   } db2_memory_key_row_t;

   int db2_memory_list_depends_on_keys(int64_t memory_id, db2_memory_key_row_t *rows, int max);

   /* Row shape for db2_memory_list_candidates. */
   typedef struct
   {
      int64_t id;
      char tier[4];
      char key[512];
      char content[2048];
      char kind[16];
      double confidence;
      int use_count;
   } db2_memory_cand_row_t;

   /* Tier filter selecting which set of candidates to load. */
   typedef enum
   {
      DB2_MEM_CAND_PRIMARY = 1,  /* tier IN ('L1','L2','L3','L4','L5') */
      DB2_MEM_CAND_FALLBACK = 2, /* tier IN ('L0','L1','L2','L4') */
   } db2_memory_cand_filter_t;

   /* List candidates ordered by confidence DESC, use_count DESC. Returns
    * the count written. */
   int db2_memory_list_candidates(db2_memory_cand_filter_t filter, db2_memory_cand_row_t *rows,
                                  int max);

   /* Row shape for db2_memory_l1_session_clusters. */
   typedef struct
   {
      char session_id[128];
      int count;
   } db2_memory_l1_cluster_row_t;

   /* List (source_session, count) for L1 memories grouped by source_session,
    * excluding `excluded_source` and rows where source_session is empty.
    * Filtered to clusters with at least `min_count` rows. Returns the count
    * written. */
   int db2_memory_l1_session_clusters(const char *excluded_source, int min_count,
                                      db2_memory_l1_cluster_row_t *rows, int max);

   /* Fetch up to `max` created_at strings (NUL-terminated, truncated to fit)
    * for L1 memories with the given source_session, ordered ASC. Returns count
    * written. Each row is a fixed-size 32-byte buffer. */
   typedef struct
   {
      char created_at[32];
   } db2_memory_created_at_row_t;

   int db2_memory_l1_session_created_at(const char *session_id, db2_memory_created_at_row_t *rows,
                                        int max);

   /* Fetch up to `max` content strings (NUL-terminated, truncated to fit) for
    * L1 memories with the given source_session, ordered ASC by created_at.
    * Returns count written. */
   typedef struct
   {
      char content[2048];
   } db2_memory_content_row_t;

   int db2_memory_l1_session_content(const char *session_id, db2_memory_content_row_t *rows,
                                     int max);

   /* Section identifiers for memory_recall's static recall pipeline. */
   typedef enum
   {
      DB2_MEM_RECALL_IDENTITY = 1,
      DB2_MEM_RECALL_PREFERENCES,
      DB2_MEM_RECALL_ACTIVE_CONTEXT,
      DB2_MEM_RECALL_OPEN_COMMITMENTS,
   } db2_memory_recall_section_t;

   /* Fetch (id, tier, kind, key, content) rows for a fixed recall section
    * (memory_recall in memory_context.c). Reuses db2_memory_cand_row_t as
    * the row shape; confidence/use_count are not populated by these queries
    * and remain zero. Returns the count written. */
   int db2_memory_list_recall_section(db2_memory_recall_section_t section,
                                      db2_memory_cand_row_t *rows, int max);

   /* Returns the count of L2 memories for `source_session`. Used by the
    * window-compaction pass to decide whether a session window deserves
    * elevated retention. Returns 0 on error or no rows. */
   int db2_memory_count_l2_for_session(const char *source_session);

   /* Delete every L0 memory for `session_id` along with its provenance rows.
    * Best-effort. Used by session folding after the L1 episode checkpoint
    * has been written. */
   void db2_memory_session_l0_purge(const char *session_id);

   /* Read the most recent contradiction_log row marked details='retroactive_scan'.
    * Writes the timestamp string into `out` (truncated to fit) on hit.
    * Returns 1 on hit (out has a non-empty timestamp), 0 on miss / SQL
    * failure / NULL detected_at. */
   int db2_memory_last_retro_scan(char *out, int out_len);

   /* Returns the number of memories in tier 'L2' (or 0 on error). */
   int db2_memory_count_l2(void);

   /* Returns the number of memories in tier 'L3' (or 0 on error). */
   int db2_memory_count_l3(void);

   /* Returns the number of L0-tier memories whose created_at is older
    * than 7 days. Used by the doctor to flag orphaned scratch rows.
    * Returns 0 on error. */
   int db2_memory_count_orphaned_l0(void);

   /* DELETEs every L0-tier memory whose created_at is older than 7
    * days. Returns the number of rows removed (>=0), or -1 on DB /
    * connection error. */
   int db2_memory_prune_orphaned_l0(void);

   /* (id_a, id_b, content_a, content_b) row used by retroactive contradiction
    * scans. Caller runs is_contradiction over the content pair and records
    * matches via memory_record_conflict. */
   typedef struct
   {
      int64_t id_a;
      int64_t id_b;
      char content_a[2048];
      char content_b[2048];
   } db2_memory_pair_row_t;

   /* Cross-key L2 pair candidates: distinct keys, each side confidence > 0.5,
    * each side's leading "<word>_" key prefix appears in the other side's
    * key||content. Up to `max_pairs` rows. */
   int db2_memory_l2_cross_key_pairs(int max_pairs, db2_memory_pair_row_t *out, int max);

   /* Cross-kind candidates: fact (in L1/L2) vs decision (in L2/L3) where each
    * side's key appears in the other side's content. Up to `max_pairs` rows.
    * Returned as (fact_id, decision_id, fact_content, decision_content). */
   int db2_memory_l2_fact_vs_decision_pairs(int max_pairs, db2_memory_pair_row_t *out, int max);

   /* Record a 'retroactive_scan' marker row in contradiction_log for rate
    * limiting. Best-effort. */
   void db2_memory_record_retro_scan_marker(const char *ts);

   /* INSERT a memory_lineage row. Returns the new rowid on success, or -1 on
    * SQL error. `source_ref` may be NULL (stored as empty string). */
   int64_t db2_memory_lineage_insert(const char *object_type, int64_t object_id,
                                     const char *source_kind, const char *source_ref,
                                     double confidence);

   /* db2_memory_collect_code_matches moved out of the db2 surface: the code-
    * shaped-query path now routes through pgvector memory search alongside
    * the generic lexical path. memory_core_search.inc still exposes
    * memory_collect_code_matches as the wrapper that tags hits with
    * MEM_SOURCE_CODE. */

   /* The direct memory recall collector moved out of the db2 surface.
    * Dense memory retrieval now routes through pgvector. The retrieval
    * lives in
    * memory_core_search.inc::memory_collect_memory_matches_via_vector. */

   /* Collect memory rows joined to memory_relations where any of
    * src_entity / dst_entity / relation / fact_text matches `token`
    * (case-insensitive; fact_text uses substring match). Grouped by memory_id
    * and ordered by max(weight) DESC, confidence DESC, use_count DESC. */
   int db2_memory_collect_relation_token_matches(const char *token, int limit, memory_t *out,
                                                 int max);

   /* db2_memory_collect_negation_matches moved out of the db2 surface: the
    * negative-polarity recall path now routes through pgvector memory search
    * (memory_core_search.inc::memory_collect_memory_matches_via_vector).
    * Future work:
    * add negation_tokens to the memory point payload and switch to a vector
    * payload filter for literal-token negation matching. */

   /* Filter `ids` (length n) to those whose memories.lifecycle_state =
    * 'archived'. Writes matching ids into `out` (caller-allocated, capacity
    * `max`) and returns the count written. Used by the lifecycle-hide pass
    * during rerank. */
   int db2_memory_filter_archived_ids(const int64_t *ids, int n, int64_t *out, int max);

   /* Read (weight, unit_type, memory_kind) for one unit_id, but only if the
    * owning memory is still active (tier L1/L2/L3). Returns 1 on hit, 0 on
    * miss / inactive. The text outputs are NUL-terminated and truncated to
    * fit. Used by vector-driven unit-semantic matching to gate hits by
    * tier without a full scan. */
   int db2_memory_unit_active_meta(int64_t unit_id, double *weight_out, char *unit_type_out,
                                   int unit_type_len, char *unit_kind_out, int unit_kind_len);

   /* Fetch memories in `session_id` whose id is strictly less than `before_id`,
    * most-recent first (id DESC). Up to min(limit, max) rows. Used for
    * session-window expansion around a search result. */
   int db2_memory_session_neighbors_before(const char *session_id, int64_t before_id, int limit,
                                           memory_t *out, int max);

   /* Fetch memories in `session_id` whose id is strictly greater than
    * `after_id`, oldest first (id ASC). Up to min(limit, max) rows. */
   int db2_memory_session_neighbors_after(const char *session_id, int64_t after_id, int limit,
                                          memory_t *out, int max);

   /* Pick the most-specific temporal ref_key for `memory_id`, ordered by
    * granularity priority (date_phrase > absolute_day > month > weekday >
    * year > everything else), then weight DESC, then id ASC. Writes the
    * ref_key into `out` (NUL-terminated, truncated to fit) and returns 1 on
    * hit, 0 on miss. */
   int db2_memory_pick_first_temporal_ref(int64_t memory_id, char *out, int out_len);

   /* Aggregation routing: pick one of three SQL paths.
    *  - entity_seed non-NULL/empty: entity route (joins memory_entities).
    *  - else keyword non-NULL/empty: keyword LIKE route over content/key.
    *  - else: fallback "list-all" route ordered by last-touched.
    * Fills `out` with up to `max` rows; truncated_out is set to 1 when
    * the underlying result set has more than max rows. Returns count. */
   int db2_memory_aggregate(const char *entity_seed, const char *keyword, memory_t *out, int max,
                            int *truncated_out);

   /* Walk active memories grouped by key (confidence DESC), marking
    * non-canonical rows with merged_into = canonical_id. Honours
    * dry_run by skipping the UPDATE. Returns the count of duplicate
    * rows merged (or that would have been merged when dry_run). */
   int db2_memory_dedupe_by_key(int dry_run);

   /* Increment a memory's use_count and stamp last_used_at = now.
    * Returns 0 on success, -1 on miss / SQL failure. */
   int db2_memory_touch(int64_t memory_id);

   /* Single-row SELECT into memory_t. Returns 0 on hit, -1 on miss. */
   int db2_memory_get(int64_t memory_id, memory_t *out);

   /* DELETE FROM memory_provenance WHERE memory_id = ?. Best-effort. */
   void db2_memory_provenance_delete(int64_t memory_id);

   /* SELECT id FROM memory_units WHERE memory_id = ?. Up to `max` ids
    * written; returns count. */
   int db2_memory_unit_list_ids(int64_t memory_id, int64_t *out, int max);

   /* DELETE FROM memories WHERE id = ?. Returns rows changed (0 or 1). */
   int db2_memory_delete_row(int64_t memory_id);

   /* UPDATE memories SET content = ? WHERE id = ?. Returns rows changed. */
   int db2_memory_update_content(int64_t memory_id, const char *content);

   /* UPDATE memories SET confidence = GREATEST(confidence - 0.1, 0.0) WHERE id = ?.
    * Returns 0 on success, -1 on failure. */
   int db2_memory_reject(int64_t memory_id, const char *reason);

   /* SELECT lineage rows for an (object_type, object_id) pair. memory_lineage_t
    * lives in headers/memory.h; callers must include "aimee.h" before this
    * header for the type to resolve. Returns count written. */
   int db2_memory_lineage_get(const char *object_type, int64_t object_id, memory_lineage_t *out,
                              int max);

   /* List memories ordered by updated_at DESC, optionally filtered by
    * tier, kind, and an archived-hide flag. Empty/NULL filters are
    * skipped. limit <= 0 means "no LIMIT clause" (still capped by `max`).
    * Returns count written. */
   int db2_memory_list(const char *tier, const char *kind, int hide_archived, int limit,
                       memory_t *out, int max);

   /* Load a memory corpus suitable for the live `aimee eval mem`
    * harness.  Tries three plans in order — L2 facts, then any-tier
    * facts in {L1,L2,L3}, then durable memories in {L1,L2,L3} with
    * kind != 'scratch' — and returns the first plan that yields rows.
    *
    * On success writes up to |max| rows into |out|, copies the chosen
    * plan's label into |label_out| (when non-NULL), and returns the
    * row count.  Returns 0 (and clears |label_out|) when no plan
    * matches or DB2 is not initialized. */
   int db2_memory_load_eval_corpus(memory_t *out, int max, char *label_out, size_t label_len);

   /* Look up an existing high-confidence L2 memory whose key matches and
    * whose content differs from `content` (the conflict-gate probe).
    * Fills *existing_confidence_out with the conflicting row's
    * confidence and returns 1 on hit, 0 on no conflict, -1 on error. */
   int db2_memory_find_conflicting_l2(const char *key, const char *content,
                                      double *existing_confidence_out);

   /* Look up the first memory matching `key` and `kind` exactly. Returns
    * the row id on hit (>0), or 0 on no match / error. Used by the
    * agent-feedback path to link an outcome episode to its source task
    * memory without leaking SQL outside src/db2/. */
   int64_t db2_memory_find_id_by_key_kind(const char *key, const char *kind);

   /* Returns 1 if a memory row exists matching `key` AND tier ∈
    * {tier_a, tier_b}, else 0. Used by kb_extract_convention_candidates
    * to skip duplicates before promoting a chunk to L3/L4. */
   int db2_memory_key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b);

   /* Memories export helpers (cJSON serialization, heap-allocated row
    * type) live in db2/memory_export.h to keep this file under the
    * line-check budget. */

   /* Top-N L2 fact memories ordered by use_count DESC, confidence DESC.
    * Fills only |key| and |content| on each row (other fields are
    * zeroed); used by the session-briefing "Key Facts" panel. Returns
    * rows written. */
   int db2_memory_top_l2_facts(memory_t *out, int max);

   /* L1/L2/L3 memories ordered by kind priority (workflow → decision →
    * other), then tier priority (L3 → L2 → L1), then use_count DESC.
    * Fills id/key/content/kind on each row (other fields zeroed). Used
    * by the session-briefing scope-context panels which apply their
    * own scope-rank filter on top. Returns rows written. */
   int db2_memory_list_session_scope_priority(memory_t *out, int max);

   /* Same ordering as db2_memory_list_session_scope_priority but
    * filtered to memories whose key OR content LIKE |pattern| AND
    * which are NOT yet tagged in memory_workspaces. The session
    * briefing uses this as a fallback when the canonical scoped query
    * comes back empty. Returns rows written. */
   int db2_memory_list_session_scope_priority_like(const char *pattern, memory_t *out, int max);

   /* Substring-keyword search over L2/L3/L5 fact/pattern memories.
    * Matches when LOWER(content) or LOWER(key) contains LOWER(keyword).
    * Ordered by confidence DESC, use_count DESC. Fills only the |key|
    * and |content| fields of each row (other fields are zeroed); used
    * by the agent-runtime relevance injector. Returns rows written. */
   int db2_memory_search_facts_patterns_by_keyword(const char *keyword, memory_t *out, int max);

   /* (tier, kind, count) tuple from a `GROUP BY tier, kind` over the
    * memories table.  Used by the dashboard's tier/kind panel. */
   typedef struct
   {
      char tier[16];
      char kind[32];
      int count;
   } db2_memory_tier_kind_count_t;

   /* Group memories by (tier, kind) and return the (tier, kind, count)
    * triples ordered by tier, then kind. Returns rows written into |out|
    * (capped by |max|). */
   int db2_memory_count_by_tier_kind(db2_memory_tier_kind_count_t *out, int max);

   /* Allocate and fill an array of every memory id in DB2. On success,
    * sets *out_ids to a heap-allocated buffer the caller must free()
    * and *out_count to its length, and returns 0. On allocation or DB
    * failure, sets *out_ids to NULL, *out_count to 0, and returns -1.
    *
    * Used by the dashboard scope-counter panel which needs to call
    * memory_primary_scope() on every row; libpq only supports one
    * active result per connection, so the caller materialises the id
    * list before issuing per-id sub-queries. */
   int db2_memory_alloc_all_ids(int64_t **out_ids, size_t *out_count);

   /* INSERT OR IGNORE into memory_entities. Caller is responsible for
    * canonicalizing `entity` first. Best-effort. */
   void db2_memory_entity_insert(int64_t memory_id, const char *entity, const char *role,
                                 double weight);

   /* INSERT OR IGNORE into memory_temporal_refs. Caller passes the
    * normalized ref_key. Best-effort. */
   void db2_memory_temporal_insert(int64_t memory_id, const char *ref_key, const char *granularity,
                                   double weight);

   /* DELETE FROM memory_entities WHERE memory_id = ?. Best-effort. */
   void db2_memory_entities_delete_for_memory(int64_t memory_id);

   /* DELETE FROM memory_temporal_refs WHERE memory_id = ?. Best-effort. */
   void db2_memory_temporal_refs_delete_for_memory(int64_t memory_id);

   /* Read created_at for a memory and parse it as YYYY-MM-DD. Returns 1
    * on success (year/month/day filled), 0 on miss / parse failure. */
   int db2_memory_parse_created_date(int64_t memory_id, int *year, int *month, int *day);

   /* Replace the negation_tokens column on a memory and sync the DB2
    * negation lexical index accordingly. Best-effort. */
   void db2_memory_negation_tokens_update(int64_t memory_id, const char *new_tokens);

   /* Read source_session for a memory id. Returns 0 on hit, -1 on miss
    * or empty session_id. */
   int db2_memory_get_source_session(int64_t memory_id, char *out, int out_len);

   /* (content, key) pair returned by db2_memory_list_prior_in_session. */
   typedef struct
   {
      char content[2048];
      char key[256];
   } db2_memory_prior_row_t;

   /* List up to `max` memories with the same source_session and an
    * id < `before_id`, ordered newest-first, capped by `limit`. */
   int db2_memory_list_prior_in_session(const char *session_id, int64_t before_id, int limit,
                                        db2_memory_prior_row_t *out, int max);

   /* INSERT a memory_coref_audit row. Best-effort. */
   void db2_memory_coref_audit_insert(int64_t memory_id, const char *session_id,
                                      const char *outcome, const char *entity, const char *mode,
                                      double confidence);

   /* (id, unit_type, unit_key, unit_text, weight) row from memory_units. */
   typedef struct
   {
      int64_t id;
      char unit_type[32];
      char unit_key[256];
      char unit_text[2048];
      double weight;
   } db2_memory_unit_row_t;

   /* List memory_units for a memory_id. Returns count written. */
   int db2_memory_units_list(int64_t memory_id, db2_memory_unit_row_t *out, int max);

   /* Pick the highest-priority entity attached to a memory (actor >
    * subject > person > other; tiebreak by weight desc / id asc). */
   void db2_memory_lookup_primary_entity(int64_t memory_id, char *out, int out_len);

   /* Compute (valid_at, invalid_at) for a memory by combining
    * memories.valid_from / valid_until with the strongest temporal_ref
    * and falling back to created_at. */
   void db2_memory_lookup_time_bounds(int64_t memory_id, char *valid_at_out, int valid_len,
                                      char *invalid_at_out, int invalid_len);

   /* INSERT OR REPLACE into memory_episodes and return the row id of
    * the inserted/looked-up episode (0 on failure). The follow-up
    * SELECT handles the OR REPLACE case where last_insert_rowid
    * isn't reliable. */
   int64_t db2_memory_episode_insert(int64_t memory_id, const char *episode_key,
                                     const char *episode_text, const char *source_session,
                                     const char *reference_time);

   /* Fetch the parent memory_t for a unit_id (joins memory_units to
    * memories). Returns 0 on hit, -1 on miss. Salience is not
    * returned (legacy contract). */
   int db2_memory_get_by_unit_id(int64_t unit_id, memory_t *out);

   /* INSERT OR IGNORE into memory_unit_edges (src, dst, edge_type, weight).
    * Self-edges are silently skipped. Best-effort. */
   void db2_memory_unit_edge_insert(int64_t src_unit_id, int64_t dst_unit_id, const char *edge_type,
                                    double weight);

   /* DELETE FROM memory_relations WHERE memory_id = ?. Best-effort. */
   void db2_memory_relations_delete_for_memory(int64_t memory_id);

   /* DELETE FROM memory_episodes WHERE memory_id = ?. Best-effort. */
   void db2_memory_episodes_delete_for_memory(int64_t memory_id);

   /* (scope, summary) row from memory_summaries. */
   typedef struct
   {
      char scope[64];
      char summary[2048];
   } db2_memory_summary_row_t;

   /* List memory_summaries rows for a memory_id, ORDER BY id ASC.
    * limit <= 0 means unlimited (still capped by `max`). */
   int db2_memory_summaries_list(int64_t memory_id, int limit, db2_memory_summary_row_t *out,
                                 int max);

   /* Shadow-mode learned retrieval shortcut observation. Records a normalized
    * query and the ordered top memory ids produced by the normal blend. Stable
    * repeated mappings are promoted in-store, but callers still run normal
    * recall; consumers can use the table later to safely short-circuit. */
   int db2_retrieval_shortcut_observe(const char *normalized_query, const int64_t *ids, int count);

   /* INSERT OR UPDATE memory_summaries(memory_id, scope, summary).
    * Empty `scope` defaults to "headline". Best-effort; no return. */
   void db2_memory_summary_upsert(int64_t memory_id, const char *scope, const char *summary);

   /* DELETE FROM memory_summaries WHERE memory_id = ?. Best-effort. */
   void db2_memory_summaries_delete_for_memory(int64_t memory_id);

   /* (actor, action, object, location, event_time) row from
    * memory_event_frames. */
   typedef struct
   {
      char actor[128];
      char action[128];
      char object[256];
      char location[128];
      char event_time[64];
   } db2_memory_event_frame_row_t;

   /* List memory_event_frames rows for a memory_id, ORDER BY id ASC. */
   int db2_memory_event_frames_list(int64_t memory_id, db2_memory_event_frame_row_t *out, int max);

   /* INSERT a memory_event_frames row. Empty `evidence_kind` defaults
    * to "derived". Best-effort. */
   void db2_memory_event_frame_insert(int64_t memory_id, const char *actor, const char *action,
                                      const char *object, const char *location,
                                      const char *event_time, const char *evidence_kind);

   /* DELETE FROM memory_event_frames WHERE memory_id = ?. Best-effort. */
   void db2_memory_event_frames_delete_for_memory(int64_t memory_id);

   /* DELETE FROM memory_chunks WHERE memory_id = ?. Best-effort. */
   void db2_memory_chunks_delete_for_memory(int64_t memory_id);

   /* DELETE FROM memory_aliases WHERE memory_id = ?. Best-effort. */
   void db2_memory_aliases_delete_for_memory(int64_t memory_id);

   /* (relation, target_key, target_content) row from memory_links
    * joined to the target memory. */
   typedef struct
   {
      char relation[64];
      char target_key[256];
      char target_content[2048];
   } db2_memory_link_target_row_t;

   /* List memory_links rows where source_id = memory_id, joined to the
    * target memory's key + content. ORDER BY ml.id ASC. */
   int db2_memory_links_with_targets(int64_t memory_id, db2_memory_link_target_row_t *out, int max);

   /* INSERT OR IGNORE into memory_units (memory_id, unit_type,
    * memory_kind, unit_key, unit_text, weight) and return the row id
    * of the inserted/looked-up unit (0 on failure). The caller passes
    * already-normalized unit_key + unit_text (normalize_key applied)
    * and the stringified memory_kind. */
   int64_t db2_memory_unit_insert(int64_t memory_id, const char *unit_type, const char *memory_kind,
                                  const char *unit_key, const char *unit_text, double weight);

   /* DELETE FROM memory_unit_edges WHERE either endpoint belongs to
    * memory_id's units. Best-effort. */
   void db2_memory_unit_edges_delete_for_memory(int64_t memory_id);

   /* DELETE FROM memory_units WHERE memory_id = ?. Best-effort. */
   void db2_memory_units_delete_for_memory(int64_t memory_id);

   /* Read source_session + kind + cognified_memory_kind for a memory
    * id. Returns 0 on hit, -1 on miss. */
   int db2_memory_get_session_kinds(int64_t memory_id, char *session_out, int session_len,
                                    char *kind_out, int kind_len, char *explicit_kind_out,
                                    int explicit_kind_len);

   /* (ref_key, granularity, weight) row from memory_temporal_refs,
    * ORDER BY weight DESC, id ASC. */
   typedef struct
   {
      char ref_key[128];
      char granularity[32];
      double weight;
   } db2_memory_temporal_ref_row_t;

   int db2_memory_temporal_refs_list(int64_t memory_id, db2_memory_temporal_ref_row_t *out,
                                     int max);

   /* (entity, role, weight) row from memory_entities,
    * ORDER BY weight DESC, id ASC. */
   typedef struct
   {
      char entity[256];
      char role[32];
      double weight;
   } db2_memory_entity_row_t;

   int db2_memory_entities_list_weighted(int64_t memory_id, db2_memory_entity_row_t *out, int max);

   /* (chunk_text, chunk_index) row from memory_chunks,
    * ORDER BY chunk_index ASC. */
   typedef struct
   {
      char chunk_text[2048];
      int chunk_index;
   } db2_memory_chunk_row_t;

   int db2_memory_chunks_list(int64_t memory_id, db2_memory_chunk_row_t *out, int max);

   /* INSERT OR UPDATE memory_chunks(memory_id, chunk_index, chunk_text).
    * Best-effort; no return. */
   void db2_memory_chunk_upsert(int64_t memory_id, int chunk_index, const char *chunk_text);

   /* INSERT OR IGNORE into memory_aliases(memory_id, alias, weight).
    * Caller passes the already-normalized alias. Best-effort. */
   void db2_memory_alias_insert(int64_t memory_id, const char *alias, double weight);

   /* List unit_ids of episode-type units belonging to other memories
    * in the same source_session. Used to draw same_episode edges
    * across the session. */
   int db2_memory_episode_unit_ids_for_session(int64_t memory_id, const char *source_session,
                                               int64_t *out, int max);

   /* List unit_ids of episode-type units belonging to memories that
    * `source_memory_id` supersedes (via memory_links.relation =
    * 'supersedes'). */
   int db2_memory_episode_unit_ids_supersedes(int64_t source_memory_id, int64_t *out, int max);

   /* (id, key, content) row from memories ORDER BY updated_at DESC.
    * Used by memory_rebuild_derived_indexes. limit <= 0 => unlimited. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
   } db2_memory_id_key_content_row_t;

   int db2_memory_list_id_key_content(int limit, db2_memory_id_key_content_row_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_QUERY_H */
