#ifndef DEC_MEMORY_H
#define DEC_MEMORY_H 1

typedef struct
{
   int64_t id;
   char tier[4];
   char kind[16];
   char key[512];
   char headline[512];
   char content[2048];
   char use_cases[1024];
   double confidence;
   int use_count;
   char last_used_at[32];
   char created_at[32];
   char updated_at[32];
   char source_session[128];
   double salience;
   char provenance_category[32];
   double retrieval_score; /* deterministic hybrid score captured before CE reorder */
   int hybrid_rank;        /* 1-based deterministic hybrid rank, or 0 when unknown */
} memory_t;

/* Narrow ranking input: only fields needed to compute retrieval order.
 * Confidence-shaped fields (confidence, use_count, lifecycle state) are
 * intentionally absent — they must not influence ranking. After ordering
 * is fixed, callers reattach the full memory_t by id for display/trace. */
typedef struct
{
   int64_t id;
   char kind[16];
   char key[512];
   char content[2048];
   char use_cases[1024];
} memory_ranker_input_t;

typedef struct
{
   char session_id[128];
   int seq;
   char file_path[MAX_PATH_LEN];
   int start_line;
   int end_line;
   char summary[1024];
   double score;
   char files[32][MAX_PATH_LEN];
   int file_count;
} search_result_t;

typedef struct
{
   int64_t id;
   int64_t memory_a;
   int64_t memory_b;
   char detected_at[32];
   int resolved;
   char resolution[64];
} conflict_t;

/* anti_pattern_t lives in db2/anti_patterns.h. The -Idb2 search path
 * (set in the project Makefile) resolves the unqualified include. */
#include "anti_patterns.h"

typedef struct
{
   int tier_counts[6];          /* L0, L1, L2, L3, L4, L5 */
   int kind_counts[KIND_COUNT]; /* fact, pref, decision, episode, task, scratch, procedure, policy
                                 */
   int total;
   int conflicts;
   double pagerank_last_ms;
   double pagerank_avg_ms;
   double pagerank_max_ms;
   int pagerank_samples;
   int pagerank_last_candidates;
   int pagerank_last_edges;
} memory_stats_t;

typedef struct
{
   double lexical;
   double coverage;
   double entity;
   double temporal;
   double evidence;
   double semantic;
   double state;
   double intent;
   double confidence;
   double salience;
   double surprise;
   double pagerank;
   double cross_encoder;  /* raw cross-encoder rerank score (0.0 when disabled/unavailable) */
   double hybrid_total;   /* hybrid score BEFORE cross-encoder blend, for explain surface */
   double blended_total;  /* final score AFTER cross-encoder blend (== total when rerank ran) */
   double rerank_mix;     /* blend weight used (0.0 when rerank did not run) */
   double graph_score;    /* utility-weighted graph boost contribution */
   double code_proximity; /* code-projection edge proximity score */
   double utility;        /* decayed utility signal from feedback */
   double source_fusion;  /* fused graph-vector-code ranking delta */
   double total;
} memory_score_parts_t;

typedef struct
{
   memory_t memory;
   memory_score_parts_t parts;
} memory_diagnostic_t;

#define MEMORY_ANSWER_MAX_CITATIONS 4
#define MEMORY_ANSWER_TRACE_MAX_IDS 16

typedef enum
{
   MEMORY_ANSWER_DECISION_ANSWERABLE = 0,
   MEMORY_ANSWER_DECISION_ABSTAIN,
   MEMORY_ANSWER_DECISION_EXEMPT
} memory_answer_decision_t;

typedef enum
{
   MEMORY_ANSWER_REASON_OK = 0,
   MEMORY_ANSWER_REASON_STRUCTURAL_EMPTY,
   MEMORY_ANSWER_REASON_STRUCTURAL_NO_EXTRACT,
   MEMORY_ANSWER_REASON_CITATION_REQUIRED,
   MEMORY_ANSWER_REASON_GROUNDING_LOW,
   MEMORY_ANSWER_REASON_CHUNK_FLOOR,
   MEMORY_ANSWER_REASON_CURATED_EXEMPT,
   MEMORY_ANSWER_REASON_DB_UNAVAILABLE
} memory_answer_reason_t;

typedef struct
{
   memory_answer_decision_t decision;
   memory_answer_reason_t reason;
   int64_t candidate_ids[MEMORY_ANSWER_TRACE_MAX_IDS];
   int candidate_id_count;
   int trace_truncated;
   int ranked_count;
   int64_t anchor_id;
   int anchor_rank;
   double topk_grounding;
   double anchor_coverage;
   double cluster_coverage;
   double threshold;
   double chunk_floor;
   int structural;
   int exempt;
} memory_answer_evidence_t;

typedef struct
{
   char answer[1024];
   double confidence;
   int no_answer;
   int low_confidence;
   char evidence_mode[16];
   int retrieval_count;
   int citation_count;
   int64_t citation_ids[MEMORY_ANSWER_MAX_CITATIONS];
   memory_answer_evidence_t evidence;
   char error[256];
} memory_answer_result_t;

const char *memory_answer_evidence_decision_str(const memory_answer_evidence_t *trace);
const char *memory_answer_evidence_reason_str(const memory_answer_evidence_t *trace);

typedef enum
{
   MEM_ROUTE_LEXICAL = 0,
   MEM_ROUTE_SEMANTIC,
   MEM_ROUTE_GRAPH,
   MEM_ROUTE_HYBRID
} memory_query_route_t;

typedef enum
{
   MEM_SHAPE_UNKNOWN = 0,
   MEM_SHAPE_FACTOID,
   MEM_SHAPE_LIST,
   MEM_SHAPE_YES_NO,
   MEM_SHAPE_WHEN,
   MEM_SHAPE_HOW,
   MEM_SHAPE_WHY,
   MEM_SHAPE_QUANTITATIVE,     /* "how many", "count", "number of" */
   MEM_SHAPE_TEMPORAL_INTERVAL /* "how long ago", "days between", "how long" */
} memory_query_shape_t;

/* Reason a query was classified as code-shaped (Phase 6). */
typedef enum
{
   CODE_SEED_NONE = 0,   /* not code-shaped */
   CODE_SEED_CALLER = 1, /* explicit caller flag */
   CODE_SEED_TOKEN = 2,  /* file/symbol token heuristic */
   CODE_SEED_VECTOR = 3  /* high-confidence code vector seed */
} code_seed_reason_t;

typedef struct
{
   double lexical_weight;
   double semantic_weight;
   double graph_weight;
   double temporal_weight;
} memory_query_weight_profile_t;

typedef struct
{
   memory_query_route_t route;
   memory_query_shape_t shape;
   memory_query_weight_profile_t weights;
   int fetch_multiplier;
   int min_fetch;
   int max_fetch;
   int graph_hops;
   int semantic_enabled;
   int allow_code_graph;                /* 1 = traversal may enter code subgraphs */
   code_seed_reason_t code_seed_reason; /* which rule opened code traversal */
} memory_query_plan_t;

typedef struct
{
   int64_t id;
   int64_t memory_id;
   char episode_key[256];
   char episode_text[2048];
   char source_session[128];
   char reference_time[32];
   char created_at[32];
} memory_episode_t;

typedef struct
{
   int64_t id;
   int64_t memory_id;
   int64_t episode_id;
   char src_entity[128];
   char relation[64];
   char dst_entity[256];
   char fact_text[1024];
   char valid_at[32];
   char invalid_at[32];
   double weight;
   char created_at[32];
} memory_relation_t;

typedef struct
{
   char entity[128];
   int mention_count;
   int relation_count;
   char latest_episode[256];
   char summary[1024];
} memory_entity_profile_t;

/* Structured profile card stored in the entity_profiles table.
 * Built by aggregating relations, episodes, and entity mentions.
 * card_json holds a self-describing JSON blob with facts, relations,
 * and episode keys that agents can consume directly. */
typedef struct
{
   char entity_id[128]; /* canonical/normalised entity name (PK) */
   char canonical_name[128];
   int observation_count; /* total mentions + relations */
   char card_json[4096];  /* JSON: facts[], relations[], episodes[], summary */
   char last_refreshed[32];
   char created_at[32];
} memory_profile_card_t;

typedef enum
{
   MEMORY_UNIT_KIND_EPISODIC = 0,
   MEMORY_UNIT_KIND_SEMANTIC,
   MEMORY_UNIT_KIND_PROCEDURAL
} memory_unit_kind_t;

