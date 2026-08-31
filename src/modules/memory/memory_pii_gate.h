/* memory_pii_gate.h: per-attribute PII sensitivity gating for recall (typed-fact
 * §7). P5. Pure decision logic — no DB/config dependency, unit-tested directly.
 *
 * On the pre-injection recall path, sensitive facts are withheld from the
 * <aimee-context> envelope unless the current turn explicitly asks for them,
 * while identity facts needed for normal operation (preferred name, role) always
 * pass at a low confidence floor. Sensitivity is keyed off the rel_type's
 * `sensitivity` tier (§1), which is NOT NULL and defaults to `pii`, so a learned
 * or seed-omitted attribute fails CLOSED (withheld) rather than leaking.
 *
 * This is the decision core; wiring it into ingress_preinject_* (reading each
 * recalled fact's rel_type sensitivity + the turn text) is layered on top. */
#ifndef DEC_MEMORY_PII_GATE_H
#define DEC_MEMORY_PII_GATE_H 1

#include "rel_types.h" /* rel_sensitivity_t */

#ifdef __cplusplus
extern "C"
{
#endif

   /* The minimum confidence an identity/normal fact needs to be injected (§7). */
#define PII_GATE_CONFIDENCE_FLOOR 0.4

   /* Does the turn explicitly request sensitive/PII information? Case-insensitive
    * scan for cues ("address", "phone number", "email", "birthday", "password",
    * "credential", "where do i live", ...). NULL/empty -> 0. */
   int memory_pii_turn_requests_sensitive(const char *turn_text);

   /* The sensitivity tier governing a rel_type: from the seed ontology when
    * known; for an unknown type it defaults OPEN (SENS_NORMAL) so free-form
    * extracted relations are not all withheld, except names that plainly denote
    * a credential (SENS_SECRET) or a regulated PII identifier (SENS_PII). */
   rel_sensitivity_t memory_pii_rel_sensitivity(const char *rel_type);

   /* Recall-path decision: should a fact of `sens` and `confidence` be injected
    * into the pre-injection envelope, given whether the turn requests sensitive
    * info? SENS_NORMAL injects at confidence >= floor; SENS_PII injects only when
    * the turn requests it (and >= floor); SENS_SECRET never injects (credentials
    * are served through the vault, never the pre-injection context). Returns 1 to
    * inject, 0 to withhold. */
   int memory_pii_should_inject(rel_sensitivity_t sens, double confidence,
                                int turn_requests_sensitive);

   /* Turn classifier: returns 0 and sets *requests_sensitive to 0 or 1, or
    * non-zero if it could not produce an answer. */
   typedef int (*memory_pii_turn_classifier_fn)(const char *turn_text,
                                                int *requests_sensitive);

   /* Route the turn classification through `classifier` (the memory module over
    * the bus) instead of scanning in-process. Pass NULL to go back to the local
    * scan.
    *
    * A registered classifier is authoritative, and unlike the write gate there
    * is no way to defer: the answer is a plain 0/1 with no third state, and the
    * recall path has to proceed either way. So a failure fails CLOSED -- the
    * turn is treated as NOT asking for sensitive information, which withholds
    * PII rather than leaking it. The cost of a broken module is missing facts,
    * never an exposed one; falling back to the local scan would instead make a
    * broken module invisible. */
   void memory_pii_register_turn_classifier(memory_pii_turn_classifier_fn classifier);

   /* Classify a whole block of relations at once. Writes `count` tiers into
    * `out`. Returns 0, or -1 if no answer could be produced.
    *
    * Batched because the recall path gates every candidate fact it read: the
    * round trips are the cost, not the classifying. With no classifier
    * registered this is exactly memory_pii_rel_sensitivity in a loop.
    *
    * A -1 means the caller learned nothing about these relations. It must NOT
    * treat that as "all normal" — withhold the block and let the failure
    * surface. */
   int memory_pii_rel_sensitivity_batch(const char *const *rel_types, int count,
                                        rel_sensitivity_t *out);

   /* Batch classifier: returns 0 and fills `out`, or non-zero if it could not
    * answer for the whole batch. Partial answers are not a thing — a block is
    * classified or it is not. */
   typedef int (*memory_pii_sensitivity_batch_fn)(const char *const *rel_types, int count,
                                                  rel_sensitivity_t *out);

   /* Route batch classification through `classifier` (the memory module over the
    * bus). Pass NULL to go back to the local table.
    *
    * Authoritative like the others: a failure is reported as a failure, never
    * quietly answered from the local table, which would make a broken module
    * look healthy. */
   void memory_pii_register_sensitivity_batch(memory_pii_sensitivity_batch_fn classifier);

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_PII_GATE_H */
