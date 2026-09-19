#ifndef AIMEE_DB2_GRAPH_KINDS_H
#define AIMEE_DB2_GRAPH_KINDS_H 1

/* Persisted DB2 graph schema codes shared by native code-index writers and
 * the Go memory owner. Values are an on-disk contract, not memory policy.
 * Historical typedef names remain for native storage consumers. No validation,
 * extraction, ranking or memory-side communication is implemented here.
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

#endif /* AIMEE_DB2_GRAPH_KINDS_H */