#define MEMORY_UNIT_KIND_EPISODIC_STR   "episodic"
#define MEMORY_UNIT_KIND_SEMANTIC_STR   "semantic"
#define MEMORY_UNIT_KIND_PROCEDURAL_STR "procedural"

/* Kind-specific lifecycle configuration (loaded from kind_lifecycle table) */
typedef struct
{
   int promote_use_count;
   double promote_confidence;
   int demote_days;
   double demote_confidence;
   int expire_days;
   double demotion_resistance;
} kind_lifecycle_t;

/* Kind lifecycle configuration is owned by DB2; include db2/kind_lifecycle.h
 * for db2_kind_lifecycle_load. */

/* --- Write Quality Gates --- */

typedef enum
{
   GATE_ACCEPT,    /* write as requested */
   GATE_DOWNGRADE, /* write to L0 scratch instead of requested tier */
   GATE_REDACT,    /* write with sensitive content masked */
   GATE_REJECT     /* do not write */
} gate_result_t;

typedef struct
{
   gate_result_t result;
   char reason[256];
   char redacted_content[2048]; /* set when result == GATE_REDACT */
} gate_verdict_t;

int memory_gate_check(const char *tier, const char *kind, const char *key, const char *content,
                      double confidence, gate_verdict_t *verdict);

/* --- Functional Tier Helpers --- */

/* Returns a numeric priority for a tier string (higher = higher priority in
 * context assembly).  Unknown tiers return 0. */
int memory_tier_priority(const char *tier);

/* Returns the functional name for a tier: "Experience", "Observation",
 * "World", "MentalModel", "Pattern", or "Unknown". */
const char *memory_functional_tier_name(const char *tier);

/* Reclassify L3 directives (kind='policy' or 'workflow') to L4, writing a
 * migration summary to the log.  Safe to call repeatedly (idempotent).
 * Returns number of rows reclassified, or -1 on DB error. */
int memory_reclassify_directives(void);

/* Approval-gated variant: when require_approval is non-zero, policies only
 * promote if a row exists in memory_promotion_approvals.  Workflows bypass
 * the gate (they already require explicit operator approval via
 * store_workflow). */
int memory_reclassify_directives_ex(int require_approval);

/* Promote stable L2 facts/preferences to L3 when confidence >= 0.95,
 * use_count >= 5, and updated_at is older than 30 days.  Returns number of
 * rows promoted, or -1 on DB error.  Safe to call repeatedly. */
int memory_promote_stable_l2_to_l3(void);

/* Synthesize L5 pattern memories from L2 facts observed across >= 3
 * distinct sessions.  Returns number of L5 patterns synthesized, or -1 on
 * DB error.  Safe to call repeatedly. */
int memory_synthesize_l5_patterns(void);

/* Record operator approval for an L3→L4 promotion.  Used by the approval
 * gate and by the `memory approve` CLI / MCP tool. */
int memory_approve_l4_promotion(int64_t memory_id, const char *approver, const char *note);

/* --- Tiered Memory --- */
int memory_insert(const char *tier, const char *kind, const char *key, const char *content,
                  double confidence, const char *session_id, memory_t *out);
int memory_insert_ex(const char *tier, const char *kind, const char *key, const char *content,
                     const char *use_cases, double confidence, const char *session_id,
                     memory_t *out);
int memory_get(int64_t id, memory_t *out);
int memory_touch(int64_t id);
int memory_update_content(int64_t id, const char *content);
int memory_reject(int64_t id, const char *reason);
int memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max);
int memory_delete(int64_t id);
int memory_stats(memory_stats_t *out);
int memory_rebuild_derived_indexes(int limit);
int memory_repair_vector_index(int64_t memory_id, const char *command);
int memory_repair_vector_index_failed_only(const char *command, int limit, int *failed_out);
int memory_rebuild_vector_index_for_version(const char *version, int *failed_out);
int memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max);
int memory_diagnose_scoped(const char *query, const char *scope_type, const char *scope_value,
                           int limit, memory_diagnostic_t *out, int max);
int memory_explain_match(const char *query, int64_t memory_id, memory_diagnostic_t *out);
int memory_ask_query(const char *query, int limit, memory_answer_result_t *out);
int memory_ask_query_scoped(const char *query, const char *scope_type, const char *scope_value,
                            int limit, memory_answer_result_t *out);
char *memory_answer_query(const char *query, int limit);
char *memory_answer_query_scoped(const char *query, const char *scope_type, const char *scope_value,
                                 int limit);
/* Returns 1 if answer contains at least one citation marker ([#N]). */
int memory_citation_gate_check(const char *answer);
int memory_list_episodes(const char *query, int limit, memory_episode_t *out, int max);
int memory_get_episode(const char *episode_key, memory_episode_t *out);
int memory_search_graph(const char *query, int limit, memory_relation_t *out, int max);

/* As-of variant: filter memory_relations to those where valid_at <= as_of and
 * (invalid_at is empty OR invalid_at > as_of). Pass NULL for as_of to get all. */
int memory_search_graph_as_of(const char *query, const char *as_of, int limit,
                              memory_relation_t *out, int max);

int memory_get_entity_profile(const char *entity, memory_entity_profile_t *out);
int memory_get_entity_edges(const char *entity, int limit, memory_relation_t *out, int max);

/* Lineage record: tracks which session/source produced a graph node or edge.
 * object_type: "memory", "relation", or "edge".
 * object_id:   the rowid in the corresponding table.
 * source_kind: "session", "cognify", "extract", or "import".
 * source_ref:  session_id or external ref string. */
typedef struct
{
   int64_t id;
   char object_type[32];
   int64_t object_id;
   char source_kind[32];
   char source_ref[256];
   char ingested_at[32];
   double confidence;
} memory_lineage_t;

/* Insert a lineage record for a newly created node or edge.
 * Returns the new rowid on success, -1 on failure. */
int64_t memory_lineage_insert(const char *object_type, int64_t object_id, const char *source_kind,
                              const char *source_ref, double confidence);

/* Fetch lineage rows for a given object.
 * Returns count written into out (up to max). */
int memory_lineage_get(const char *object_type, int64_t object_id, memory_lineage_t *out, int max);

/* Cite: show provenance chain for a memory ID in human-readable form. */
void memory_cite(int64_t memory_id, int json_out);

/* Profile card API (entity_profiles table).
 *
 * memory_is_profile_query: return 1 if query is profile-shaped.
 * memory_profile_card_build: aggregate entity_edges/memory_entities into a JSON card
 *   and upsert it into entity_profiles. Returns 0 on success, -1 on failure.
 * memory_profile_card_get: load a stored card into memory_profile_card_t.
 *   Returns 0 on success, -1 if not found.
 * memory_profile_card_refresh: refresh cards older than stale_secs for entities
 *   with >= min_obs observations. Returns count of cards refreshed. */
int memory_is_profile_query(const char *query);
int memory_profile_card_build(const char *entity_id, int min_obs, char *out_json,
                              size_t out_json_len);
int memory_profile_card_get(const char *entity, char *out_json, size_t out_json_len);
int memory_profile_card_refresh(int min_obs, int stale_secs);
char *memory_get_context_block(const char *query, const char *block_type, int limit);
int memory_query_plan(const char *query, int limit, int hard_cap, memory_query_plan_t *out);
const char *memory_query_route_name(memory_query_route_t route);
const char *memory_query_shape_name(memory_query_shape_t shape);

/* Combine token-count specificity with shape-aware width into a single
 * scaling factor for the dynamic fetch budget. Returns a positive
 * double; 1.0 means "no scaling". See memory_core_helpers.inc. */
double memory_fetch_budget_factor(memory_query_shape_t shape, int ntokens);

/* --- Aggregation-Aware Query Routing ---
 *
 * Aggregation queries ("list all X", "every Y", "what Xs has ENTITY Ved")
 * want coverage, not similarity ranking.  Detection is structural and
 * cheap: no LLM in the default path.  Callers that want the
 * aggregation route dispatch through memory_aggregate() when the hint
 * reports detected=1.  See docs/proposals/done/aggregation-aware-query-
 * routing.md for design notes.
 */
#define MEMORY_AGGREGATION_MAX_ENTITY        128
#define MEMORY_AGGREGATION_MAX_STATUS        32
#define MEMORY_AGGREGATION_DEFAULT_MAX_ITEMS 200

typedef struct
{
   int detected;        /* 1 if the query matches an aggregation shape */
   int has_quantifier;  /* explicit "all/every/list/enumerate/which" */
   int has_status_word; /* "open/pending/unresolved/past/completed" present */
   char entity_seed[MEMORY_AGGREGATION_MAX_ENTITY]; /* lowercased seed, empty if none */
   char status_word[MEMORY_AGGREGATION_MAX_STATUS]; /* lowercased status keyword */
} memory_aggregation_hint_t;

