/* kb_store/entity_edges.h: storage primitives for entity_edges — KB_STORE subsystem.
 *
 * Tables: entity_edges (the graph itself). Profiles live in
 * kb_store/entity_profiles.h.
 *
 * The edge_t typedef lives in headers/memory.h; callers include
 * "aimee.h" first so the type resolves.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_KB_STORE_ENTITY_EDGES_H
#define DEC_KB_STORE_ENTITY_EDGES_H 1

#include <stdint.h>

/* GRAPH_ENDPOINT_MAX is defined in headers/memory.h; provide a local fallback
 * so entity_edges.h can be included independently. The canonical definition
 * is in memory.h and must match this value. */
#ifndef GRAPH_ENDPOINT_MAX
#define GRAPH_ENDPOINT_MAX 512
#endif

#ifdef __cplusplus
extern "C"
{
#endif

   /* (target, weight) pair returned by neighbor-style queries. */
   typedef struct
   {
      char node[GRAPH_ENDPOINT_MAX];
      int weight;
   } kb_store_entity_neighbor_t;

   /* Insert a fresh edge or bump the weight of an existing
    * (source, relation, target) triple. On a fresh insert,
    * relation_id / subject_kind / object_kind are recorded. Sets
    * *out_added = 1 when a new row was inserted. Returns 0 on success,
    * -1 on error. */
   int kb_store_entity_edge_upsert(const char *source, const char *relation, const char *target,
                                   int64_t window_id, int relation_id, int subject_kind,
                                   int object_kind, int *out_added);

   /* List edges where source = entity OR target = entity (symmetric).
    * Fills full edge_t rows. Returns count. */
   int kb_store_entity_edge_list_by_entity(const char *entity, edge_t *out, int max);

   /* Neighbors of `entity` (symmetric). `limit_sql` caps the SQL LIMIT
    * (per-direction). Returns count written into `out`. */
   int kb_store_entity_edge_neighbors(const char *entity, kb_store_entity_neighbor_t *out, int max,
                                      int limit_sql);

   /* Walk-step: top-50 neighbors of `node` (symmetric, ORDER BY weight
    * DESC). Used by memory_episodes BFS. Returns full edge_t rows. */
   int kb_store_entity_edge_walk_step(const char *node, edge_t *out, int max);

   /* Walk-step with ontology kinds: same traversal as kb_store_entity_edge_walk_step,
    * but fills a richer struct carrying relation_id / subject_kind / object_kind
    * for the BFS in memory_episodes. Legacy NULL columns surface as
    * REL_CO_DISCUSSED (12) / NODE_OTHER (99).
    *
    * "kinds" here means the ontology kind COLUMNS, and nothing more. This was
    * called *_typed, which read as "walks typed facts" and is not what it does or
    * ever did — the distinction between the two edge populations is edge_class,
    * not this struct. Like every traversal reader it sees both populations
    * (semantic rows constrained to current); it is named for its output shape. */
   typedef struct
   {
      char source[128];
      char relation[32];
      char target[128];
      int relation_id;
      int subject_kind;
      int object_kind;
      int weight;
   } kb_store_entity_edge_with_kinds_t;

   /* index.c co-citation: targets where source=? AND relation=? AND
    * weight > min_weight, plus symmetric sources where target=? same
    * filter. Returns up to `max` distinct names. */
   int kb_store_entity_edge_co_targets(const char *node, const char *relation, int min_weight,
                                       char (*out)[128], int max);

   /* memory_core_search token search: edges whose source/target/
    * relation matches `token` case-insensitively. Returns full edge_t
    * rows ordered by weight DESC. */
   int kb_store_entity_edge_search_by_token(const char *token, edge_t *out, int max, int limit_sql);

   /* Rich edge row for `graph explain`: full provenance + scoring fields. */
   typedef struct
   {
      int64_t id;
      char source[GRAPH_ENDPOINT_MAX];
      char relation[64];
      char target[GRAPH_ENDPOINT_MAX];
      int weight;
      int structural_weight;
      double utility_score;
      char edge_origin[32];
   } kb_store_entity_edge_explain_t;

   /* List edges incident to |entity| (source OR target) with full provenance,
    * ordered by structural_weight + weight DESC.  Returns count written. */
   int kb_store_entity_edge_explain_by_entity(const char *entity,
                                              kb_store_entity_edge_explain_t *out, int max);

   /* --- Phase 4: utility-aware graph scoring --- */

   /* Extended neighbour result including utility_score for scoring.
    * `relation` and `confidence_class` let the caller score the edge it
    * actually traversed rather than falling back to the generic gravity
    * default. is_semantic=1 marks a typed-fact edge (edge_class='semantic');
    * confidence_class is only meaningful when it is set. */
   typedef struct
   {
      char node[GRAPH_ENDPOINT_MAX];
      int weight;
      double utility_score;
      double effective_utility; /* decayed utility after half-life */
      char relation[64];
      char confidence_class[4]; /* "A"/"B"/"C"; empty for co-occurrence */
      int is_semantic;
   } kb_store_entity_edge_weighted_neighbor_t;

/* Largest frontier a single batched neighbour read will accept. Bounds the
 * generated SQL, so the caller expands a wider frontier in chunks rather than
 * building an unbounded statement. */
#define EE_FRONTIER_BATCH_MAX 64

   /* --- Phase 1: entity edge uniqueness migration --- */

   typedef struct
   {
      int64_t total_rows;    /* total rows in entity_edges */
      int64_t dup_triples;   /* triples with count > 1 */
      int64_t dup_rows;      /* extra rows to remove (count - 1 per group) */
      int64_t largest_group; /* max rows sharing one triple */
      int64_t table_size_kb; /* estimated table + index size in KB */
   } kb_store_entity_edge_dedup_report_t;

   /* Returns 1 if idx_ee_unique_triple exists, 0 if not, -1 on DB error. */
   int kb_store_entity_edge_unique_index_exists(void);

   /* Build idx_ee_unique_triple UNIQUE index (blocking DDL; call outside
    * a long transaction).  Sets *out_already_exists if index was present.
    * Returns 0 on success, -1 on error. */
   int kb_store_entity_edge_build_unique_index(int *out_already_exists);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_ENTITY_EDGES_H */
