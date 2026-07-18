#ifndef DEC_MEMORY_ONTOLOGY_H
#define DEC_MEMORY_ONTOLOGY_H 1

/* memory_ontology.h: typed node and edge ontology for the aimee memory graph.
 *
 * entity_edges rows are S-R-O triples.  Each row carries:
 *   source      TEXT  — subject entity name
 *   relation    TEXT  — text relation label
 *   target      TEXT  — object entity name
 *   relation_id INT   — integer code matching memory_relation_kind_t (nullable;
 *                        NULL is treated as REL_CO_DISCUSSED)
 *   subject_kind INT  — integer code matching memory_node_kind_t (nullable)
 *   object_kind  INT  — integer code matching memory_node_kind_t (nullable)
 *
 * memory_relation_schema(relation_id, subject_kind, object_kind) holds the
 * allowed (subject_kind, object_kind) pairs per relation.  Writes are
 * validated at insert time; invalid triples are logged and downgraded to
 * REL_CO_DISCUSSED with a warning.
 */

/* ---- Node kinds --------------------------------------------------------- */

typedef enum
{
   NODE_FILE = 0,
   NODE_FUNCTION = 1,
   NODE_STRUCT = 2,
   NODE_MODULE = 3,
   NODE_BUG = 4,
   NODE_COMMIT = 5,
   NODE_PR = 6,
   NODE_DEVELOPER = 7,
   NODE_CONCEPT = 8,
   NODE_EVENT = 9,
   NODE_PERSON = 10,
   NODE_PLACE = 11,
   NODE_TIME_EXPR = 12,
   /* Identity/world-fact kinds (typed-fact layer, §1). Appended (not reordered)
    * so existing persisted integer codes are unaffected. */
   NODE_DEVICE = 13,
   NODE_ORG = 14,
   NODE_IP = 15,
   NODE_SCALAR = 16, /* value-typed object: age=30, port=8740, … */
   NODE_OTHER = 99
} memory_node_kind_t;

/* ---- Relation kinds ----------------------------------------------------- */

typedef enum
{
   REL_DEPENDS_ON = 0,
   REL_IMPLEMENTS = 1,
   REL_FIXES = 2,
   REL_INTRODUCED_BY = 3,
   REL_TESTS = 4,
   REL_CALLS = 5,
   REL_MUTATES = 6,
   REL_PARTICIPATED_IN = 7,
   REL_OCCURRED_AT = 8,
   REL_AUTHORED_BY = 9,
   REL_SUPERSEDES = 10,
   REL_CO_EDITED = 11,    /* collaborative edit relation */
   REL_CO_DISCUSSED = 12, /* collaborative discussion relation */
   REL_SUMMARISES = 13,
   REL_OTHER = 99
} memory_relation_kind_t;

/* Bitmask helpers for memory_graph_walk relation filtering.
 * Use RELATION_MASK(x) to build a mask, then pass to memory_graph_walk(). */
#define RELATION_MASK(r)  (1u << (unsigned)(r))
#define RELATION_MASK_ALL (~0u)

/* ---- Ontology helpers --------------------------------------------------- */

/* Map a relation text label to its integer code.
 * Returns REL_OTHER for unknown labels. */
memory_relation_kind_t memory_ontology_relation_from_text(const char *label);

/* Map an integer code to the canonical relation text label.
 * Returns "other" for unknown codes. */
const char *memory_ontology_relation_to_text(memory_relation_kind_t rel);

/* Map a node-kind text label to its integer code.
 * Returns NODE_OTHER for unknown labels. */
memory_node_kind_t memory_ontology_node_kind_from_text(const char *label);

/* Map an integer code to the canonical node-kind text label.
 * Returns "other" for unknown codes. */
const char *memory_ontology_node_kind_to_text(memory_node_kind_t kind);

/* Validate a (subject_kind, relation_id, object_kind) triple against the
 * static schema.  Returns 1 if valid (or if the schema table is empty /
 * relation is REL_OTHER), 0 if invalid. */
int memory_ontology_validate(memory_node_kind_t subject_kind, memory_relation_kind_t relation,
                             memory_node_kind_t object_kind);

#endif /* DEC_MEMORY_ONTOLOGY_H */