/* Inspect `query` for an aggregation shape.  Writes detection state into
 * `hint` and returns hint->detected.  Safe to call with NULL/empty query. */
int memory_detect_aggregation_shape(const char *query, memory_aggregation_hint_t *hint);

/* Retrieve an unranked (or recency-ranked) set of memories matching the
 * aggregation hint.  Bypasses vector search entirely — pure SQL against the
 * `memories`/`memory_entities` tables.  When the underlying result set
 * exceeds max_items, writes up to max_items rows and sets *truncated=1.
 *
 * `query` supplies fallback keyword(s) when no entity seed is present; pass
 * the original user query.
 *
 * Returns the number of rows written (>= 0), or -1 on error.  max_items is
 * clamped against the caller-supplied `max` array size and against
 * MEMORY_AGGREGATION_DEFAULT_MAX_ITEMS when <= 0. */
int memory_aggregate(const memory_aggregation_hint_t *hint, const char *query, int max_items,
                     memory_t *out, int max, int *truncated);

/* Promotion/demotion/expiry. Returns count of affected memories. */
int memory_promote(void);
int memory_promote_delegation_patterns(void);
int memory_demote(void);
int memory_expire(void);
int memory_run_maintenance(int *promoted, int *demoted, int *expired);

/* Health metrics: record maintenance cycle stats and prune old data. */
void memory_record_health(int promotions, int demotions, int expirations);
void memory_prune_health(void);

/* Health query: rolling 7-day stats. */
typedef struct
{
   double contradiction_rate; /* contradictions / new memories over 7 days */
   double promotion_rate;     /* promotions / eligible L1 per cycle */
   double demotion_rate;      /* demotions / total L2 per cycle */
   double staleness;          /* % of L2 facts unused in 30+ days */
   int total_contradictions;
   int total_promotions;
   int total_demotions;
   int total_expirations;
   int cycles;
} memory_health_t;

int memory_query_health(memory_health_t *out);

/* Contradiction audit log. */
void memory_log_contradiction(int64_t mem_a, int64_t mem_b, const char *resolution,
                              const char *details);

/* Provenance surfacing. */
typedef struct
{
   int64_t id;
   int64_t memory_id;
   char session_id[64];
   char action[32];
   char details[512];
   char created_at[32];
} provenance_entry_t;

#define MAX_PROVENANCE_ENTRIES 64

int memory_get_provenance(int64_t memory_id, provenance_entry_t *out, int max);
void add_provenance(int64_t memory_id, const char *session_id, const char *action,
                    const char *details);

/* Session folding: compress L0 into L1 checkpoint. */
int memory_fold_session(const char *session_id);

/* --- Search --- */
int memory_search(char **clusters, int cluster_count, int limit, search_result_t *out, int max);
int memory_find_facts(const char *query, int limit, memory_t *out, int max);
int memory_find_facts_scoped(const char *query, const char *scope_type, const char *scope_value,
                             int limit, memory_t *out, int max);
int memory_find_facts_visible(const char *query, const char *workspace, const char *project,
                              int limit, memory_t *out, int max);

/* --- Conversation Scanning --- */
int memory_scan_conversations(char dirs[][MAX_PATH_LEN], int dir_count);

/* --- Window Compaction --- */
int memory_compact_windows(int *summary_count, int *fact_count);

/* --- Workspace Scoping --- */
typedef enum
{
   MEMORY_SCOPE_NONE = 0,
   MEMORY_SCOPE_GLOBAL,
   MEMORY_SCOPE_WORKSPACE,
   MEMORY_SCOPE_PROJECT
} memory_scope_level_t;

typedef struct
{
   char type[16];
   char value[128];
} memory_scope_tag_t;

const char *memory_scope_level_name(memory_scope_level_t level);
int memory_tag_global(int64_t memory_id);
int memory_tag_project(int64_t memory_id, const char *project);
int memory_tag_workspace(int64_t memory_id, const char *workspace);
int memory_auto_tag_workspace(int64_t memory_id, const char *key, const char *content);
int memory_tag_scope(int64_t memory_id, const char *scope_type, const char *scope_value);
int memory_collect_scopes(int64_t memory_id, memory_scope_tag_t *out, int max);
memory_scope_level_t memory_primary_scope(int64_t memory_id, char *value, size_t value_len);
int memory_scope_visibility_rank(int64_t memory_id, const char *workspace, const char *project);

/* --- Canonical filter / scope contract (memory-public-contract) --- */

/* How broadly the scope lattice is applied. */
typedef enum
{
   MEMORY_VISIBILITY_DEFAULT = 0, /* normal lattice — project > workspace > global */
   MEMORY_VISIBILITY_STRICT,      /* only the exact named scope(s), no lattice promotion */
   MEMORY_VISIBILITY_EXPANDED,    /* include parent scopes up to global */
} memory_visibility_t;

/* One axis of the four-dimensional scope spec.
 * "current" = derive from session context; "" = omit that axis. */
typedef struct
{
   char session[128];
   char project[128];
   char workspace[128];
   char user[128];
} memory_scope_spec_t;

/* Canonical caller-facing filter object.  Produced by
 * memory_filter_from_opts() / memory_filter_from_scope(); consumed by
 * retrieval helpers and surfaced in --explain output. */
typedef struct
{
   memory_scope_spec_t scope;
   memory_visibility_t visibility;
   char tiers[6][4]; /* e.g. "L1", "L2"; empty entry terminates */
   int tier_count;
   char kinds[8][16]; /* e.g. "fact", "decision"; empty entry terminates */
   int kind_count;
   char as_of[32]; /* ISO-8601 timestamp or "" */
} memory_filter_t;

/* Resolve a (scope_type, scope_value) pair into a memory_filter_t.
 * scope_type / scope_value may be NULL (ambient context only). */
void memory_filter_from_scope(const char *scope_type, const char *scope_value,
                              memory_filter_t *out);

/* Serialize a filter to a fresh cJSON object.  Caller owns it. */
struct cJSON *memory_filter_to_json(const memory_filter_t *f);

/* --- Effective importance (memory-public-contract) --- */

/* Compute the effective importance of a memory for ranking / explain.
 * Formula: base × kind_decay(age) × bounded_reinforcement(use_count)
 * with a freshness floor for rows < 24h old.
 * now_sec: current epoch-seconds (0 = use time(NULL)).
 * Returns a value in (0, 1]. */
double memory_effective_importance(const memory_t *m, time_t now_sec);

/* --- Workflow Learning --- */

/* Upsert a project workflow memory (kind=workflow) scoped to a workspace.
 * Key format: workflow:{workspace}:{signal_type}. Content is the rule text.
 * Repeat observations merge into the existing row and bump confidence toward
 * 1.0 (capped). Returns the memory id on success, -1 on failure. */
int64_t memory_upsert_workflow(const char *workspace, const char *signal_type, const char *rule,
                               double observed_confidence, const char *session_id);

/* Observe a Bash command and, if it carries a learnable workflow signal,
 * upsert a workflow memory tagged to the workspace matching the current
 * working directory. Silent no-op when no signal is detected, no workspace
 * matches, or the DB write fails. */
void workflow_observe_bash(const char *command);

/* --- HyDE and Query Decomposition --- */

#define MEMORY_REWRITE_MAX_SUBQUERIES 4

/* Result of a pre-retrieval query rewrite.
 * has_hyde: 1 if hyde_answer was generated.
 * has_decomp: 1 if sub_questions were generated.
 * Zero-init = no-op (rewrite disabled or failed). */
typedef struct
{
   int has_hyde;
   char hyde_answer[1024];
   int has_decomp;
   int sub_question_count;
   char sub_questions[MEMORY_REWRITE_MAX_SUBQUERIES][512];
} memory_query_rewrite_t;

/* Call the external rewrite command (memory.rewrite.command) and parse results
 * into out. Silently no-ops if rewriting is disabled or the command fails.
 * cfg must be loaded by the caller. */
void memory_query_rewrite(const char *query, const config_t *cfg, memory_query_rewrite_t *out);

/* --- Negation and Absence Memory --- */

typedef enum
{
   POLARITY_POSITIVE = 0,
   POLARITY_NEGATIVE = 1
} polarity_t;

/* Return 1 if the lowercased word is a negation marker
 * ("not", "never", "no", "without", "haven't", "hasn't", "didn't",
 *  "doesn't", "can't", "won't", "neither", "nor"). */
