#ifndef DEC_KB_CURATOR_GROUNDING_H
#define DEC_KB_CURATOR_GROUNDING_H 1

#include <stddef.h>

struct cJSON;

#ifdef __cplusplus
extern "C"
{
#endif

   /* Return 1 if |callee| is a known side-effecting / system-call style
    * function name (I/O, filesystem, network, process, signal, env, or
    * database mutation), else 0. Case-sensitive exact match. */
   int kb_curator_callee_is_side_effecting(const char *callee);

   /* Return 1 if |payload| (a code_unit artifact payload object) claims the
    * function has NO side effects. "Claims none" means: the "side_effects"
    * key is absent, is JSON null, is an empty array, OR is an array whose
    * only entries are strings equal (case-insensitive) to "none", "no",
    * "no side effects", "pure", or "n/a". Any other non-empty content
    * counts as an honest non-empty claim (returns 0). */
   int kb_curator_payload_claims_no_side_effects(const struct cJSON *payload);

   /* Return 1 (and fill reason_out with the first offending callee name) if
    * the payload claims no side effects AND at least one entry in
    * callees[0..n_callees) is side-effecting. Otherwise return 0 and set
    * reason_out[0]='\0'. reason_out may be NULL. */
   int kb_curator_grounding_contradicts(const struct cJSON *payload, const char *const *callees,
                                        int n_callees, char *reason_out, size_t reason_len);

#ifdef __cplusplus
}
#endif

#endif