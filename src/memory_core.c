/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* memory_core.c: high-level memory orchestration, context assembly, scoring,
 * and DB2 (incl. pgvector) retrieval routing. */
#include "aimee.h"
#include "memory_context_internal.h"

#if defined(AIMEE_DB2_DISABLED)

#include <math.h>

static const char *memory_storage_unavailable_msg(void)
{
   return "memory storage unavailable in this process";
}

static char *memory_dup_cstr(const char *s)
{
   const char *src = s ? s : "";
   size_t len = strlen(src) + 1;
   char *out = (char *)malloc(len);
   if (out)
      memcpy(out, src, len);
   return out;
}

static void memory_answer_unavailable(memory_answer_result_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->no_answer = 1;
   out->evidence.decision = MEMORY_ANSWER_DECISION_ABSTAIN;
   out->evidence.reason = MEMORY_ANSWER_REASON_DB_UNAVAILABLE;
   out->evidence.structural = 1;
   out->evidence.anchor_rank = -1;
   snprintf(out->error, sizeof(out->error), "%s", memory_storage_unavailable_msg());
}

int memory_gate_check(const char *tier, const char *kind, const char *key, const char *content,
                      double confidence, gate_verdict_t *verdict)
{
   (void)tier;
   (void)kind;
   (void)key;
   (void)content;
   (void)confidence;
   if (!verdict)
      return -1;
   memset(verdict, 0, sizeof(*verdict));
   verdict->result = GATE_REJECT;
   snprintf(verdict->reason, sizeof(verdict->reason), "%s", memory_storage_unavailable_msg());
   return 0;
}

int memory_tier_priority(const char *tier)
{
   if (!tier)
      return 0;
   if (strcmp(tier, TIER_L5) == 0)
      return 5;
   if (strcmp(tier, TIER_L4) == 0)
      return 4;
   if (strcmp(tier, TIER_L3) == 0)
      return 3;
   if (strcmp(tier, TIER_L2) == 0)
      return 2;
   if (strcmp(tier, TIER_L1) == 0)
      return 1;
   return 0;
}

const char *memory_functional_tier_name(const char *tier)
{
   if (!tier)
      return "Unknown";
   if (strcmp(tier, TIER_L0) == 0 || strcmp(tier, TIER_L1) == 0)
      return TIER_L0_NAME;
   if (strcmp(tier, TIER_L2) == 0)
      return TIER_L2_NAME;
   if (strcmp(tier, TIER_L3) == 0)
      return TIER_L3_NAME;
   if (strcmp(tier, TIER_L4) == 0)
      return TIER_L4_NAME;
   if (strcmp(tier, TIER_L5) == 0)
      return TIER_L5_NAME;
   return "Unknown";
}

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

const char *memory_answer_evidence_decision_str(const memory_answer_evidence_t *trace)
{
   (void)trace;
   return "abstain";
}

const char *memory_answer_evidence_reason_str(const memory_answer_evidence_t *trace)
{
   (void)trace;
   return "db_unavailable";
}

double memory_fetch_budget_factor(memory_query_shape_t shape, int ntokens)
{
   double token_factor;
   double shape_factor = 1.0;
   if (ntokens <= 2)
      token_factor = 1.5;
   else if (ntokens <= 4)
      token_factor = 1.0;
   else if (ntokens <= 7)
      token_factor = 0.75;
   else
      token_factor = 0.5;

   if (shape == MEM_SHAPE_LIST || shape == MEM_SHAPE_QUANTITATIVE ||
       shape == MEM_SHAPE_TEMPORAL_INTERVAL)
      shape_factor = 1.5;
   else if (shape == MEM_SHAPE_YES_NO)
      shape_factor = 0.75;
   return token_factor * shape_factor;
}

int memory_query_plan(const char *query, int limit, int hard_cap, memory_query_plan_t *out)
{
   (void)query;
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->route = MEM_ROUTE_LEXICAL;
   out->shape = MEM_SHAPE_UNKNOWN;
   out->weights.lexical_weight = 1.0;
   out->fetch_multiplier = 1;
   out->min_fetch = limit > 0 ? limit : 1;
   out->max_fetch = hard_cap > 0 ? hard_cap : out->min_fetch;
   return 0;
}

