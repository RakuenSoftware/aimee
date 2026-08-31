/* kb_memory_facts.h: background LLM extraction of typed facts from memories.
 *
 * memory.store enqueues a "memory_facts" async job per stored memory (gated on
 * unconditional now). The curator drain calls kb_memory_facts_drain(), which
 * claims pending jobs, asks the curator LLM for subject-relation-object triples
 * grounded in the memory's content, and commits each through the typed-fact gate
 * (db2_fact_commit). This is the general extractor that replaces the narrow
 * pattern set for conversational/personal facts; the deterministic pattern
 * extractor still runs inline for high-precision possessive/IP/location facts.
 */
#ifndef DEC_KB_MEMORY_FACTS_H
#define DEC_KB_MEMORY_FACTS_H 1

#include <stddef.h>
#include <stdint.h>

/* Validate an end-exclusive byte span and derive its stable locator and
 * region hash. Exposed so the extraction contract can be unit-tested without
 * an LLM. Returns 0 on success, -1 for an invalid/out-of-bounds span. */
int kb_memory_fact_evidence_span(const char *content, int64_t source_start, int64_t source_end,
                                 char *span_out, size_t span_cap, char *hash_out,
                                 size_t hash_cap);
/* Drain up to `batch` pending "memory_facts" jobs. Returns the number processed
 * (0 when none, or when DB2 is unavailable). */
int kb_memory_facts_drain(int batch);

#endif /* DEC_KB_MEMORY_FACTS_H */
