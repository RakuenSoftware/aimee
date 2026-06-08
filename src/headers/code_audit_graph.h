/* code_audit_graph.h: pure graph-derived code-health audit algorithms.
 *
 * These operate on already-fetched edge/key arrays (no DB, no I/O) so they are
 * unit-testable in isolation; the kb-side db2 layer (db2/code_audit.c) fetches
 * the rows from entity_edges / code_embeddings and calls these. Two checks live
 * here — dead exports and import cycles; clone grouping is a trivial sort-group
 * done in the assemble layer.
 *
 * Edge-key formats (from db2/code_projection.c):
 *   exports edge target: "export:<proj>:<name>"
 *   imports edge target: "import:<proj>:<name>"
 * An export is consumed iff some import shares the same "<proj>:<name>" tail.
 */
#ifndef DEC_CODE_AUDIT_GRAPH_H
#define DEC_CODE_AUDIT_GRAPH_H 1

/* A directed file-dependency edge (from imports a symbol that to exports). */
typedef struct
{
   const char *from; /* importing file key */
   const char *to;   /* exporting file key */
} audit_edge_t;

/* Select dead exports: export keys ("export:<X>") with no matching import key
 * ("import:<X>"). Writes borrowed pointers from `exports` into `out`. Returns
 * the count written (<= max). Pure. */
int code_audit_dead_exports(const char *const *exports, int n_exports, const char *const *imports,
                            int n_imports, const char **out, int max);

/* Find import cycles among directed file-dependency edges. Each cycle is
 * rendered as a malloc'd "a -> b -> … -> a" string into `out` (caller frees
 * each). At most one cycle is reported per distinct cycle-entry node, capped at
 * `max`. Returns the count written. Pure (allocates only the result strings). */
int code_audit_find_cycles(const audit_edge_t *edges, int n_edges, char **out, int max);

#endif /* DEC_CODE_AUDIT_GRAPH_H */
