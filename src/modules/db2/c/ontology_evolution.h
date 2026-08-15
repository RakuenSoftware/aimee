/* ontology_evolution.h: self-extending, metadata-driven ontology promotion
 * pipeline (typed-fact §2 / P4). Novel rel_types the gate sees accumulate an
 * occurrence_count in `ontology_evaluations`; once a type crosses a threshold it
 * becomes a candidate the model evaluates, and the chosen decision is applied
 * here: approve (promote the provisional rel_type to active), map (record an
 * alias onto a canonical type), or reject.
 *
 * This file is the deterministic data layer: observation tracking, candidate
 * selection, and the three decision applications. The model call that *chooses*
 * among them (a maintenance-cycle mode) and the `aimee expand` CLI/url fetch are
 * wiring, layered on top of these primitives. */
#ifndef DEC_DB2_ONTOLOGY_EVOLUTION_H
#define DEC_DB2_ONTOLOGY_EVOLUTION_H 1

#include <stddef.h>
#include "../headers/rel_types.h" /* REL_TYPE_NAME_MAX */

#ifdef __cplusplus
extern "C"
{
#endif

   /* ontology_evaluations.status lifecycle. */
#define ONTO_EVAL_PENDING  "pending"
#define ONTO_EVAL_APPROVED "approved"
#define ONTO_EVAL_MAPPED   "mapped"
#define ONTO_EVAL_REJECTED "rejected"

   /* Record one observation of a novel rel_type: insert the evaluation row (or
    * bump its occurrence_count). Idempotent on rel_type. The rel_type is
    * normalized. Returns the new occurrence_count (>0) or -1. Called from the
    * commit path whenever the gate returns NOVEL. */
   long db2_ontology_eval_observe(const char *rel_type);

   /* The current occurrence_count for a rel_type (>=0), or -1 on error / when no
    * evaluation row exists. */
   long db2_ontology_eval_count(const char *rel_type);

   /* The evaluation status for a rel_type. Writes the status string into out.
    * Returns 1 if a row exists (out set), 0 if none, -1 on error. */
   int db2_ontology_eval_status(const char *rel_type, char *out, size_t out_len);

   /* List pending candidates whose occurrence_count >= threshold (ready for the
    * model to evaluate), most-observed first. Writes rel_type names into out.
    * threshold <= 0 is rejected. Returns the count written (>=0), or -1. */
   int db2_ontology_eval_candidates(int threshold, char (*out)[REL_TYPE_NAME_MAX], int max);

   /* Apply APPROVE: promote the provisional rel_type to status='active' and mark
    * the evaluation 'approved'. Both in one transaction. Requires an existing
    * evaluation row. Returns 0 on success, -1 on error. */
   int db2_ontology_approve(const char *rel_type);

   /* Apply MAP: record that `novel` aliases onto canonical `target`
    * (evaluation status='mapped', mapped_to=target) and retire the provisional
    * rel_type row (status='mapped') so it is no longer an independent active type.
    * `target` must resolve to an existing rel_type. Returns 0/-1. (Rewriting
    * existing edges / resolving novel->target on read is deferred to wiring.) */
   int db2_ontology_map(const char *novel, const char *target);

   /* Apply REJECT: mark the evaluation 'rejected' and the provisional rel_type
    * 'rejected'. Returns 0/-1. */
   int db2_ontology_reject(const char *rel_type);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ONTOLOGY_EVOLUTION_H */
