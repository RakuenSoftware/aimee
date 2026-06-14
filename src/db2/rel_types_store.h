/* rel_types_store.h: DB2-backed persistence + commit path for the typed-fact
 * ontology (typed-fact §1 / P1b). The live `rel_types` table overlay of the
 * in-code SEED_ONTOLOGY, plus db2_fact_commit() — the single commit point for
 * `semantic` edges. Pure ontology logic stays in rel_types.c / memory_fact_gate.c;
 * this file is the part that needs a DB2 connection. */
#ifndef DEC_DB2_REL_TYPES_STORE_H
#define DEC_DB2_REL_TYPES_STORE_H 1

#include "../headers/memory_ontology.h"
#include "../headers/memory_fact_gate.h"
#include "fact_lifecycle.h" /* fact_authority_t, FACT_CLASS_* (§4/§5) */

#ifdef __cplusplus
extern "C"
{
#endif

   /* Upsert the in-code seed ontology into the `rel_types` table (idempotent —
    * ON CONFLICT DO NOTHING). Call once at DB2 init / before first commit. 0/-1. */
   int db2_rel_types_ensure_seed(void);

   /* Resolve a rel_type name (normalized) to its `rel_types` row id. Returns 1 and
    * sets *out_id when present, 0 when absent, -1 on error. */
   int db2_rel_types_resolve(const char *rel_type, long *out_id);

   /* Stage a novel rel_type as a provisional row (status='provisional') so a
    * Class-C semantic edge has a resolvable relation_id (§1 "provisional rel_type",
    * promotion is §2). Idempotent. Returns the row id (>0) or -1. */
   long db2_rel_types_stage_provisional(const char *rel_type);

   /* The SINGLE commit point for typed `semantic` edges (R1-B3). Validates the
    * candidate triple against the ontology (seed) and, when `enabled`, writes a
    * semantic edge:
    *   ACCEPT      -> write edge_class='semantic', relation_id = the resolved id;
    *   NOVEL       -> stage provisional rel_type, write a Class-C semantic edge;
    *   REJECT_KIND / BADARG -> no write.
    * Returns the gate verdict regardless of `enabled` (so callers can observe what
    * *would* happen with the flag off). `enabled` is config.typed_facts_enabled.
    * `authority` keys the §5 confidence class: a user assertion writes Class A, a
    * model ACCEPT writes Class B, model NOVEL writes Class C. */
   fact_gate_verdict_t db2_fact_commit(const char *source, memory_node_kind_t head_kind,
                                       const char *rel_type, const char *target,
                                       memory_node_kind_t tail_kind, fact_authority_t authority,
                                       int enabled);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_REL_TYPES_STORE_H */
