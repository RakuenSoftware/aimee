#ifndef MEMORY_CORE_INTERNAL_H
#define MEMORY_CORE_INTERNAL_H
/* INTERNAL: cross-TU decls for the memory_core KB-real cluster (memory_core.c #else
 * branch + the .c split out of it). NEVER included by the AIMEE_DB2_DISABLED stub
 * build. Unstable/private. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include "aimee.h"
#include "config.h"
#include "memory.h"
#include "cJSON.h"

/* Auto-free a heap pointer on scope exit. The memory-search chain declares large
 * memory_t[] scratch buffers (sizeof(memory_t) ~4.4KB, e.g. [128] = 566KB); with
 * -flto inlining the deep query chain into ~1MB frames these nest and overflow the
 * stack (worker threads especially). Moving them to the heap keeps the chain off
 * the stack without threading free() through every early return. */
static inline void memory_autofree_impl(void *p)
{
   free(*(void **)p);
}
#define MEMORY_AUTOFREE __attribute__((cleanup(memory_autofree_impl)))

/* memory_core real-branch shared types (moved from memory_core.c #else) */
typedef enum
{
   MEM_QUERY_GENERAL = 0,
   MEM_QUERY_TEMPORAL,
   MEM_QUERY_ENTITY,
   MEM_QUERY_PROCEDURAL
} memory_query_intent_t;

typedef struct
{
   int year;
   int month;
   int day;
   int granularity; /* 1=year, 2=month, 3=day */
   int valid;
} memory_parsed_date_t;

typedef struct
{
   enum
   {
      MEM_DATE_CONSTRAINT_NONE = 0,
      MEM_DATE_CONSTRAINT_MATCH,
      MEM_DATE_CONSTRAINT_BEFORE,
      MEM_DATE_CONSTRAINT_AFTER,
      MEM_DATE_CONSTRAINT_BETWEEN
   } mode;
   memory_parsed_date_t first;
   memory_parsed_date_t second;
} memory_temporal_constraint_t;

typedef struct
{
   double lexical_key;
   double lexical_content;
   double lexical_long_token_bonus;
   double coverage;
   double entity;
   double temporal;
   double evidence;
   double semantic;
   double state;
   double workflow_intent;
   double entity_intent;
   double temporal_intent;
   double salience;
   double surprise;
   double speaker;
} memory_rank_weights_t;

typedef struct
{
   int enabled;
   int iterations;
   double weight;
   char relations[256];
} memory_pagerank_config_t;

typedef struct
{
   int64_t memory_id;
   double score;
} memory_pagerank_score_t;

typedef struct
{
   double last_ms;
   double avg_ms;
   double max_ms;
   int samples;
   int last_candidates;
   int last_edges;
} memory_pagerank_runtime_stats_t;

extern memory_pagerank_runtime_stats_t s_memory_pagerank_stats;
extern pthread_mutex_t s_memory_pagerank_stats_mu;
/* promoted cross-TU (former .inc statics) */
int memory_append_linked_neighbors(memory_t *matches, int count, int max_count,
                                   const char *allowed_relations);
double memory_base_evidence_strength(const char *key, const char *content, double confidence);
void memory_canonicalize_term(const char *input, char *out, size_t out_len);
int memory_compute_pagerank_scores(const memory_t *matches, int count,
                                   const memory_pagerank_config_t *cfg,
                                   memory_pagerank_score_t *out, int max_out);
double memory_content_salience(const char *content);
double memory_content_surprise(const char *session_id, const char *content);
const char *memory_effective_embedding_cmd(const char *command);
int memory_embed_command_is_http(const char *cmd);
int memory_embed_http_post(const char *base, const char *path, const char *body, char **resp);
int memory_embed_text_runtime(const char *text, const char *command, float *out, int max_dim);
int memory_env_int(const char *name, int fallback, int min_value, int max_value);
double memory_env_weight(const char *name, double fallback);
int memory_expand_query_terms(const char *norm_query, char base_tokens[][64], int base_count,
                              char expanded[][64], int max_terms);
int memory_fetch_row_by_id(int64_t memory_id, memory_t *out);
int memory_fetch_row_by_unit_id(int64_t unit_id, memory_t *out);
void memory_fill_pagerank_stats(memory_stats_t *out);
int memory_is_coref_pronoun_token(const char *tok);
int memory_is_likely_action_token(const char *tok);
int memory_is_month_token(const char *tok);
int memory_is_probable_location_token(const char *tok);
int memory_is_relation_token(const char *tok);
int memory_is_signal_token(const char *tok);
int memory_is_stopword_token(const char *tok);
int memory_is_weekday_token(const char *tok);
void memory_load_pagerank_config(memory_pagerank_config_t *cfg);
void memory_maybe_run_maintenance(void);
int memory_month_number(const char *tok);
double memory_pagerank_bonus_lookup(const memory_pagerank_score_t *scores, int count,
                                    int64_t memory_id);
