/* db2/entity_edges.h: storage primitives for entity_edges — DB2 subsystem.
 *
 * Tables: entity_edges (the graph itself). Profiles live in
 * db2/entity_profiles.h.
 *
 * The edge_t typedef lives in headers/memory.h; callers include
 * "aimee.h" first so the type resolves.
 *
 * Pure domain API. No backend types or handles in any signature. */
#ifndef DEC_DB2_ENTITY_EDGES_H
#define DEC_DB2_ENTITY_EDGES_H 1

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
   } db2_entity_neighbor_t;

   /* Insert a fresh edge or bump the weight of an existing
    * (source, relation, target) triple. On a fresh insert,
    * relation_id / subject_kind / object_kind are recorded. Sets
    * *out_added = 1 when a new row was inserted. Returns 0 on success,
    * -1 on error. */
   int db2_entity_edge_upsert(const char *source, const char *relation, const char *target,
                              int64_t window_id, int relation_id, int subject_kind, int object_kind,
                              int *out_added);

   /* Typed-fact (§1 / P1) semantic-edge writer: like db2_entity_edge_upsert but
    * stamps edge_class='semantic' so the row is separable from co-occurrence rows
    * sharing this table (R1-A1). The single commit point for semantic edges is
    * the typed-fact gate (db2_fact_commit) — callers route through it, not here
    * directly. Bumps weight on a repeat (source,relation,target) and upgrades the
    * stored confidence_class/confidence when the new write outranks it (§5; never
    * downgrades). confidence_class is FACT_CLASS_* (NULL/empty -> "C"). 0/-1. */
   int db2_entity_edge_upsert_semantic(const char *source, const char *relation, const char *target,
                                       int relation_id, int subject_kind, int object_kind,
                                       const char *confidence_class, double confidence,
                                       int *out_added);

   /* Recall over typed facts: edges where (source=entity OR target=entity) AND
    * edge_class='semantic' (co-occurrence rows excluded). Returns count. */
   int db2_entity_edges_semantic_by_entity(const char *entity, edge_t *out, int max);

   /* List edges where source = entity OR target = entity (symmetric).
    * Fills full edge_t rows. Returns count. */
   int db2_entity_edge_list_by_entity(const char *entity, edge_t *out, int max);

   /* Neighbors of `entity` (symmetric). `limit_sql` caps the SQL LIMIT
    * (per-direction). Returns count written into `out`. */
   int db2_entity_edge_neighbors(const char *entity, db2_entity_neighbor_t *out, int max,
                                 int limit_sql);

   /* Neighbors of `entity` filtered to relation == `rel_a`, optionally
    * also matching `rel_b` (NULL = single-relation filter). When
    * `order_by_weight` is non-zero, the SQL adds ORDER BY weight DESC
    * before LIMIT. */
   int db2_entity_edge_neighbors_filtered(const char *entity, const char *rel_a, const char *rel_b,
                                          int order_by_weight, db2_entity_neighbor_t *out, int max,
                                          int limit_sql);

   /* Walk-step: top-50 neighbors of `node` (symmetric, ORDER BY weight
    * DESC). Used by memory_episodes BFS. Returns full edge_t rows. */
   int db2_entity_edge_walk_step(const char *node, edge_t *out, int max);

   /* Typed walk-step: same shape as db2_entity_edge_walk_step but fills
    * a richer struct with relation_id / subject_kind / object_kind for
    * the typed BFS in memory_episodes. Legacy NULL columns surface as
    * REL_CO_DISCUSSED (12) / NODE_OTHER (99). */
   typedef struct
   {
      char source[128];
      char relation[32];
      char target[128];
      int relation_id;
      int subject_kind;
      int object_kind;
      int weight;
   } db2_entity_edge_typed_t;

   int db2_entity_edge_walk_step_typed(const char *node, db2_entity_edge_typed_t *out, int max);

   /* memory_scan recurring topics: top targets where source=? AND
    * relation=?, GROUP BY target ORDER BY SUM(weight) DESC. */
   int db2_entity_edge_top_targets_by_relation(const char *source, const char *relation,
                                               db2_entity_neighbor_t *out, int max);

   /* memory_scan top partners: both directions UNION ALL, grouped by
    * partner, summed by weight, ordered DESC. */
   int db2_entity_edge_top_partners_by_relation(const char *entity, const char *relation,
                                                db2_entity_neighbor_t *out, int max);

   /* memory_assemble: top distinct triples by weight. */
   int db2_entity_edge_top_distinct_triples(edge_t *out, int max);

   /* index.c co-citation: targets where source=? AND relation=? AND
    * weight > min_weight, plus symmetric sources where target=? same
    * filter. Returns up to `max` distinct names. */
   int db2_entity_edge_co_targets(const char *node, const char *relation, int min_weight,
                                  char (*out)[128], int max);

   /* memory_improve.c: bump utility_score by delta on every edge that
    * touches the given key (source = key OR target = key). */
   int db2_entity_edge_bump_utility(const char *key, double delta);

   /* memory_core_search outbound only: targets where source=?, ordered
    * by weight DESC up to limit_sql rows. */
   int db2_entity_edge_outbound_neighbors(const char *source, db2_entity_neighbor_t *out, int max,
                                          int limit_sql);

   /* memory_core_search token search: edges whose source/target/
    * relation matches `token` case-insensitively. Returns full edge_t
    * rows ordered by weight DESC. */
   int db2_entity_edge_search_by_token(const char *token, edge_t *out, int max, int limit_sql);

   /* Maintenance: drop edges where neither endpoint appears in any
    * L1/L2 memory key/content. Returns rows deleted. */
   int db2_entity_edge_prune_orphans(void);

   /* Maintenance: normalize weights per relation so the maximum is 100.
    * Returns rows updated. */
   int db2_entity_edge_normalize_weights(void);

   /* (relation_id, subject_kind, object_kind) triple from
    * memory_relation_schema. Subject/object kinds use the integer codes
    * from memory_ontology_node_kind_t with 99 as the wildcard "any". */
   typedef struct
   {
      int relation_id;
      int subject_kind;
      int object_kind;
   } db2_relation_schema_row_t;

   /* List the memory_relation_schema rows ordered by
    * (relation_id, subject_kind, object_kind). Returns rows written
    * into |out|. */
   int db2_relation_schema_list(db2_relation_schema_row_t *out, int max);

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
   } db2_entity_edge_explain_t;

   /* List edges incident to |entity| (source OR target) with full provenance,
    * ordered by structural_weight + weight DESC.  Returns count written. */
   int db2_entity_edge_explain_by_entity(const char *entity, db2_entity_edge_explain_t *out,
                                         int max);

   /* --- Phase 4: utility-aware graph scoring --- */

   /* Extended neighbour result including utility_score for scoring. */
   typedef struct
   {
      char node[GRAPH_ENDPOINT_MAX];
      int weight;
      double utility_score;
      double effective_utility; /* decayed utility after half-life */
   } db2_entity_edge_weighted_neighbor_t;

   /* Utility half-life decay.
    * Returns decayed utility given a stored score, a touch timestamp
    * (ISO-8601 UTC text), and a half-life in days (default 90).
    * Empty or invalid timestamp returns 0.0 when score is 0, or the
    * raw score when non-zero (legacy sentinel). */
   double db2_entity_edge_utility_decay(double raw_score, const char *touched_at,
                                        int half_life_days);

   /* Compute prune_priority = weight + (utility_weight * decayed_utility).
    * Used to rank neighbours when the neighbour budget is exceeded.
    * utility_weight=0 disables utility contribution (Phase 4 default). */
   double db2_entity_edge_prune_priority(int weight, double decayed_utility, double utility_weight);

   /* Weighted neighbours: like db2_entity_edge_neighbors but also returns
    * utility_score and effective_utility (decayed with half_life_days=90).
    * When utility_scoring_enabled=0, effective_utility is set to 0.
    * Returns count written into out. */
   int db2_entity_edge_neighbors_weighted(const char *entity,
                                          db2_entity_edge_weighted_neighbor_t *out, int max,
                                          int limit_sql, int utility_scoring_enabled);

   /* Set-based two-hop expansion CTE.  Given a seed entity, return all
    * neighbours reachable within exactly two hops in one SQL round trip.
    * hop=1 rows get factor 1.0; hop=2 rows get factor 0.5.
    * Returns count of (node, weight, hop) triples written into out. */
   typedef struct
   {
      char node[GRAPH_ENDPOINT_MAX];
      int weight;
      int hop; /* 1 or 2 */
   } db2_entity_edge_hop_t;

   int db2_entity_edge_two_hop_neighbors(const char *entity, int max, int limit_per_hop,
                                         db2_entity_edge_hop_t *out);

   /* Backfill utility_touched_at for rows with non-zero utility_score and
    * empty utility_touched_at.  Returns rows updated.  Safe to call
    * repeatedly (idempotent). */
   int db2_entity_edge_backfill_utility_touched_at(void);

   /* --- Phase 1: entity edge uniqueness migration --- */

   typedef struct
   {
      int64_t total_rows;    /* total rows in entity_edges */
      int64_t dup_triples;   /* triples with count > 1 */
      int64_t dup_rows;      /* extra rows to remove (count - 1 per group) */
      int64_t largest_group; /* max rows sharing one triple */
      int64_t table_size_kb; /* estimated table + index size in KB */
   } db2_entity_edge_dedup_report_t;

   /* Dry-run audit: count duplicate triples and estimate cost.
    * Returns 0 on success, -1 on DB error. */
   int db2_entity_edge_dedup_audit(db2_entity_edge_dedup_report_t *out);

   /* Migrate: write rollback JSONL to rollback_path (if non-NULL), then
    * merge duplicates — sum weights, clamp utility to [-5, 5], keep
    * newest utility_touched_at.  dry_run=1 skips writes and fills *out.
    * Returns 0 on success, -1 on error. */
   int db2_entity_edge_dedup_migrate(const char *rollback_path, int dry_run,
                                     db2_entity_edge_dedup_report_t *out);

   /* Returns 1 if idx_ee_unique_triple exists, 0 if not, -1 on DB error. */
   int db2_entity_edge_unique_index_exists(void);

   /* Build idx_ee_unique_triple UNIQUE index (blocking DDL; call outside
    * a long transaction).  Sets *out_already_exists if index was present.
    * Returns 0 on success, -1 on error. */
   int db2_entity_edge_build_unique_index(int *out_already_exists);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ENTITY_EDGES_H */
