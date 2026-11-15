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

/* anti_pattern_t lives in kb_store/anti_patterns.h. The -Ikb_store search path
 * (set in the project Makefile) resolves the unqualified include. */
#include "anti_patterns.h"

typedef struct
{
   int tier_counts[6];          /* L0, L1, L2, L3, L4, L5 */
   int kind_counts[KIND_COUNT]; /* fact, pref, decision, episode, task, scratch, procedure,
                                 * policy, workflow, opinion */
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
   double hybrid_total;   /* lexical+dense hybrid score, for the explain surface */
   double blended_total;  /* final score after the post-hybrid passes */
   double graph_score;    /* utility-weighted graph boost contribution */
   double graph_weight;   /* weight actually applied to graph_score */
   double code_proximity; /* code-projection edge proximity score */
   double utility;        /* decayed utility signal from feedback */
   double outcome;        /* P5 work-outcome overlay; default weight is zero */
   double source_fusion;  /* fused graph-vector-code ranking delta */
   double total;
} memory_score_parts_t;

typedef struct
{
   memory_t memory;
   memory_score_parts_t parts;
} memory_diagnostic_t;

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

/* Kind lifecycle configuration is owned by KB_STORE; include kb_store/kind_lifecycle.h
 * for kb_store_kind_lifecycle_load. */

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

/* --- Functional Tier Helpers --- */

/* memory_authority_t — who is asking for a destructive memory edit. Lives in its
 * own dependency-free header so the kb_service backend contract can name it. */
#include "memory_authority.h"

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

/* Recursively validate declared memory sources before a derived write. The
 * walk fails closed on a rejected/suppressed/missing source, cycle, row cap or
 * depth cap. */
#define MEMORY_DERIVATION_MAX_SOURCES 256

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
   int64_t write_to_readable_samples;
   double write_to_readable_p50_secs;
   double write_to_readable_p95_secs;
   double write_to_readable_p99_secs;
} memory_health_t;

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

/* Session folding: compress a bounded, complete L0 session into an L1
 * checkpoint. Returns the number of source rows folded, 0 when empty, or -1
 * when the source set is over the bound, recursively refused, or cannot be
 * persisted completely. `summary_out` is published only on success. */

/* --- Conversation Scanning --- */

/* --- Window Compaction --- */

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

/* --- Workflow Learning --- */

/* Upsert a project workflow memory (kind=workflow) scoped to a workspace.
 * Key format: workflow:{workspace}:{signal_type}. Content is the rule text.
 * Repeat observations merge into the existing row and bump confidence toward
 * its durable provenance ceiling. Returns the memory id on success, -1 on failure. */

/* Observe a Bash command and, if it carries a learnable workflow signal,
 * upsert a workflow memory tagged to the workspace matching the current
 * working directory. Silent no-op when no signal is detected, no workspace
 * matches, or the DB write fails. */
void workflow_observe_bash(const char *command);
/* Host transport for the shared Go workflow plan and authenticated KB write. */
struct cJSON *workflow_execute(const struct cJSON *input);

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
   /* Items excluded as restatements of something already admitted. Distinct from
    * rejected_for_budget: a suppressed duplicate FREES budget for real evidence,
    * so the two moving in opposite directions is the intended effect and the way
    * to tell whether the suppression is earning its place. */
   int suppressed_near_duplicates;
   int deferred_for_origin_quota;
   int held_for_activation;
} context_budget_metrics_t;

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

/* --- Anti-Patterns ---
 * Storage primitives (insert/list/check/bump/delete/exists_*) live in
 * kb_store/anti_patterns.{h,c} as kb_store_anti_pattern_*. The high-level extraction
 * and escalation passes below are implemented in memory_advanced.c. */

/* Escalate high-hit anti-patterns to hard directive rules. */

/* --- Temporal Facts --- */