int is_negation_marker(const char *word);

/* Scan text and produce space-separated "not_<token>" synthetic terms for
 * any content token that falls within ±3 tokens of a negation marker,
 * stopping at simple clause boundaries (.,!?;).  Tokens shorter than
 * 3 characters and stopwords are skipped.
 * buf is written as a NUL-terminated string; at most buf_len-1 chars.
 * Returns the number of synthetic tokens written. */
int extract_negation_tokens(const char *text, char *buf, size_t buf_len);

/* Classify the polarity of a query string using the same heuristic.
 * Returns POLARITY_NEGATIVE if a negation marker is detected, else
 * POLARITY_POSITIVE. */
polarity_t memory_query_polarity(const char *query);

/* Conversational window expansion: for each result with a source_session,
 * fetch up to window_radius neighbours (earlier and later memories from the
 * same session, ordered by id) and inject them into out[] without duplicates.
 * Returns the new count (may be larger than the input count, capped at max). */
int memory_expand_to_session_window(memory_t *out, int count, int max, int window_radius);

/* --- Retrieval Planner --- */

typedef enum
{
   INTENT_DEBUG = 0,
   INTENT_PLAN,
   INTENT_REVIEW,
   INTENT_DEPLOY,
   INTENT_GENERAL
} task_intent_t;

#define NUM_KINDS 8 /* fact, pref, decision, episode, task, scratch, procedure, policy */

typedef struct
{
   task_intent_t intent;
   double kind_budget[NUM_KINDS]; /* fraction per kind (sums to ~1.0) */
   int include_l3;                /* include L3 failure episodes */
   double recency_weight;         /* 0.0=no bias, 1.0=strongly prefer recent */
} retrieval_plan_t;

task_intent_t classify_intent(const char *task_hint);
void retrieval_plan_for_intent(task_intent_t intent, retrieval_plan_t *plan);

/* Return 1 if task_hint is a session-shaped query (e.g. "what happened",
 * "how did the trip go", "recap"), 0 otherwise.  Session-shaped queries
 * should preferentially surface episode cards. */
int memory_is_session_query(const char *task_hint);

/* --- Quantitative / Date-Arithmetic Deriver --- */

/* A single derived fact produced by the post-retrieval deriver. */
typedef struct
{
   char label[128]; /* human-readable label, e.g. "Count of camping trips" */
   char value[64];  /* the computed value, e.g. "3" or "42 days" */
} memory_derived_fact_t;

#define MEMORY_DERIVED_MAX_FACTS 32

/* Container for all derived facts from one deriver run. */
typedef struct
{
   memory_derived_fact_t facts[MEMORY_DERIVED_MAX_FACTS];
   int count;
} memory_derived_facts_t;

/* Classify the query shape for |query|.  Returns MEM_SHAPE_QUANTITATIVE,
 * MEM_SHAPE_TEMPORAL_INTERVAL, or MEM_SHAPE_UNKNOWN if the query does not
 * match a deriver-eligible pattern. */
memory_query_shape_t memory_classify_deriver_shape(const char *query);

/* Post-retrieval deriver: scan |candidates| (up to |cand_count| memories) for
 * typed edge data (OCCURRED_AT, valid_from, etc.) and compute counts, date
 * diffs, and aggregated intervals relevant to |query|.
 * Results are written into |out|.  Returns the number of derived facts, or 0
 * if the deriver is disabled / inapplicable. */
int memory_derive_facts(const char *query, int64_t *candidate_ids, int cand_count,
                        const config_t *cfg, memory_derived_facts_t *out);

/* Format |facts| as a compact JSON string (caller must free). */
char *memory_derived_facts_to_json(const memory_derived_facts_t *facts);

/* --- Retrieval-Failure Detection --- */

/* Summary of retrieval confidence computed over the top-K candidates.
 * coverage: fraction of non-trivial query tokens present in any top-K hit.
 * separation: normalised score gap between top-1 and the third hit (or 0.0
 *   if fewer than two candidates exist).
 * score: blended confidence [0.0, 1.0].
 * below_threshold: 1 when score < the configured threshold.
 * fallback_triggered: 1 when a broader re-fetch was performed. */
typedef struct
{
   double coverage;
   double separation;
   double score;
   int below_threshold;
   int fallback_triggered;
} retrieval_confidence_t;

/* Compute retrieval confidence for |candidates| (|cand_count| entries) against
 * |query_terms| (|term_count| entries).  Writes results into |out|.
 * |threshold| is the configured failure threshold (e.g. 0.35). */
void memory_retrieval_confidence(const char **query_terms, int term_count,
                                 const void *candidates_opaque, int cand_count, double threshold,
                                 retrieval_confidence_t *out);

/* --- Context Assembly --- */

/* Per-item entry returned by memory_assemble_context_explain(). */
#define CONTEXT_EXPLAIN_REASON_LEN 80
typedef struct
{
   int64_t id;
   char tier[4];
   char kind[16];
   char key[256];
   char scope[32]; /* "project", "workspace", "global", or "" */
   double score;
   int tokens; /* estimated token cost of this item */
   double score_per_token;
   int selected; /* 1 = included in context, 0 = rejected */
   char rejection_reason[CONTEXT_EXPLAIN_REASON_LEN];
} context_assemble_explain_entry_t;

/* Budget metrics emitted as part of explain output. */
typedef struct
{
   int budget_tokens;
   int used_tokens;
   int rejected_for_budget; /* items excluded because budget was full */
} context_budget_metrics_t;

char *memory_assemble_context(const char *task_hint);
char *memory_assemble_context_ws(const char *task_hint, const char *workspace);

/* Like memory_assemble_context but additionally populates explain[] with
 * per-candidate selection details and metrics.  Pass NULL/0 to skip explain. */
char *memory_assemble_context_explain(const char *task_hint,
                                      context_assemble_explain_entry_t *explain, int *explain_count,
                                      int explain_max, context_budget_metrics_t *metrics);

/* --- Graph Boost (for context scoring) --- */
#define MAX_BOOST_ENTRIES 256

typedef struct
{
   char entity[128];
   double score;
} boost_entry_t;

typedef struct
{
   boost_entry_t entries[MAX_BOOST_ENTRIES];
   int count;
} boost_map_t;

void memory_graph_boost(char **query_terms, int term_count, boost_map_t *out);

/* Context cache storage lives in src/db1/caches.h (db1_context_cache_*). */
char *cache_input_hash(char *buf, size_t buf_len);

/* --- Conflict Detection --- */
int64_t memory_detect_conflict(const char *key, const char *content);
int memory_record_conflict(int64_t mem_a, int64_t mem_b);
int memory_list_conflicts(conflict_t *out, int max);
int memory_resolve_conflict(int64_t conflict_id, const char *resolution);
int memory_scan_retroactive_conflicts(void);

/* --- L3 Failure Episodes --- */
int memory_synthesize_failure_episodes(void);

/* --- Anti-Patterns ---
 * Storage primitives (insert/list/check/bump/delete/exists_*) live in
 * db2/anti_patterns.{h,c} as db2_anti_pattern_*. The high-level extraction
 * and escalation passes below are implemented in memory_advanced.c. */
int anti_pattern_extract_from_feedback(void);
int anti_pattern_extract_from_failures(void);

/* Escalate high-hit anti-patterns to hard directive rules. */
int anti_pattern_escalate(int hit_threshold);

/* --- Temporal Facts --- */
int memory_supersede(int64_t old_id, const char *new_content, double confidence,
                     const char *session_id, memory_t *out);
int memory_fact_history(const char *key, memory_t *out, int max);

/* --- Drift Detection --- */
typedef struct
{
   int drifted;
   int64_t task_id;
   char task_title[256];
   char message[512];
} drift_result_t;

int memory_check_drift(int64_t task_id, const char *file_path, const char *command,
                       drift_result_t *out);

/* --- Style Learning --- */
int memory_learn_style(void);

/* --- Graph --- */

/* Maximum byte length of a canonical graph endpoint (file:*, symbol:*, concept:*, etc.).
 * All fixed endpoint-carrying graph structs must use this constant before any
 * file:* or symbol:* projection is enabled. */
#define GRAPH_ENDPOINT_MAX 512

typedef struct
{
   int64_t id;
   char source[GRAPH_ENDPOINT_MAX];
   char relation[32];
   char target[GRAPH_ENDPOINT_MAX];
   int weight;
} edge_t;

int memory_extract_edges(int64_t window_id, char **file_refs, int file_count, char **terms,
                         int term_count);