void memory_query_embed_cache_reset(void);
void memory_query_embed_prewarm(const char *const *texts, int n, const char *command);
int memory_query_has_term(const char *norm_query, const char *term);
memory_query_intent_t memory_query_intent(const char *raw_query, const char *norm_query);
int memory_query_is_code_like(const char *query);
memory_query_route_t memory_query_route(const char *raw_query, const char *norm_query,
                                        memory_query_shape_t shape);
memory_query_shape_t memory_query_shape(const char *raw_query, const char *norm_query);
memory_rank_weights_t memory_rank_weights(void);
void memory_refresh_aliases(int64_t memory_id, const char *key, const char *content);
void memory_refresh_chunks(int64_t memory_id, const char *content);
void memory_refresh_coref_entities(int64_t memory_id, const char *content);
void memory_refresh_derived_metadata(int64_t memory_id, const char *key, const char *content);
void memory_refresh_episode_relations(int64_t memory_id, const char *key, const char *content);
void memory_refresh_event_frames(int64_t memory_id, const char *key, const char *content);
void memory_refresh_summaries(int64_t memory_id, const char *key, const char *content);
void memory_refresh_unit_embeddings(int64_t memory_id);
void memory_refresh_units_graph(int64_t memory_id, const char *key, const char *content);
int memory_rerank_is_slow(void);
void memory_runtime_state_increment(const char *key, int delta);
int memory_salience_enabled(void);
double memory_salience_weight(void);
int memory_scope_matches(int64_t memory_id, const char *scope_type, const char *scope_value);
int memory_semantic_query_unavailable(void);
int memory_skip_persistent_side_effects(void);
int memory_split_tokens(const char *norm, char tokens[][64], int max_tokens);
int memory_surprise_enabled(void);
double memory_surprise_weight(void);
int memory_sync_vec_row(int64_t memory_id, const float *vec, int dim);
int memory_temporal_value_conflict(const char *a, const char *b);
int memory_token_in_norm(const char *norm, const char *token);
double memory_unit_kind_intent_boost(const char *memory_kind, memory_query_intent_t intent);
int memory_vector_ready(void);

extern __thread long long s_qembed_ms;
extern __thread int s_qembed_spawns;
extern __thread int s_rerank_calls;
extern __thread long long s_rerank_ms;
/* promoted cross-TU (former .inc statics) */
void memory_alias_insert(int64_t memory_id, const char *alias, double weight);
int memory_alias_is_useful_token(const char *token);
void memory_alias_join_tokens(char *buf, size_t buf_len, char tokens[][64], int start, int count);
void memory_coref_audit_record(int64_t memory_id, const char *session_id, const char *outcome,
                               const char *entity, const char *mode, double confidence);
int memory_coref_has_pronoun(const char *content);
int memory_coref_llm_resolve(int64_t memory_id, const char *content, const char *session_buf,
                             const config_t *cfg);
const char *memory_coref_mode_effective(const config_t *cfg);
int memory_coref_window_effective(const config_t *cfg);
void memory_entity_insert(int64_t memory_id, const char *entity, const char *role, double weight);
int memory_extract_named_entities(const char *text, char names[][128], int max_names);
void memory_format_date(char *buf, size_t buf_len, int year, int month, int day);
int memory_parse_created_date(int64_t memory_id, int *year, int *month, int *day);
void memory_refresh_entities(int64_t memory_id, const char *key, const char *content);
void memory_shift_day(int *year, int *month, int *day, int delta);
void memory_temporal_insert(int64_t memory_id, const char *ref_key, const char *granularity,
                            double weight);

typedef struct
{
   int64_t memory_id;
   unsigned int source_mask;
} memory_candidate_source_t;

extern int s_lane_fact_count;
extern int64_t s_lane_fact_ids[96];
extern int s_lane_summary_count;
extern int64_t s_lane_summary_ids[96];
/* promoted cross-TU (former .inc statics) */
int memory_add_expanded_term(char terms[][64], int count, int max_terms, const char *term);
int memory_append_unique(memory_t *out, int count, int max, const memory_t *candidate);
int memory_apply_lane_floor(memory_t *matches, int count, const int64_t *lane_ids, int lane_count,
                            int floor_n, int total);
int memory_build_query_decomposition(const char *norm_query, char subqueries[][128],
                                     int max_subqueries);
void memory_build_semantic_query_text(const char *raw_query, memory_query_intent_t intent,
                                      char *out, size_t out_len);
int memory_collect_alias_matches(const char *alias, int limit, memory_t *out, int count, int max);
int memory_collect_chunk_matches(const char *query, int limit, memory_t *out, int count, int max);
int memory_collect_entity_matches(const char *term, int limit, memory_t *out, int count, int max);
int memory_collect_event_frame_matches(const char *term, int limit, memory_t *out, int count,
                                       int max);