/* --- Drift Detection --- */
typedef struct
{
   int drifted;
   int64_t task_id;
   char task_title[256];
   char message[512];
} drift_result_t;

/* --- Style Learning --- */

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

/* The embed command that selects the in-process lexical fixture. TEST BUILDS ONLY —
 * it is compiled out of aimee-kb, so passing it there is an ordinary (failing) exec.
 * There is no implicit embedder: an empty command embeds nothing and returns 0. */
#define MEMORY_EMBED_TEST_FIXTURE "test-lexical-fixture"

/* `input_type` declares which side of the embedder this text is (see
 * embed_input_type_t). It is required rather than defaulted so the compiler forces
 * every call site to state it — a query silently embedded as a document costs
 * retrieval quality and raises no error. */
typedef struct
{
   char state[16]; /* closed | open | half_open */
   int available;
   unsigned failure_streak;
   unsigned recovery_attempt;
   int64_t retry_after_ms;
   int64_t last_success_ms;
   int64_t last_failure_ms;
   uint64_t suppressed_calls;
} memory_embedder_health_t;

/* --- Effectiveness Tracking --- */

typedef struct
{
   int64_t memory_id;
   int times_surfaced;
   int success_present;
   int failure_present;
   double effectiveness;
} memory_effectiveness_t;

/* Get effectiveness stats for display */
typedef struct
{
   double avg_effectiveness;
   int low_effectiveness_count;
   int high_impact_count;
   int never_surfaced_l2;
} effectiveness_stats_t;

/* --- Memory-to-Memory Linking --- */
typedef struct
{
   int64_t id;
   int64_t source_id;
   int64_t target_id;
   char relation[32];
   char created_at[32];
} memory_link_t;

/* --- Content Safety --- */

#define SCAN_BLOCK    0 /* never persist */
#define SCAN_REDACT   1 /* persist with value masked */
#define SCAN_CLASSIFY 2 /* persist but mark sensitive */

/* --- Memory Improve Loop --- */

/* --- Scene Clustering --- */

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

/* JSON envelope helpers used by the aimee-kb RPC seam (kb_service.c, the
 * cmd_memory_core directive handlers, and dashboard code).  to_json returns
 * a heap-allocated cJSON the caller owns; from_json populates `out` from an
 * object of the same shape and returns 0 on success, -1 on malformed input. */

/* Counts per state, written into |out|. */
typedef struct
{
   int64_t open;
   int64_t suppressed;
   int64_t resolved;
   int64_t expired;
} memory_directive_counts_t;

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
#define MEMORY_MAINTENANCE_MODE_REPLAY    (1u << 0)
#define MEMORY_MAINTENANCE_MODE_COMPACT   (1u << 1)
#define MEMORY_MAINTENANCE_MODE_PRUNE     (1u << 2)
#define MEMORY_MAINTENANCE_MODE_SUMMARIZE (1u << 3)
/* auditable-correctness D7: read-only drift report — count code embeddings whose
 * source file was re-scanned after the embedding was written (a staleness
 * heuristic ranking re-ingest order, NOT a correctness verdict). Default-off
 * (not in MODES_DEFAULT); requested explicitly (e.g. `maintenance --mode drift`). */
#define MEMORY_MAINTENANCE_MODE_DRIFT (1u << 4)

#define MEMORY_MAINTENANCE_MODES_DEFAULT                                                           \
   (MEMORY_MAINTENANCE_MODE_REPLAY | MEMORY_MAINTENANCE_MODE_COMPACT |                             \
    MEMORY_MAINTENANCE_MODE_PRUNE)

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
   int drift_candidates; /* D7: code embeddings whose file was re-scanned since embed */
   int drift_requeued;   /* D7: drifted projects enqueued for re-ingest (0 under dry_run) */
   double elapsed_ms;
   int64_t memory_count_before;
   int64_t memory_count_after;
   char summary_json[2048];
} memory_maintenance_summary_t;

#endif /* DEC_MEMORY_H */