void memory_query_rewrite(const char *query, const config_t *cfg, memory_query_rewrite_t *out)
{
   (void)query;
   (void)cfg;
   if (out)
      memset(out, 0, sizeof(*out));
}

int memory_insert(const char *tier, const char *kind, const char *key, const char *content,
                  double confidence, const char *session_id, memory_t *out)
{
   return memory_insert_ex(tier, kind, key, content, "", confidence, session_id, out);
}

int memory_insert_ex(const char *tier, const char *kind, const char *key, const char *content,
                     const char *use_cases, double confidence, const char *session_id,
                     memory_t *out)
{
   (void)tier;
   (void)kind;
   (void)key;
   (void)content;
   (void)use_cases;
   (void)confidence;
   (void)session_id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_get(int64_t id, memory_t *out)
{
   (void)id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_touch(int64_t id)
{
   (void)id;
   return -1;
}

int memory_update_content(int64_t id, const char *content)
{
   (void)id;
   (void)content;
   return -1;
}

int memory_reject(int64_t id, const char *reason)
{
   (void)id;
   (void)reason;
   return -1;
}

int memory_list(const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   (void)tier;
   (void)kind;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_delete(int64_t id)
{
   (void)id;
   return -1;
}

int memory_stats(memory_stats_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return 0;
}

int memory_rebuild_derived_indexes(int limit)
{
   (void)limit;
   return 0;
}

int memory_repair_vector_index(int64_t memory_id, const char *command)
{
   (void)memory_id;
   (void)command;
   return -1;
}

int memory_repair_vector_index_failed_only(const char *command, int limit, int *failed_out)
{
   (void)command;
   (void)limit;
   if (failed_out)
      *failed_out = 0;
   return -1;
}

int memory_rebuild_vector_index_for_version(const char *version, int *failed_out)
{
   (void)version;
   if (failed_out)
      *failed_out = 0;
   return -1;
}

int memory_diagnose(const char *query, int limit, memory_diagnostic_t *out, int max)
{
   (void)query;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_diagnose_scoped(const char *query, const char *scope_type, const char *scope_value,
                           int limit, memory_diagnostic_t *out, int max)
{
   (void)query;
   (void)scope_type;
   (void)scope_value;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_explain_match(const char *query, int64_t memory_id, memory_diagnostic_t *out)
{
   (void)query;
   (void)memory_id;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_ask_query(const char *query, int limit, memory_answer_result_t *out)
{
   (void)query;
   (void)limit;
   memory_answer_unavailable(out);
   return -1;
}

int memory_ask_query_scoped(const char *query, const char *scope_type, const char *scope_value,
                            int limit, memory_answer_result_t *out)
{
   (void)query;
   (void)scope_type;
   (void)scope_value;
   (void)limit;
   memory_answer_unavailable(out);
   return -1;
}

char *memory_answer_query(const char *query, int limit)
{
   (void)query;
   (void)limit;
   return memory_dup_cstr("");
}

char *memory_answer_query_scoped(const char *query, const char *scope_type, const char *scope_value,
                                 int limit)
{
   (void)query;
   (void)scope_type;
   (void)scope_value;
   (void)limit;
   return memory_dup_cstr("");
}

int memory_search(char **clusters, int cluster_count, int limit, search_result_t *out, int max)
{
   (void)clusters;
   (void)cluster_count;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_find_facts(const char *query, int limit, memory_t *out, int max)
{
   (void)query;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_find_facts_scoped(const char *query, const char *scope_type, const char *scope_value,
                             int limit, memory_t *out, int max)
{
   (void)query;
   (void)scope_type;
   (void)scope_value;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_find_facts_visible(const char *query, const char *workspace, const char *project,
                              int limit, memory_t *out, int max)
{
   (void)query;
   (void)workspace;
   (void)project;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_expand_to_session_window(memory_t *out, int count, int max, int window_radius)
{
   (void)out;
   (void)max;
   (void)window_radius;
   return count > 0 ? count : 0;
}

int memory_list_episodes(const char *query, int limit, memory_episode_t *out, int max)
{
   (void)query;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_get_episode(const char *episode_key, memory_episode_t *out)
{
   (void)episode_key;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_search_graph(const char *query, int limit, memory_relation_t *out, int max)
{
   (void)query;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_search_graph_as_of(const char *query, const char *as_of, int limit,
                              memory_relation_t *out, int max)
{
   (void)query;
   (void)as_of;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

int memory_get_entity_profile(const char *entity, memory_entity_profile_t *out)
{
   (void)entity;
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_get_entity_edges(const char *entity, int limit, memory_relation_t *out, int max)
{
   (void)entity;
   (void)limit;
   (void)out;
   (void)max;
   return 0;
}

char *memory_get_context_block(const char *query, const char *block_type, int limit)
{
   (void)query;
   (void)block_type;
   (void)limit;
   return memory_dup_cstr("");
}

int64_t memory_lineage_insert(const char *object_type, int64_t object_id, const char *source_kind,
                              const char *source_ref, double confidence)
{
   (void)object_type;
   (void)object_id;
   (void)source_kind;
   (void)source_ref;
   (void)confidence;
   return -1;
}

int memory_lineage_get(const char *object_type, int64_t object_id, memory_lineage_t *out, int max)
{
   (void)object_type;
   (void)object_id;
   (void)out;
   (void)max;
   return 0;
}

void memory_cite(int64_t memory_id, int json_out)
{
   (void)memory_id;
   (void)json_out;
}

void add_provenance(int64_t memory_id, const char *session_id, const char *action,
                    const char *details)
{
   (void)memory_id;
   (void)session_id;
   (void)action;
   (void)details;
}

int memory_reclassify_directives_ex(int require_approval)
{
   (void)require_approval;
   return 0;
}

int memory_reclassify_directives(void)
{
   return memory_reclassify_directives_ex(0);
}

int memory_synthesize_l5_patterns(void)
{
   return 0;
}

int memory_promote_stable_l2_to_l3(void)
{
   return 0;
}

int memory_approve_l4_promotion(int64_t memory_id, const char *approver, const char *note)
{
   (void)memory_id;
   (void)approver;
   (void)note;
   return -1;
}

int memory_collect_scopes(int64_t memory_id, memory_scope_tag_t *out, int max)
{
   (void)memory_id;
   (void)out;
   (void)max;
   return 0;
}

memory_scope_level_t memory_primary_scope(int64_t memory_id, char *value, size_t value_len)
{
   (void)memory_id;
   if (value && value_len > 0)
      value[0] = '\0';
   return MEMORY_SCOPE_NONE;
}

int memory_scope_visibility_rank(int64_t memory_id, const char *workspace, const char *project)
{
   (void)memory_id;
   (void)workspace;
   (void)project;
   return 0;
}

int memory_tag_global(int64_t memory_id)
{
   (void)memory_id;
   return -1;
}

int memory_tag_project(int64_t memory_id, const char *project)
{
   (void)memory_id;
   (void)project;
   return -1;
}

int memory_tag_workspace(int64_t memory_id, const char *workspace)
{
   (void)memory_id;
   (void)workspace;
   return -1;
}

int memory_tag_scope(int64_t memory_id, const char *scope_type, const char *scope_value)
{
   (void)memory_id;
   (void)scope_type;
   (void)scope_value;
   return -1;
}

int memory_auto_tag_workspace(int64_t memory_id, const char *key, const char *content)
{
   (void)memory_id;
   (void)key;
   (void)content;
   return -1;
}

int64_t memory_upsert_workflow(const char *workspace, const char *signal_type, const char *rule,
                               double observed_confidence, const char *session_id)
{
   (void)workspace;
   (void)signal_type;
   (void)rule;
   (void)observed_confidence;
   (void)session_id;
   return -1;
}

int memory_embed(int64_t memory_id, const char *command)
{
   (void)memory_id;
   (void)command;
   return -1;
}

int memory_embed_text(const char *text, const char *command, float *out, int max_dim)
{
   (void)text;
   (void)command;
   if (!out || max_dim <= 0)
      return -1;
   memset(out, 0, (size_t)max_dim * sizeof(*out));
   return 0;
}

double cosine_similarity(const float *a, const float *b, int dim)
{
   double dot = 0.0;
   double na = 0.0;
   double nb = 0.0;
   if (!a || !b || dim <= 0)
      return 0.0;
   for (int i = 0; i < dim; i++)
   {
      dot += (double)a[i] * (double)b[i];
      na += (double)a[i] * (double)a[i];
      nb += (double)b[i] * (double)b[i];
   }
   double denom = sqrt(na) * sqrt(nb);
   return denom > 1e-9 ? dot / denom : 0.0;
}

void memory_coref_stats(memory_coref_stats_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
}

void memory_coref_stats_reset(void)
{
}

#else

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
#include "cJSON.h"
#include "dogfood.h"
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* Regex patterns, gate functions, and content scanning are platform-owned. */

#include <pthread.h>

static int memory_split_tokens(const char *norm, char tokens[][64], int max_tokens);
static int memory_token_in_norm(const char *norm, const char *token);
static int memory_expand_query_terms(const char *norm_query, char base_tokens[][64], int base_count,
                                     char expanded[][64], int max_terms);
static int memory_is_month_token(const char *tok);
static int memory_is_weekday_token(const char *tok);
static int memory_is_probable_location_token(const char *tok);
static int memory_is_relation_token(const char *tok);
static int memory_is_likely_action_token(const char *tok);
static int memory_is_coref_pronoun_token(const char *tok);
static int memory_temporal_value_conflict(const char *a, const char *b);
static double memory_env_weight(const char *name, double fallback);
static int memory_env_int(const char *name, int fallback, int min_value, int max_value);
static void memory_refresh_aliases(int64_t memory_id, const char *key, const char *content);
static void memory_refresh_summaries(int64_t memory_id, const char *key, const char *content);
static void memory_refresh_coref_entities(int64_t memory_id, const char *content);
static void memory_refresh_event_frames(int64_t memory_id, const char *key, const char *content);
static void memory_refresh_chunks(int64_t memory_id, const char *content);
static void memory_refresh_units_graph(int64_t memory_id, const char *key, const char *content);
static void memory_refresh_unit_embeddings(int64_t memory_id);
static int memory_fetch_row_by_id(int64_t memory_id, memory_t *out);
static int memory_fetch_row_by_unit_id(int64_t unit_id, memory_t *out);
static void memory_refresh_episode_relations(int64_t memory_id, const char *key,
                                             const char *content);

typedef enum
{
   MEM_QUERY_GENERAL = 0,
   MEM_QUERY_TEMPORAL,
   MEM_QUERY_ENTITY,
   MEM_QUERY_PROCEDURAL
} memory_query_intent_t;

static memory_query_intent_t memory_query_intent(const char *raw_query, const char *norm_query);
static memory_query_shape_t memory_query_shape(const char *raw_query, const char *norm_query);
static memory_query_route_t memory_query_route(const char *raw_query, const char *norm_query,
                                               memory_query_shape_t shape);
static int memory_query_has_term(const char *norm_query, const char *term);
static int memory_query_is_code_like(const char *query);

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

static pthread_mutex_t s_memory_pagerank_stats_mu = PTHREAD_MUTEX_INITIALIZER;
static memory_pagerank_runtime_stats_t s_memory_pagerank_stats;

/* Keep the monolith split by responsibility without changing linkage.
 * The included fragments share this translation unit private state. */
#include "memory_core_helpers.inc"
#include "memory_core_crud.inc"
#include "memory_core_search.inc"
#include "memory_core_scope_embed.inc"
#include "memory_core_tiers.inc"

#endif /* AIMEE_DB2_DISABLED */
