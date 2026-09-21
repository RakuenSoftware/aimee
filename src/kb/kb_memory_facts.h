/* C connection adapter for the Go-owned memory-fact pipeline. */
#ifndef DEC_KB_MEMORY_FACTS_H
#define DEC_KB_MEMORY_FACTS_H 1

/* Drain up to `batch` pending "memory_facts" jobs. Returns the number processed
 * (0 when none or when the memory module is unavailable). */
int kb_memory_facts_drain(int batch);

#endif /* DEC_KB_MEMORY_FACTS_H */
