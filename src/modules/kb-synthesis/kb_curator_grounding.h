#ifndef DEC_KB_CURATOR_GROUNDING_H
#define DEC_KB_CURATOR_GROUNDING_H 1

#include <aimee/kb-synthesis/module_api.h>

#include <stdint.h>

struct cJSON;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Implemented by aimee-kb's event-bus adapter. Grounding policy is owned by
    * the separately supervised kb-synthesis process; the curator only shapes
    * bounded request data and applies the returned decision. */
   typedef int (*kb_curator_grounding_provider_fn)(
       aimee_kb_synthesis_claim_kind_t claim_kind, const char *const *claims, uint32_t claim_count,
       const char *const *callees, uint32_t callee_count,
       aimee_kb_synthesis_grounding_decision_t *decision);

   void kb_curator_grounding_register_provider(kb_curator_grounding_provider_fn provider);

   /* Request one bounded grounding decision. Returns 0 only after a registered
    * provider supplies a valid module decision. Missing providers, values that
    * exceed the wire bounds, and event-bus failures return -1; there is no local
    * policy fallback. */
   int kb_curator_grounding_decide(const struct cJSON *payload, const char *const *callees,
                                   uint32_t callee_count,
                                   aimee_kb_synthesis_grounding_decision_t *decision);

#ifdef __cplusplus
}
#endif

#endif
