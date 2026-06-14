/* fact_ingest.h: the pattern-first typed-fact ingest pipeline (typed-fact §6 ->
 * §1). P5. Wires the pure extractor (memory_extract_patterns) to the typed gate
 * (db2_fact_commit): each high-precision candidate triple from a turn's text is
 * routed through the gate, which validates it against the ontology and (when
 * enabled) writes a semantic edge / stages a novel rel_type. This is the first
 * live writer of the typed layer; the remaining ingress HOOK that calls it from
 * the server's ingest path is wiring layered on top. */
#ifndef DEC_DB2_FACT_INGEST_H
#define DEC_DB2_FACT_INGEST_H 1

#include "fact_lifecycle.h" /* fact_authority_t */

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run pattern-first ingest on one turn's `text`: extract candidate triples
    * (§6) and route each through the typed gate db2_fact_commit with `authority`
    * (§5 class keying). `enabled` is the master gate (config.typed_facts_enabled):
    * when 0 the gate is observe-only and nothing is written. Returns the number of
    * triples the gate would commit (ACCEPT or NOVEL) when enabled — i.e. the count
    * actually written — or 0 when disabled / nothing matched, or -1 on bad args.
    *
    * Retraction is NOT handled here: callers should first check
    * memory_pattern_is_retraction() and route a correction through
    * db2_fact_retract() instead of ingesting the turn as new assertions. */
   int db2_fact_ingest_text(const char *text, fact_authority_t authority, int enabled);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FACT_INGEST_H */
