/* kb_memory_facts.h: background LLM extraction of typed facts from memories.
 *
 * memory.store enqueues a "memory_facts" async job per stored memory (gated on
 * typed_facts_enabled). The curator drain calls kb_memory_facts_drain(), which
 * claims pending jobs, asks the curator LLM for subject-relation-object triples
 * grounded in the memory's content, and commits each through the typed-fact gate
 * (db2_fact_commit). This is the general extractor that replaces the narrow
 * pattern set for conversational/personal facts; the deterministic pattern
 * extractor still runs inline for high-precision possessive/IP/location facts.
 */
#ifndef DEC_KB_MEMORY_FACTS_H
#define DEC_KB_MEMORY_FACTS_H 1

#include "config.h"

/* Drain up to `batch` pending "memory_facts" jobs. Returns the number processed
 * (0 when none, when typed_facts_enabled is off, or when DB2 is unavailable). */
int kb_memory_facts_drain(const config_t *cfg, int batch);

#endif /* DEC_KB_MEMORY_FACTS_H */
