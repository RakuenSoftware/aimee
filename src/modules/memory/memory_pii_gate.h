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

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_PII_GATE_H */
