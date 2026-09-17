/* Legacy typed-fact commit result codes still consumed by DB2 callers.
 * The authoritative gate is implemented in Go memory. This header declares no
 * C gate, callback or transport; its remaining type consumers are pending G0.
 */
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
                                     issue), or the memory owner could not answer. The fact
                                     was NOT committed; the caller must retry/defer and must never
                                     treat it as success. No local fallback may turn an
                                     unavailable owner into acceptance. */
      FACT_GATE_REJECT_SENSITIVE, /* validated but WITHHELD from the shared KB: a
                                     credential/regulated-PII relation. Personal/sensitive
                                     facts stay in the user's local DB1, never DB2. Not
                                     committed; caller treats it as not-stored, not an error.
                                     (commit path only — the pure gate never returns it) */
   } fact_gate_verdict_t;

#ifdef __cplusplus
}
#endif

#endif /* DEC_MEMORY_FACT_GATE_H */
