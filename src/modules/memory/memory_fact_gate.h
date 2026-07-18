/* memory_fact_gate.h: the typed-fact write gate (proposal typed-fact §1 / P1).
 *
 * Sibling of memory_gate_check (the prose-memory gate): where that gates free
 * text, this validates a candidate *typed triple* (subject-kind, rel_type,
 * object-kind) against the rel_types ontology BEFORE it is committed as a
 * `semantic` edge in entity_edges. It is the single commit point for semantic
 * edges — every triple emitter routes through it; no emitter writes a semantic
 * edge directly.
 *
 * This header exposes the pure type-validation core (no DB), so it unit-tests in
 * isolation. The DB-backed commit (resolve relation_id, write the edge with
 * edge_class='semantic', stage novel types as provisional) lives in
 * db2/rel_types_store.c and is gated behind config.typed_facts_enabled. */
#ifndef DEC_MEMORY_FACT_GATE_H
#define DEC_MEMORY_FACT_GATE_H 1

#include "memory_ontology.h"
#include "rel_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      FACT_GATE_ACCEPT = 0,       /* known rel_type, kinds satisfy head/tail constraints */
      FACT_GATE_REJECT_KIND,      /* known rel_type, but subject/object kind not allowed */
      FACT_GATE_NOVEL,            /* rel_type not in the (seed) ontology — caller stages/defers */
      FACT_GATE_BADARG,           /* missing/empty rel_type */
      FACT_GATE_DEFER,            /* validated but the semantic-edge write failed (DB issue): the
                                     fact was NOT committed; the caller must retry/defer and must
                                     never treat it as success (commit path only — the pure
                                     memory_fact_gate_check never returns it) */
      FACT_GATE_REJECT_SENSITIVE, /* validated but WITHHELD from the shared KB: a
                                     credential/regulated-PII relation. Personal/sensitive
                                     facts stay in the user's local DB1, never DB2. Not
                                     committed; caller treats it as not-stored, not an error.
                                     (commit path only — the pure gate never returns it) */
   } fact_gate_verdict_t;

   /* Validate (head_kind, rel_type, tail_kind) against the seed ontology. On a
    * known rel_type, `*matched` (when non-NULL) is set to its definition so the
    * caller can read correction_behavior / sensitivity / inverse without a second
    * lookup. `*matched` is left NULL for NOVEL/BADARG. Pure: seed only, no DB. */
   fact_gate_verdict_t memory_fact_gate_check(memory_node_kind_t head_kind, const char *rel_type,
                                              memory_node_kind_t tail_kind,
                                              const rel_type_def_t **matched);

   const char *fact_gate_verdict_to_text(fact_gate_verdict_t v);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_FACT_GATE_H */
