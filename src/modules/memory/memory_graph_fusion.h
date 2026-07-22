/* src/headers/memory_graph_fusion.h: Phase 6 graph-vector fusion rerank API. */

#ifndef DEC_MEMORY_GRAPH_FUSION_H
#define DEC_MEMORY_GRAPH_FUSION_H 1

#include "memory.h"
#include "db2/entity_edges.h"

/* Relation gravity table: traversal multiplier per relation label.
 * These are provisional priors — must be labelled as such in diagnostics
 * and stay behind allow_code_graph gate until benchmarked. */
double memory_graph_relation_gravity(const char *relation);

/* Compute the edge score formula from the proposal:
 *   edge_score = relation_gravity(relation)
 *              * structural_factor
 *              * observed_factor
 *              * (1 + clamp(effective_utility, -0.5, 2.0))
 *              * hop_decay
 *
 * structural_factor = code_edge ? (1 + clamp(sw, 0, 3) / 3) : 1.0
 * observed_factor   = 1 + clamp(log1p(weight), 0, 3) / 3
 * hop_decay         = pow(0.5, max(hop - 1, 0))  (1-based hop)
 *
 * Labelled provisional; must be ablated before enabling by default. */
double memory_graph_edge_score(const char *relation, int is_code_edge, int structural_weight,
                               int weight, double effective_utility, int hop);

/* Detect whether a query should open code subgraph traversal.
 * Updates plan->allow_code_graph and plan->code_seed_reason.
 * caller_flag=1 forces code-shaped (rule 1).
 * Returns the detected reason. */
code_seed_reason_t memory_graph_detect_code_shape(const char *query, int caller_flag,
                                                  memory_query_plan_t *plan);

/* Graph expansion result: a candidate memory-id with its graph score. */
typedef struct
{
   int64_t memory_id;
   double graph_score;
   int hops;                     /* minimum hop distance from seed */
   char via[GRAPH_ENDPOINT_MAX]; /* canonical node key used as bridge */
} memory_graph_expansion_t;

/* Expand from a set of canonical node-key seeds (e.g. from code vector hits).
 * Walks entity_edges up to max_hops hops, scoring each reached node with
 * memory_graph_edge_score(), then looks up memories linked to reached nodes.
 * allow_code_graph gates whether code subgraphs are entered.
 * Returns count of expansion results written into out (up to max). */
int memory_graph_expand_from_seeds(const char **node_keys, int seed_count, int max_hops,
                                   int max_neighbors, int allow_code_graph,
                                   int utility_scoring_enabled, memory_graph_expansion_t *out,
                                   int max);

/* Map a code_embeddings point_id back to a canonical node_key by looking up
 * the code_embeddings row.  Returns 0 on success (node_key filled), -1 if
 * not found or DB unavailable. */
int memory_graph_point_id_to_node_key(int64_t point_id, char *node_key, size_t cap);

/* Score-part helper: populate parts->graph_score and parts->code_proximity
 * from graph expansion results for a given memory_id. */
void memory_graph_populate_score_parts(memory_score_parts_t *parts, int64_t memory_id,
                                       const memory_graph_expansion_t *expansions,
                                       int expansion_count);

/* --- Phase 7: path-credit feedback distribution --- */

/* One edge in a retrieval path used to reach a selected/cited result. */
typedef struct
{
   char relation[64];
   int hop; /* 1-based hop distance from seed */
} memory_graph_path_edge_t;

/* Distribute a feedback |delta| across the edges of a retrieval path so the
 * total credit equals |delta|, weighting each edge by relation gravity and
 * hop decay (proposal formula):
 *
 *   edge_credit_i = delta * gravity(ei) * pow(0.5, max(hop_i-1, 0))
 *                   / sum_j( gravity(ej) * pow(0.5, max(hop_j-1, 0)) )
 *
 * Writes per-edge credit into out_credits[0..path_length-1].
 * Guards path_length<=0 and a zero denominator (returns -1 without writing).
 * Returns 0 on success. */
int memory_graph_distribute_path_credit(double delta, const memory_graph_path_edge_t *edges,
                                        int path_length, double *out_credits);

/* --- Recall-path fusion state (thread-local, set per request) ---
 *
 * The kb recall handler sets the requested graph_code_fusion_state ("off" |
 * "shadow" | "on"; NULL/unknown ⇒ off) before invoking the recall path, then
 * clears it. The recall path consults memory_fusion_state_is_on() to decide
 * whether to run the graph-vector fusion expansion, stages the expansion
 * results via memory_fusion_expansions_set(), and memory_compute_score calls
 * memory_fusion_expansions_apply() to populate each candidate's graph_score
 * (which the score blend then weights by graph_weight). State is thread-local
 * so concurrent requests on different worker threads never interfere; when no
 * state is set the apply hook is a no-op and ranking is byte-identical. */
void memory_fusion_state_set(const char *graph_code_fusion_state);
int memory_fusion_state_is_on(void);
void memory_fusion_state_clear(void);
void memory_fusion_expansions_set(const memory_graph_expansion_t *expansions, int count);
void memory_fusion_expansions_apply(memory_score_parts_t *parts, int64_t memory_id);
void memory_fusion_expansions_clear(void);

/* Ablation sub-gates for the fusion expansion (thread-local; default both on so
 * `graph_code_fusion_state=on` runs the full fusion unless an arm overrides):
 *   utility_scoring — utility-weight the entity-edge neighbours.
 *   code_projection — allow the expansion to enter code subgraphs at all
 *                     (combined with the per-query code-shape gate).
 * The ablation runner sets these per arm; memory_fusion_state_clear() resets
 * both to the default (on). */
void memory_fusion_gates_set(int utility_scoring, int code_projection);
int memory_fusion_utility_scoring(void);
int memory_fusion_code_projection(void);

#endif /* DEC_MEMORY_GRAPH_FUSION_H */
