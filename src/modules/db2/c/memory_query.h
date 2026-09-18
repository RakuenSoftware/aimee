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

   /* Search L1+L2 memories whose key OR content matches `pattern`
    * (caller supplies the SQL LIKE pattern, e.g. "%foo%"). Limit 10
    * rows in SQL; orchestrator handles dedup + seed-skip + scoring. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
   } db2_memory_search_match_t;

   /* LIKE-based fact lookup over memories.{key,content}, used as the last-resort
    * fallback in the search pipeline when lexical / semantic / alias / entity all
    * miss. Returns up to min(limit, max) rows ranked by exact-key, exact-content,
    * key-prefix, then tier (L3>L2>L1) and use_count. */

   /* Indexed lexical lookup over memories_fts. Unlike the LIKE fallback above,
    * this is safe to call once per query token without rescanning the memories
    * table. The query must be normalized plain text. */
   int db2_memory_find_facts_fts(const char *query, int limit, memory_t *out, int max);

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

   /* List memory_episodes rows whose key/text/source_session contains `query`
    * (or all rows when `query` is empty). Ordered by exact-key match,
    * has-reference-time, then created_at DESC. Up to min(limit, max) rows.
    * memory_episode_t lives in headers/memory.h. */
   int db2_memory_episodes_search(const char *query, int limit, memory_episode_t *out, int max);

   /* (id, content) pair returned by db2_memory_list_by_key. */
   typedef struct
   {
      int64_t id;
      char content[2048];
   } db2_memory_id_content_row_t;

   /* (id, key, content) row returned by db2_memory_session_l0_list. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
   } db2_memory_session_l0_row_t;

   /* INSERT OR IGNORE a memory_unit row for an episode card. The hardcoded
    * shape: unit_type='episode_card', memory_kind='episodic', weight=2.0,
    * is_episode_card=1. Best-effort. */
   void db2_memory_unit_episode_card_insert(int64_t memory_id, const char *unit_key,
                                            const char *unit_text);

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

   /* UPDATE the standard merge fields: content, confidence, use_count,
    * observation_count, evidence_strength, salience, surprise,
    * last_used_at, updated_at WHERE id = ?. Best-effort. Returns 0 on
    * success, -1 on SQL error. */
   int db2_memory_merge_update_ex(int64_t memory_id, const char *content, const char *use_cases,
                                  double confidence, int use_count, int observation_count,
                                  double evidence_strength, double salience, double surprise,
                                  const char *ts);

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

   /* Row shape for db2_memory_list_unused_l2. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char tier[4];
      char kind[16];
      double confidence;
   } db2_memory_unused_l2_row_t;

   /* Row shape for db2_memory_list_superseded_keys. */
   typedef struct
   {
      char base_key[512];
      int versions;
   } db2_memory_superseded_row_t;

   /* Row shape for db2_memory_summarise_clusters. */
   typedef struct
   {
      char session_id[128];
      int count;
      double avg_confidence;
   } db2_memory_summary_cluster_t;

   /* Row shape for db2_memory_list_artifact_hashed. */
   typedef struct
   {
      int64_t id;
      char artifact_type[32];
      char artifact_ref[256];
      char artifact_hash[65];
   } db2_memory_artifact_row_t;

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

   /* List up to `max` (capped at 4) episode-card content strings, ordered by
    * memories.confidence DESC, memories.id DESC. Each row gets its content
    * (truncated to fit) written into rows[i]. Returns the count written. */
   typedef struct
   {
      char content[2048];
   } db2_memory_episode_card_row_t;

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

   /* Row shape for db2_memory_list_key_facts_with_provenance. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
      char supersede_date[32];
      char provenance[256];
   } db2_memory_key_fact_row_t;

   /* Fetch up to `max` (capped at 3) keys of memories that `memory_id`
    * depends_on via memory_links. Each key is written into rows[i] (truncated
    * to fit) and the count is returned. */
   typedef struct
   {
      char key[512];
   } db2_memory_key_row_t;

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
      /* Recency inputs for the context scorer. last_used_at is empty until the
       * memory has actually been recalled, so the scorer falls back to
       * created_at for never-recalled rows. */
      char last_used_at[32];
      char created_at[32];
      /* Origin identity for envelope diversity: which session this row came
       * from. Empty for rows with no originating session, which are each
       * treated as their own origin rather than pooled together. */
      char source_session[128];
      int activation_sticky_turns;
      int activation_cooldown_turns;
      int activation_delay_turns;
      int activation_suppressed;
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

   /* Fetch up to `max` created_at strings (NUL-terminated, truncated to fit)
    * for L1 memories with the given source_session, ordered ASC. Returns count
    * written. Each row is a fixed-size 32-byte buffer. */
   typedef struct
   {
      char created_at[32];
   } db2_memory_created_at_row_t;

   /* Fetch up to `max` source ids and content strings (NUL-terminated,
    * truncated to fit) for eligible current L1 memories with the given
    * source_session, ordered ASC by created_at. A completeness-sensitive
    * caller requests cap+1 and refuses the derivation when the extra row is
    * present. Returns count written. */
   typedef struct
   {
      int64_t id;
      char content[2048];
   } db2_memory_content_row_t;

   /* Section identifiers for memory_recall's static recall pipeline. */
   typedef enum
   {
      DB2_MEM_RECALL_IDENTITY = 1,
      DB2_MEM_RECALL_PREFERENCES,
      DB2_MEM_RECALL_ACTIVE_CONTEXT,
      DB2_MEM_RECALL_OPEN_COMMITMENTS,
   } db2_memory_recall_section_t;

   /* Fetch (id, tier, kind, key, content, activation policy) rows for a fixed
    * recall section (memory_recall in memory_context.c). Reuses
    * db2_memory_cand_row_t as the row shape; confidence/use_count are not
    * populated by these queries and remain zero. Returns the count written. */
   int db2_memory_list_recall_section(db2_memory_recall_section_t section,
                                      db2_memory_cand_row_t *rows, int max);

   /* Returns the count of L2 memories for `source_session`. Used by the
    * window-compaction pass to decide whether a session window deserves
    * elevated retention. Returns 0 on error or no rows. */
   int db2_memory_count_l2_for_session(const char *source_session);

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

   /* db2_memory_collect_code_matches moved out of the db2 surface: the code-
    * shaped-query path now routes through pgvector memory search alongside
    * the generic lexical path. memory_core_search.inc still exposes
    * memory_collect_code_matches as the wrapper that tags hits with
    * MEM_SOURCE_CODE. */

   /* The direct memory recall collector moved out of the db2 surface.
    * Dense memory retrieval now routes through pgvector. The retrieval
    * lives in
    * memory_core_search.inc::memory_collect_memory_matches_via_vector. */

   /* db2_memory_collect_negation_matches moved out of the db2 surface: the
    * negative-polarity recall path now routes through pgvector memory search
    * (memory_core_search.inc::memory_collect_memory_matches_via_vector).
    * Future work:
    * add negation_tokens to the memory point payload and switch to a vector
    * payload filter for literal-token negation matching. */

   /* Cursor form for callers that genuinely need to enumerate units. Rows are
    * ordered by id and strictly follow `after_id`. `has_more_out` is set when
    * another page exists, so filling the caller's buffer cannot masquerade as
    * completion. Returns rows written, or -1 on SQL failure. */
   int db2_memory_unit_list_ids_after(int64_t memory_id, int64_t after_id, int64_t *out, int max,
                                      int *has_more_out);

   typedef struct
   {
      int64_t id;
      char tier[4];
      char kind[16];
      char key[512];
      char content[2048];
      double confidence;
      char lifecycle_state[24];
      char review_reason[256];
      char scope_type[16];
      char scope_value[512];
      char created_at[32];
      char updated_at[32];
   } db2_memory_review_row_t;

   /* Human review/history surface. Ordinary recall never uses this function. */
   int db2_memory_review_list(const char *state, int limit, db2_memory_review_row_t *out, int max);

   /* List memories ordered by updated_at DESC, optionally filtered by
    * tier, kind, and an archived-hide flag. Empty/NULL filters are
    * skipped. limit <= 0 means "no LIMIT clause" (still capped by `max`).
    * Returns count written. */
   int db2_memory_list(const char *tier, const char *kind, int hide_archived, int limit,
                       memory_t *out, int max);

   /* Look up the first memory matching `key` and `kind` exactly. Returns
    * the row id on hit (>0), or 0 on no match / error. Used by the
    * agent-feedback path to link an outcome episode to its source task
    * memory without leaking SQL outside src/modules/db2/c/. */
   int64_t db2_memory_find_id_by_key_kind(const char *key, const char *kind);

   /* Legacy DB2 wire compatibility. Convention extraction is owned by Go. */
   int db2_memory_key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b);

   /* (tier, kind, count) tuple from a `GROUP BY tier, kind` over the
    * memories table.  Used by the dashboard's tier/kind panel. */
   typedef struct
   {
      char tier[16];
      char kind[32];
      int count;
   } db2_memory_tier_kind_count_t;

   /* (id, unit_type, unit_key, unit_text, weight) row from memory_units. */
   typedef struct
   {
      int64_t id;
      char unit_type[32];
      char unit_key[256];
      char unit_text[2048];
      double weight;
   } db2_memory_unit_row_t;

   /* INSERT OR REPLACE into memory_episodes and return the row id of
    * the inserted/looked-up episode (0 on failure). The follow-up
    * SELECT handles the OR REPLACE case where last_insert_rowid
    * isn't reliable. */
   int64_t db2_memory_episode_insert(int64_t memory_id, const char *episode_key,
                                     const char *episode_text, const char *source_session,
                                     const char *reference_time);

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

   /* (relation, target_key, target_content) row from memory_links
    * joined to the target memory. */
   typedef struct
   {
      char relation[64];
      char target_key[256];
      char target_content[2048];
   } db2_memory_link_target_row_t;

   /* (ref_key, granularity, weight) row from memory_temporal_refs,
    * ORDER BY weight DESC, id ASC. */
   typedef struct
   {
      char ref_key[128];
      char granularity[32];
      double weight;
   } db2_memory_temporal_ref_row_t;

   /* (entity, role, weight) row from memory_entities,
    * ORDER BY weight DESC, id ASC. */
   typedef struct
   {
      char entity[256];
      char role[32];
      double weight;
   } db2_memory_entity_row_t;

   /* (chunk_text, chunk_index) row from memory_chunks,
    * ORDER BY chunk_index ASC. */
   typedef struct
   {
      char chunk_text[2048];
      int chunk_index;
   } db2_memory_chunk_row_t;

   /* (id, key, content) row from memories ORDER BY updated_at DESC.
    * Used by memory_rebuild_derived_indexes. limit <= 0 => unlimited. */
   typedef struct
   {
      int64_t id;
      char key[512];
      char content[2048];
   } db2_memory_id_key_content_row_t;

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_MEMORY_QUERY_H */