int memory_query_edges(const char *entity, edge_t *out, int max);

/* Graph-powered related memory retrieval: given seed memory keys, walk
 * co_discussed edges (1-hop) and return related memory IDs scored by weight.
 * Returns count of related memories found (up to max). */
typedef struct
{
   int64_t id;
   char key[512];
   char content[2048];
   double score;
} graph_related_t;
_Static_assert(sizeof(((graph_related_t *)0)->key) >= GRAPH_ENDPOINT_MAX,
               "graph_related_t.key must be at least GRAPH_ENDPOINT_MAX bytes");

int memory_graph_related(char **seed_keys, int seed_count, graph_related_t *out, int max);

/* --- Typed graph walk (ontology-filtered) --- */

/* One hop entry returned by memory_graph_walk(). */
typedef struct
{
   char source[GRAPH_ENDPOINT_MAX];
   char relation[64];
   char target[GRAPH_ENDPOINT_MAX];
   int relation_id;  /* memory_relation_kind_t integer code */
   int subject_kind; /* memory_node_kind_t integer code */
   int object_kind;  /* memory_node_kind_t integer code */
   int weight;
   int hop; /* 1-based hop index from seed */
} graph_walk_entry_t;

/* Walk entity_edges BFS from |seed_entity|, up to |max_hops| hops, filtering
 * by |relation_mask| (bitmask of memory_relation_kind_t bits; use
 * RELATION_MASK_ALL for all relations).  Returns count of entries written. */
int memory_graph_walk(const char *seed_entity, unsigned int relation_mask, int max_hops,
                      graph_walk_entry_t *out, int max);

/* Prune edges where both source and target have no corresponding L1+ memory. */
int memory_graph_prune(void);

/* Normalize edge weights per relation type so max weight is 1.0. */
int memory_graph_normalize(void);

int memory_embed(int64_t memory_id, const char *command);
int memory_embed_text(const char *text, const char *command, float *out, int max_dim);
/* Deep tier: re-embed one memory with the deep model into embedding_deep. */
int memory_embed_deep(int64_t memory_id, const char *command);
/* Deep tier: backfill up to max_rows memories lacking a deep embedding (one
 * throttled batch). Returns the number deep-embedded. */
int memory_deep_backfill(const char *command, int max_rows);
double cosine_similarity(const float *a, const float *b, int dim);

/* --- Effectiveness Tracking --- */

typedef struct
{
   int64_t memory_id;
   int times_surfaced;
   int success_present;
   int failure_present;
   double effectiveness;
} memory_effectiveness_t;

/* Compute effectiveness scores for all memories with enough data. Returns count updated. */
int memory_compute_effectiveness(void);

/* Demote memories with low effectiveness. Returns count demoted. */
int memory_demote_low_effectiveness(void);

/* Get effectiveness stats for display */
typedef struct
{
   double avg_effectiveness;
   int low_effectiveness_count;
   int high_impact_count;
   int never_surfaced_l2;
} effectiveness_stats_t;

int memory_effectiveness_stats(effectiveness_stats_t *out);
/* --- Memory-to-Memory Linking --- */
typedef struct
{
   int64_t id;
   int64_t source_id;
   int64_t target_id;
   char relation[32];
   char created_at[32];
} memory_link_t;

int memory_link_create(int64_t source_id, int64_t target_id, const char *relation);
int memory_link_query(int64_t memory_id, memory_link_t *out, int max);
int memory_link_delete(int64_t link_id);
/* --- Content Safety --- */

#define SCAN_BLOCK    0 /* never persist */
#define SCAN_REDACT   1 /* persist with value masked */
#define SCAN_CLASSIFY 2 /* persist but mark sensitive */

/* Scan content for sensitive data. Returns sensitivity class ("normal", "sensitive", "restricted").
 * If action is SCAN_BLOCK, returns NULL (caller must reject).
 * If action is SCAN_REDACT, modifies content in-place. */
const char *memory_scan_content(char *content, size_t content_len);

/* Enforce retention policies: delete expired sensitive/restricted memories */
int memory_enforce_retention(void);

/* --- Memory Improve Loop --- */

/* Deduplicate memories with identical or near-identical keys.  For each key
 * collision, the lower-confidence duplicate is marked merged_into=canonical_id.
 * Returns the number of merged rows; -1 on error. */
int memory_improve_dedupe(int dry_run);

/* Summarise clusters of low-confidence L1 memories sharing entity tags.
 * Groups are superseded by a synthetic L2 observation.
 * Returns the number of summary memories created; -1 on error. */
int memory_improve_summarise(int dry_run, int min_cluster_size, double max_confidence);

/* Apply correctness feedback to edge weights.  When success=1, increment
 * utility_score on entity_edges touching the cited memories.  When success=0,
 * decrement and write a REL_CORRECTED_BY relation in memory_relations.
 * Returns 0 on success, -1 on error. */
int memory_apply_feedback(int success, int64_t *citation_ids, int citation_count);

/* Phase 7: apply correctness feedback distributed across a retrieval path.
 * The total |delta| (+0.1 success / -0.1 failure) is split across the path
 * edges by relation gravity and hop decay (memory_graph_distribute_path_credit),
 * so a directly-cited edge gets the full delta and a 2-hop bridge gets less.
 * |path_node_keys| are the canonical graph node keys along the path (seed→hit),
 * |relations| the relation label of each hop, |hops| the 1-based hop index.
 * When path_len<=0 this falls back to memory_apply_feedback() semantics for
 * the cited keys.  Returns 0 on success, -1 on error. */
int memory_apply_feedback_path(int success, const char **path_node_keys, const char **relations,
                               const int *hops, int path_len);

/* --- LLM-Driven Cognification --- */

/* A single extracted claim from the cognifier. */
typedef struct
{
   char subject[128];
   char attribute[128];
   char value[512];
   char kind[16]; /* "fact", "opinion", "preference" */
} cognify_claim_t;

/* A single extracted S-R-O relation from the cognifier. */
typedef struct
{
   char src_entity[128];
   char relation[64];
   char dst_entity[128];
   char fact_text[512];
} cognify_relation_t;

#define COGNIFY_MAX_CLAIMS    16
#define COGNIFY_MAX_RELATIONS 16
#define COGNIFY_MAX_ENTITIES  16

/* A single coreference binding extracted by the cognifier.
 * confidence < 0.5 should be skipped (ambiguous). */
typedef struct
{
   char pronoun[16];  /* e.g. "she", "they" */
   char entity[128];  /* resolved entity name, or "" if uncertain */
   double confidence; /* 0.0–1.0; < 0.5 means skip */
} cognify_coref_binding_t;

#define COGNIFY_MAX_COREF 8

/* Result of parsing a cognifier LLM response. */
typedef struct
{
   char summary[512];
   char memory_kind[16];
   cognify_claim_t claims[COGNIFY_MAX_CLAIMS];
   int claim_count;
   cognify_relation_t relations[COGNIFY_MAX_RELATIONS];
   int relation_count;
   cognify_coref_binding_t coref_bindings[COGNIFY_MAX_COREF];
   int coref_count;
} memory_cognify_result_t;

/* Call the configured cognifier command (cfg->memory_cognify_command) with the
 * given memory text and write extracted triples into memory_relations and
 * claims as new memories.  Returns 0 on success, -1 on error or if cognification
 * is disabled.  When db is non-NULL, extracted relations are persisted. */
int memory_cognify_unit(int64_t memory_id, const char *text, const config_t *cfg,
                        memory_cognify_result_t *out);

/* Parse a raw JSON string (as returned by the cognifier command) into a
 * memory_cognify_result_t.  Returns 0 on success, -1 on parse error. */
int memory_cognify_parse_response(const char *json, memory_cognify_result_t *out);

/* --- Async Cognification Job Queue --- */

typedef struct
{
   int pending;
   int running;
   int done;
   int failed;
   int retried;
   int total;
   int processed;
} memory_cognify_queue_stats_t;

/* Query job counts by status.  Returns 0 on success. */
int memory_cognify_queue_status(memory_cognify_queue_stats_t *out);

/* Drain all pending cognification jobs, processing each via
 * memory_cognify_unit().  Jobs that fail are retried up to max_attempts
 * before being marked 'failed'.  Writes aggregate stats to *out (if non-NULL).
 * timeout_secs: stop after this many seconds (0 = run until queue empty).
 * Returns 0 on success, -1 on fatal error. */
int memory_cognify_drain(const config_t *cfg, int timeout_secs, memory_cognify_queue_stats_t *out);

