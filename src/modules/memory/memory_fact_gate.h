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
      FACT_GATE_DEFER,            /* no verdict was reached: the semantic-edge write failed (DB
                                     issue), or a registered checker could not answer. The fact
                                     was NOT committed; the caller must retry/defer and must never
                                     treat it as success. The gate returns it only when a checker
                                     is registered; the local seed-table path never does. */
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

   /* Verdict provider: returns 0 and sets *verdict, or non-zero if it could not
    * produce one. Values are fact_gate_verdict_t's pure-gate range (ACCEPT,
    * REJECT_KIND, NOVEL, BADARG). */
   typedef int (*memory_fact_gate_checker_fn)(memory_node_kind_t head_kind, const char *rel_type,
                                              memory_node_kind_t tail_kind, int *verdict);

   /* Route the verdict through `checker` (the memory module over the bus) instead
    * of deciding it in-process. Pass NULL to go back to the local seed table.
    *
    * A registered checker is authoritative: when it fails, the gate reports DEFER
    * rather than falling back, because a silent fallback would let a broken module
    * look healthy. DEFER is never a write — the commit path retries. `*matched` is
    * still filled from the local seed table, which the module cannot return. */
   void memory_fact_gate_register_checker(memory_fact_gate_checker_fn checker);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_FACT_GATE_H */
