#ifndef AIMEE_FIDELITY_CHECK_H
#define AIMEE_FIDELITY_CHECK_H

#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* auditable-correctness P3 — fidelity-check gating.
    *
    * Returns nonzero iff the fidelity entailment judge may run for a turn: the
    * `fidelity_check_enabled` flag is on AND its hard dependencies
    * (`kb_evidence_emit_enabled`, `ingress_preinject_enabled`) are also on. The
    * judge binds against the per-turn retrieval_event, which exists only when
    * evidence emission and ingress pre-injection both run (D11).
    *
    * FAIL-CLOSED: a NULL cfg or any dependency being off returns 0, and the caller
    * records the turn as `not_evaluated` rather than running the judge against
    * absent evidence. This is enforced here, at the run site — not in
    * config_validate (which is per-key and only warns in non-strict mode). The
    * judge producer itself is a later, default-off increment. */
   int fidelity_check_eligible(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_FIDELITY_CHECK_H */