/* --- Per-Session Episode Cards --- */

/* Structured episode card returned by the LLM cognifier for a closed session. */
typedef struct
{
   char session_id[128];
   char title[256];
   char participants[512]; /* comma-separated list */
   char places[256];       /* comma-separated list */
   char events[1024];      /* newline-separated list */
   char outcomes[512];     /* newline-separated list */
   char open_threads[512]; /* newline-separated list */
} memory_episode_card_t;

/* Generate a structured episode card for |source_session| by calling the
 * cognifier command with the session's memories as input.  If generation
 * succeeds the card is stored as a memory_unit with is_episode_card=1 and
 * REL_SUMMARISES edges pointing to each constituent memory.
 * Returns the new memory_unit id on success, 0 on error or if disabled. */
int64_t memory_episode_card_generate(const char *source_session, const config_t *cfg);

/* Parse a raw episode-card JSON string into a memory_episode_card_t.
 * Returns 0 on success, -1 on parse error or missing title. */
int memory_episode_card_parse(const char *json, memory_episode_card_t *out);

/* Query episode cards for |source_session|.  Fills |out| with up to |max|
 * records (memory content).  Returns number of records written. */
int memory_episode_cards_query(const char *source_session, char **out, int max);

/* --- Scene Clustering --- */

/* K-means clustering of memory unit embeddings.
 * Uses pgvector memory unit vectors, runs k-means.
 * Returns number of scenes created, -1 on error.
 * workspace_id: empty string means all workspaces. */
int memory_cluster_scenes(const char *workspace_id);

/* Assign a single memory to the nearest existing scene.
 * Called after a new memory is embedded.  No-op if no scenes exist.
 * Returns 0 on success. */
int memory_assign_scene(int64_t memory_id);

/* In-process coreference resolution counters.
 * Incremented each time memory_coref_audit_record() fires.
 * Thread-safe; reset with memory_coref_stats_reset(). */
typedef struct
{
   int64_t bound;
   int64_t unbound;
   int64_t ambiguous;
} memory_coref_stats_t;

void memory_coref_stats(memory_coref_stats_t *out);
void memory_coref_stats_reset(void);

/* --- Session Briefing ---
 *
 * Assemble a compact, deterministic start-of-session context bundle.  Returns
 * a new cJSON object with three arrays: key_facts, recent_activity,
 * active_entities.  All ranking is DB-side; no LLM calls.  Caller owns the
 * returned cJSON*.  Returns NULL on allocation failure.
 *
 * limit_tokens is an approximate character budget (1 token ~= 4 chars) that
 * caps the rendered payload.  Sections are filled in priority order
 * (key_facts > recent_activity > active_entities); later sections are
 * truncated if the running total crosses the budget.  Section-internal
 * ordering is deterministic so two runs against a frozen DB produce
 * byte-identical bundles.
 *
 * Default sizing: MEMORY_BRIEFING_DEFAULT_LIMIT_TOKENS ~= 1500. */
#define MEMORY_BRIEFING_DEFAULT_LIMIT_TOKENS 1500
#define MEMORY_BRIEFING_MIN_LIMIT_TOKENS     64
#define MEMORY_BRIEFING_MAX_LIMIT_TOKENS     8192

struct cJSON *memory_briefing(int limit_tokens);

/* --- Prospective Memory and Triggered Recall ---
 *
 * First-class "when X, surface Y" records.  Lives in its own table so the
 * pre-turn matcher can scan armed items cheaply without muddying normal fact
 * recall.  Matcher is purely lexical — trigger text plus exact
 * entity/file anchor matches — so it runs in the hot path with no LLM cost.
 *
 * State machine: `armed` -> `triggered` -> (`completed` | `expired`).
 * Recurrence: `once` flips to `triggered` on first match; `repeat` stays
 * armed indefinitely.  The lifecycle helpers below handle state transitions. */
#define MEMORY_PROSPECTIVE_TRIGGER_LEN 512
#define MEMORY_PROSPECTIVE_ACTION_LEN  1024
#define MEMORY_PROSPECTIVE_ANCHOR_LEN  128
#define MEMORY_PROSPECTIVE_STATE_LEN   16
#define MEMORY_PROSPECTIVE_RECUR_LEN   16
#define MEMORY_PROSPECTIVE_TS_LEN      32
#define MEMORY_PROSPECTIVE_MAX_MATCHES 8

#define MEMORY_PROSPECTIVE_STATE_ARMED     "armed"
#define MEMORY_PROSPECTIVE_STATE_TRIGGERED "triggered"
#define MEMORY_PROSPECTIVE_STATE_COMPLETED "completed"
#define MEMORY_PROSPECTIVE_STATE_EXPIRED   "expired"

#define MEMORY_PROSPECTIVE_RECUR_ONCE   "once"
#define MEMORY_PROSPECTIVE_RECUR_REPEAT "repeat"

typedef struct
{
   int64_t id;
   char trigger_text[MEMORY_PROSPECTIVE_TRIGGER_LEN];
   char action_text[MEMORY_PROSPECTIVE_ACTION_LEN];
   char anchor_entity[MEMORY_PROSPECTIVE_ANCHOR_LEN];
   char anchor_file[MEMORY_PROSPECTIVE_ANCHOR_LEN];
   char recurrence[MEMORY_PROSPECTIVE_RECUR_LEN];
   char state[MEMORY_PROSPECTIVE_STATE_LEN];
   char valid_until[MEMORY_PROSPECTIVE_TS_LEN];
   char source_session[128];
   int trigger_count;
   char last_triggered_at[MEMORY_PROSPECTIVE_TS_LEN];
   char created_at[MEMORY_PROSPECTIVE_TS_LEN];
   char updated_at[MEMORY_PROSPECTIVE_TS_LEN];
} memory_prospective_t;

/* Create a new armed prospective memory.  `recurrence` defaults to
 * MEMORY_PROSPECTIVE_RECUR_ONCE when NULL/empty.  `valid_until` may be empty
 * to mean "no expiry".  On success, out (if non-NULL) is populated with the
 * new row.  Returns 0 on success, -1 on error. */
int memory_prospective_create(const char *trigger_text, const char *action_text,
                              const char *anchor_entity, const char *anchor_file,
                              const char *recurrence, const char *valid_until,
                              const char *source_session, memory_prospective_t *out);

/* List reminders matching the supplied state (NULL/empty = all states).
 * Newest-first by created_at.  Returns count written (<= max). */
int memory_prospective_list(const char *state, memory_prospective_t *out, int max);

/* Fetch a single reminder by id. Returns 0 on success, -1 if not found. */
int memory_prospective_get(int64_t id, memory_prospective_t *out);

/* Transition a reminder to `completed`.  Returns 0 on success, -1 if the row
 * is missing or already terminal. */
int memory_prospective_complete(int64_t id);

/* Mark any armed reminders whose valid_until has passed as `expired`.
 * Returns the number of rows updated. */
int memory_prospective_sweep_expired(void);

/* Run the pre-turn matcher against the current user turn and active entity /
 * file anchors.  Returns up to max armed-and-unexpired reminders, in
 * descending match-strength order (exact entity/file anchors first, then lexical
 * hits, then content substring matches).  Recurrence handling is the
 * caller's job: after surfacing, call memory_prospective_mark_triggered()
 * with the ids that should flip state. */
int memory_prospective_match(const char *turn_text, const char *active_entity,
                             const char *active_file, memory_prospective_t *out, int max);

/* Record a trigger event: increments trigger_count and last_triggered_at; for
 * `once` reminders, transitions state to `triggered`.  `repeat` reminders
 * stay `armed`.  Returns 0 on success, -1 on error. */
int memory_prospective_mark_triggered(int64_t id);

/* Read current process-local prospective-memory metrics.  Any out-parameter
 * may be NULL.  Counts are cumulative since process start. */
void memory_prospective_metrics(int64_t *triggered_total, int64_t *completed_total,
                                int64_t *expired_total, int64_t *match_calls, double *match_ms_avg,
                                double *match_ms_max);

/* --- Epistemic Directives and Active Clarification ---
 *
 * Durable "we know this gap exists, ask when relevant, mark resolved
 * once the answer lands" records.  Auto-created from contradictions
 * and repeated retrieval failures; surfaced into recall when a turn
 * is topic-relevant.  State machine:
 *   open -> resolved | suppressed | expired
 *   resolved/suppressed/expired -> (terminal)
 *
 * See docs/proposals/done/epistemic-directives-and-active-clarification.md. */