int memory_collect_memory_matches_via_vector(const char *query, const char *embed_cmd, int limit,
                                             memory_t *out, int max);
int memory_collect_semantic_matches(const char *query, const char *command, int limit,
                                    memory_t *out, int count, int max, int64_t *semantic_ids,
                                    double *semantic_scores, int *semantic_hit_count,
                                    int semantic_max);
int memory_collect_summary_matches(const char *term, int limit, memory_t *out, int count, int max);
int memory_collect_temporal_matches(const char *term, int limit, memory_t *out, int count, int max);
int memory_collect_unit_matches(const char *query, int limit, memory_t *out, int count, int max);
int memory_collect_unit_matches_via_vector(const char *query, const char *embed_cmd, int limit,
                                           memory_t *out, int max);
int memory_collect_unit_semantic_matches(const char *query, const char *command, int limit,
                                         memory_t *out, int count, int max, int64_t *semantic_ids,
                                         double *semantic_scores, int *semantic_hit_count,
                                         int semantic_max);
int memory_collect_variant_candidates(const char *raw_query, const char *norm_variant,
                                      memory_query_intent_t intent, int fetch_limit, memory_t *out,
                                      int count, int max, memory_candidate_source_t *source_stats,
                                      int *source_stats_count);
void memory_compute_score_parts(const char *raw_query, const char *norm_query,
                                const memory_ranker_input_t *m, const int64_t *semantic_ids,
                                const double *semantic_scores, int semantic_hit_count,
                                const memory_pagerank_score_t *pagerank_scores, int pagerank_count,
                                memory_score_parts_t *parts);
double memory_entity_bonus(int64_t memory_id, char qtokens[][64], int nq);
void memory_expand_synonyms(const char *query, char *out, size_t out_len);
int memory_filter_scope(memory_t *matches, int count, const char *scope_type,
                        const char *scope_value);
int memory_find_facts_lexical_fallback(const char *query, const char *scope_type,
                                       const char *scope_value, int limit, memory_t *out, int max);
int memory_find_facts_like(const char *query, int limit, memory_t *out, int max);
int memory_find_facts_visible_lexical_fallback(const char *query, const char *workspace,
                                               const char *project, int limit, memory_t *out,
                                               int max);
int memory_parse_temporal_constraint(const char *norm_query, char qtokens[][64], int nq,
                                     memory_temporal_constraint_t *out);
int memory_parse_temporal_ref_date(const char *ref_key, const char *granularity,
                                   memory_parsed_date_t *out);
double memory_query_elapsed_ms(const struct timespec *start, const struct timespec *end);
int memory_query_wants_future(const char *norm_query);
int memory_query_wants_past(const char *norm_query);
int memory_query_wants_recent(const char *norm_query);
memory_ranker_input_t memory_ranker_input_from(const memory_t *m);
void memory_record_query_plan_metrics(const memory_query_plan_t *plan, double elapsed_ms);
void memory_record_query_stage_metric(const memory_query_plan_t *plan, const char *stage_name);
int memory_rerank_matches(const char *raw_query, memory_t *matches, int count, int limit,
                          const int64_t *semantic_ids, const double *semantic_scores,
                          int semantic_hit_count, const memory_candidate_source_t *source_stats,
                          int source_stats_count);
double memory_semantic_bonus_lookup(int64_t memory_id, const int64_t *semantic_ids,
                                    const double *semantic_scores, int semantic_hit_count);
double memory_speaker_bonus(int64_t memory_id, const char *raw_query);
double memory_temporal_constraint_score(const memory_temporal_constraint_t *constraint,
                                        const memory_parsed_date_t *candidate);
int memory_temporal_query_has_explicit_date(char qtokens[][64], int nq);
int memory_temporal_ref_matches_query(const char *ref_key, char qtokens[][64], int nq);

/* memory candidate source bit-flags (cross-segment) */
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

int memory_collect_memory_matches_via_vector(const char *query, const char *embed_cmd, int limit,
                                             memory_t *out, int max);
int memory_collect_unit_matches_via_vector(const char *query, const char *embed_cmd, int limit,
                                           memory_t *out, int max);
int memory_generate_candidates(const char *query, const char *norm_query,
                               memory_query_intent_t intent, const memory_query_plan_t *plan,
                               int fetch_limit, memory_t *out, int max, int64_t *semantic_ids,
                               double *semantic_scores, int *semantic_hit_count,
                               memory_candidate_source_t *source_stats, int *source_stats_count);
void memory_note_candidate_sources(memory_candidate_source_t *stats, int *stats_count,
                                   int stats_max, const memory_t *matches, int start, int end,
                                   unsigned int source_mask);
#endif /* MEMORY_CORE_INTERNAL_H */
