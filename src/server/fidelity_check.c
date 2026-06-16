/* server/fidelity_check.c: auditable-correctness P3 fidelity-check gating.
 *
 * The fidelity entailment judge (a later, default-off increment) runs only when
 * the operator has turned it on AND its hard dependencies are satisfied. This
 * fail-closed eligibility check is enforced at the run site rather than in
 * config_validate (which is per-key and merely warns in non-strict mode), so the
 * judge can never run against absent evidence — when ineligible the caller records
 * the turn as not_evaluated. */

#include "fidelity_check.h"

int fidelity_check_eligible(const config_t *cfg)
{
   if (!cfg)
      return 0;
   /* Hard dependency chain (D11): the per-turn retrieval_event the judge binds
    * against exists only when evidence emission AND ingress pre-injection are on. */
   return cfg->fidelity_check_enabled && cfg->kb_evidence_emit_enabled &&
          cfg->ingress_preinject_enabled;
}