#define MEMORY_DIRECTIVE_QUESTION_LEN 512
#define MEMORY_DIRECTIVE_TOPIC_LEN    128
#define MEMORY_DIRECTIVE_ANCHOR_LEN   128
#define MEMORY_DIRECTIVE_STATE_LEN    16
#define MEMORY_DIRECTIVE_CAUSE_LEN    32
#define MEMORY_DIRECTIVE_EVIDENCE_LEN 512
#define MEMORY_DIRECTIVE_TS_LEN       32
#define MEMORY_DIRECTIVE_MAX_MATCHES  4

#define MEMORY_DIRECTIVE_STATE_OPEN       "open"
#define MEMORY_DIRECTIVE_STATE_SUPPRESSED "suppressed"
#define MEMORY_DIRECTIVE_STATE_RESOLVED   "resolved"
#define MEMORY_DIRECTIVE_STATE_EXPIRED    "expired"

#define MEMORY_DIRECTIVE_CAUSE_CONTRADICTION     "contradiction"
#define MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE "retrieval_failure"
#define MEMORY_DIRECTIVE_CAUSE_MISSING_CONFIG    "missing_config"
#define MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP    "user_follow_up"

#define MEMORY_DIRECTIVE_RETRIEVAL_FAILURE_THRESHOLD_DEFAULT 3

typedef struct
{
   int64_t id;
   char question[MEMORY_DIRECTIVE_QUESTION_LEN];
   char topic[MEMORY_DIRECTIVE_TOPIC_LEN];
   char anchor_entity[MEMORY_DIRECTIVE_ANCHOR_LEN];
   char anchor_file[MEMORY_DIRECTIVE_ANCHOR_LEN];
   char cause[MEMORY_DIRECTIVE_CAUSE_LEN];
   int priority;
   char state[MEMORY_DIRECTIVE_STATE_LEN];
   int64_t memory_a_id;
   int64_t memory_b_id;
   int64_t resolution_memory_id;
   char evidence[MEMORY_DIRECTIVE_EVIDENCE_LEN];
   char source_session[128];
   int surfaced_count;
   char last_surfaced_at[MEMORY_DIRECTIVE_TS_LEN];
   char resolved_at[MEMORY_DIRECTIVE_TS_LEN];
   char valid_until[MEMORY_DIRECTIVE_TS_LEN];
   char created_at[MEMORY_DIRECTIVE_TS_LEN];
   char updated_at[MEMORY_DIRECTIVE_TS_LEN];
} memory_directive_t;

/* Create a new open directive.  Dedup-safe:
 *   cause='contradiction'     → unique on (memory_a_id, memory_b_id)
 *   cause='retrieval_failure' → unique on (cause, topic)
 *   cause='missing_config'    → unique on (cause, topic)
 * Returns 0 on success (new or pre-existing row; out populated on success),
 * -1 on error, +1 when dedup rejected the insert. */
int memory_directive_create(const char *question, const char *topic, const char *anchor_entity,
                            const char *anchor_file, const char *cause, int priority,
                            int64_t memory_a_id, int64_t memory_b_id, const char *evidence,
                            const char *source_session, const char *valid_until,
                            memory_directive_t *out);

/* List directives matching |state| (NULL/empty = all) and |cause| (NULL/empty
 * = all), priority DESC then created DESC.  Returns count written. */
int memory_directive_list(const char *state, const char *cause, memory_directive_t *out, int max);

int memory_directive_get(int64_t id, memory_directive_t *out);

/* Transition an open directive to resolved, optionally linking the memory
 * whose content answered the question.  Returns 0, -1 if not open. */
int memory_directive_resolve(int64_t id, int64_t resolution_memory_id, const char *note);

/* Suppress an open directive (operator says "stop asking").  Returns 0, -1
 * if not open. */
int memory_directive_suppress(int64_t id);

/* Sweep open directives whose valid_until has passed into state='expired'.
 * Idempotent.  Returns count transitioned. */
int memory_directive_sweep_expired(void);

/* JSON envelope helpers used by the aimee-kb RPC seam (kb_service.c, the
 * cmd_memory_core directive handlers, and dashboard code).  to_json returns
 * a heap-allocated cJSON the caller owns; from_json populates `out` from an
 * object of the same shape and returns 0 on success, -1 on malformed input. */
struct cJSON *memory_directive_to_json(const memory_directive_t *d);
int memory_directive_from_json(const struct cJSON *obj, memory_directive_t *out);

/* Match-on-turn: pick the most relevant open directives for the supplied
 * turn text / active anchors.  Three stages:
 *   1. exact anchor_entity match
 *   2. exact anchor_file match
 *   3. lexical match on question+topic
 * Orders by priority DESC within each stage.  Returns count written. */
int memory_directive_match(const char *turn_text, const char *active_entity,
                           const char *active_file, memory_directive_t *out, int max);

/* Record a surface event: increments surfaced_count and last_surfaced_at.
 * Called by the recall integration once the directive is actually emitted
 * into the bundle. */
int memory_directive_mark_surfaced(int64_t id);

/* Record a retrieval failure against a normalised query key.  When the
 * rolling count crosses |threshold|, auto-creates a retrieval_failure
 * directive and returns the new directive id; otherwise returns 0. */
int64_t memory_directive_record_retrieval_failure(const char *query_norm, int threshold,
                                                  const char *source_session);

/* Auto-create a contradiction directive keyed to (memory_a_id, memory_b_id).
 * Caller should supply the two memory contents for a human-readable
 * question. Idempotent on the unique dedup index. Returns the directive id,
 * 0 if dedup rejected, -1 on error. */
int64_t memory_directive_record_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                              const char *topic, const char *anchor_entity,
                                              const char *content_a, const char *content_b,
                                              const char *source_session);

/* Resolve open contradiction directives linked to this pair of memories. */
int memory_directive_resolve_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                           int64_t resolution_memory_id, const char *note);

/* Counts per state, written into |out|. */
typedef struct
{
   int64_t open;
   int64_t suppressed;
   int64_t resolved;
   int64_t expired;
} memory_directive_counts_t;

int memory_directive_counts(memory_directive_counts_t *out);

/* Process-local metrics accessor.  Any output pointer may be NULL. */
void memory_directive_metrics(int64_t *created_total, int64_t *resolved_total,
                              int64_t *expired_total, int64_t *surfaced_total, int64_t *match_calls,
                              double *match_ms_avg, double *match_ms_max);

/* --- Memory Lifecycle States and Alerts ---
 *
 * Explicit lifecycle column on `memories`:
 *   active      -- currently true (default for declarative facts)
 *   pending     -- commitment awaiting fulfilment; has a TTL
 *   fulfilled   -- explicit resolution of a pending commitment
 *   superseded  -- replaced by a newer fact via memory_supersede()
 *   archived    -- pending TTL expired, or explicit archive
 *
 * Callers that don't opt in see `active` on every row and the subsystem
 * is invisible.  See docs/proposals/done/memory-lifecycle-states-and-alerts.md. */
#define MEMORY_LIFECYCLE_STATE_ACTIVE     "active"
#define MEMORY_LIFECYCLE_STATE_PENDING    "pending"
#define MEMORY_LIFECYCLE_STATE_FULFILLED  "fulfilled"
#define MEMORY_LIFECYCLE_STATE_SUPERSEDED "superseded"
#define MEMORY_LIFECYCLE_STATE_ARCHIVED   "archived"

/* Commitment-shape detection: returns a best-guess TTL horizon for the
 * content when it looks like a commitment, or 0 if not.  `shape_out`
 * (if non-NULL) gets a short tag: "", "date", "relative", "open-ended".
 *
 * Heuristics (coarse on purpose per the proposal's Trade-offs note):
 *   - future tense + agent-role subject ("I'll", "we'll", "we will")
 *   - explicit date anchor ("by Friday", "by 2026-05-10")
 *   - relative time ("this week", "next month")
 *   - open-ended ("I'll get to it", "TBD", no time anchor) */
#define MEMORY_LIFECYCLE_TTL_DEFAULT_DATE_DAYS       1 /* stored date + 1 day */
#define MEMORY_LIFECYCLE_TTL_DEFAULT_RELATIVE_DAYS   7
#define MEMORY_LIFECYCLE_TTL_DEFAULT_OPEN_ENDED_DAYS 30

int memory_detect_commitment_shape(const char *content, char *shape_out, size_t shape_out_len,
                                   int *ttl_days_out);

/* Transition a memory's lifecycle_state with state-machine enforcement.
 * Valid transitions:
 *   active     -> superseded | archived
 *   pending    -> fulfilled | archived
 *   fulfilled  -> archived
 *   superseded -> archived
 * Invalid transitions return -1. On success the ttl_at column is cleared
 * when leaving `pending`, and archive_reason is stored when transitioning
 * to `archived`. */
int memory_transition_lifecycle(int64_t memory_id, const char *new_state,
                                const char *archive_reason);

/* Mark a memory as pending with an explicit TTL horizon (wall-clock
 * computed as now + ttl_days).  Use when a caller knows the item is a
 * commitment without needing the heuristic.  Returns 0 on success. */
int memory_mark_pending(int64_t memory_id, int ttl_days);

/* Run a pending-TTL sweep: transitions any `pending` row whose ttl_at is
 * past into `archived` with archive_reason='pending_ttl_expired'.
 * Idempotent — re-running touches nothing. Returns count archived. */
int memory_lifecycle_sweep_expired(void);

/* Counts per lifecycle state, written into `out` (indexed by the
 * lifecycle-state constants' order above).  Any NULL counts are skipped. */
typedef struct
{
   int64_t active;
   int64_t pending;
   int64_t fulfilled;
   int64_t superseded;
   int64_t archived;
} memory_lifecycle_counts_t;

int memory_lifecycle_counts(memory_lifecycle_counts_t *out);

/* Assemble the alerts bundle.  Returns a cJSON object with three arrays:
 *   stale_pending: pending rows whose age crossed 80% of ttl_at
 *   unresolved_contradictions: memory_conflicts rows with resolved=0
 *   newly_superseded: memories transitioned to superseded since `since`
 * `since` is an ISO-8601 timestamp; pass NULL/empty for "last 7 days".
 * Caller owns the returned cJSON*. */
struct cJSON *memory_alerts(const char *since);

/* --- Proactive Recall ---
 *
 * Assemble a compact, deterministic "what's relevant right now" bundle
 * that callers can inject into the agent prompt before response
 * generation.  Six ranked sections per the proposal:
 *
 *   1. identity       — long-lived facts about the user (name, role)
 *   2. preferences    — KIND_PREFERENCE rows at L2+
 *   3. active_context — recent L1/L2 facts in the active workspace
 *   4. open_commitments — memories with lifecycle_state='pending'
 *   5. reminders      — matched armed prospective memories
 *   6. directives     — epistemic directives (reserved for when the
 *                        separate directives proposal lands)
 *
 * Each section is budgeted independently so one noisy category cannot
 * crowd out the rest.  All ranking is DB-side — no LLM calls.
 *
 * Default budgets: session-start gets a larger block, per-turn is
 * compact.  The function returns a cJSON object with all six sections
 * and an `explain` array describing why each memory was injected.
 * Caller owns the returned pointer.
 *
 * `task_hint` is the current turn's user text; pass NULL/empty at
 * session start for the larger bundle. */
#define MEMORY_RECALL_DEFAULT_LIMIT_TOKENS_SESSION 1800
#define MEMORY_RECALL_DEFAULT_LIMIT_TOKENS_TURN    600
#define MEMORY_RECALL_MIN_LIMIT_TOKENS             64
#define MEMORY_RECALL_MAX_LIMIT_TOKENS             8192

struct cJSON *memory_recall(const char *task_hint, int limit_tokens, int session_start);

/* Topic-pivot detection between consecutive user turns.  Pure
 * function — no DB access — so callers can invoke it cheaply and
 * decide whether the per-turn recall block should be re-keyed on the
 * new turn's text (pivot) or continue to blend prior-turn context.
 * Returns 1 when the Jaccard similarity of the two turns' stopword-
 * filtered token sets falls below `threshold` (the turns have moved
 * on), 0 otherwise.  A NULL/empty prev_text always returns 0 — the
 * first turn in a session is never a pivot.  threshold <= 0 selects
 * MEMORY_RECALL_PIVOT_DEFAULT_THRESHOLD.  Typical call:
 *
 *   int pivot = memory_recall_topic_pivot(prev, cur, 0);
 *   int session_start = pivot ? 0 : existing_session_start_decision;
 *
 * See docs/proposals/done/personal-agent-phase-2-per-turn-recall.md. */
#define MEMORY_RECALL_PIVOT_DEFAULT_THRESHOLD 0.15
int memory_recall_topic_pivot(const char *prev_text, const char *cur_text, double threshold);

/* Process-local metrics accessor.  Any output pointer may be NULL. */
void memory_recall_metrics(int64_t *assemblies_total, int64_t *session_start_assemblies,
                           double *ms_avg, double *ms_max);

/* --- Scheduled Memory Maintenance Cycles ---
 *
 * Bounded, mostly-deterministic curation pass that runs on a cadence
 * (or on demand via the same entry point).  Four additive modes:
 *
 *   replay    — recompute effectiveness scores and refresh profile cards
 *   compact   — merge near-duplicate memories via memory_improve_dedupe
 *   prune     — sweep expired reminders/directives/lifecycle rows + L0/L1
 *   summarize — (optional, gated) compact low-confidence clusters
 *
 * Modes are OR-able flags so callers can run a subset.  When no memories
 * have been added since the previous cycle AND the cadence hasn't
 * lapsed, the cycle is skipped (summary.skipped=1) and no mutations run.
 *
 * All ranking / mutation is DB-side; the summarize mode is the only
 * path that can touch an LLM and it stays off by default.
 *
 * See docs/proposals/done/scheduled-memory-maintenance-cycles.md. */
#define MEMORY_MAINTENANCE_MODE_REPLAY     (1u << 0)
#define MEMORY_MAINTENANCE_MODE_COMPACT    (1u << 1)
#define MEMORY_MAINTENANCE_MODE_PRUNE      (1u << 2)
#define MEMORY_MAINTENANCE_MODE_SUMMARIZE  (1u << 3)
#define MEMORY_MAINTENANCE_MODE_DEEP_EMBED (1u << 4)

#define MEMORY_MAINTENANCE_MODES_DEFAULT                                                           \
   (MEMORY_MAINTENANCE_MODE_REPLAY | MEMORY_MAINTENANCE_MODE_COMPACT |                             \
    MEMORY_MAINTENANCE_MODE_PRUNE | MEMORY_MAINTENANCE_MODE_DEEP_EMBED)

#define MEMORY_MAINTENANCE_DEFAULT_INTERVAL_SECS 900 /* 15 min */

typedef struct
{
   int modes_run;
   int skipped;
   int dry_run;
   int promoted;
   int demoted;
   int expired;
   int lifecycle_archived;
   int reminders_expired;
   int directives_expired;
   int rescored;
   int profile_cards_refreshed;
   int merged;
   int summarized;
   int deep_embedded; /* deep-tier rows backfilled this cycle */
   double elapsed_ms;
   int64_t memory_count_before;
   int64_t memory_count_after;
   char summary_json[2048];
} memory_maintenance_summary_t;

/* Run a maintenance cycle.  `modes` is a bitmask of
 * MEMORY_MAINTENANCE_MODE_* flags; pass 0 for
 * MEMORY_MAINTENANCE_MODES_DEFAULT.  When `force` is 0 the idle guard is
 * consulted and the cycle may short-circuit (summary.skipped=1).  When
 * `dry_run` is 1 no mutations run and summary.dry_run is set.  `cfg` is
 * optional; pass NULL for hardwired defaults.  Returns 0 on success
 * (even when skipped), -1 on error. */
int memory_maintenance_run(const config_t *cfg, unsigned int modes, int force, int dry_run,
                           memory_maintenance_summary_t *summary);

/* Scheduler entry point: run a cycle when `interval_seconds` has
 * elapsed since the last run (or when no previous run exists).  Cheap
 * no-op when not yet due.  Returns 1 if a cycle ran, 0 if skipped. */
int memory_maintenance_maybe_run(const config_t *cfg, memory_maintenance_summary_t *summary_out);

/* Fetch the last-persisted maintenance summary (for the dashboard
 * card).  Returns 0 if a record exists, -1 otherwise. */
int memory_maintenance_last_summary(memory_maintenance_summary_t *out);

/* Serialise a summary to a fresh cJSON object.  Caller owns it. */
struct cJSON *memory_maintenance_summary_to_json(const memory_maintenance_summary_t *summary);

/* Process-local metrics accessor.  Any output pointer may be NULL. */
void memory_maintenance_metrics(int64_t *runs_total, int64_t *skips_total, int64_t *changes_total,
                                double *ms_avg, double *ms_max);

#endif /* DEC_MEMORY_H */
