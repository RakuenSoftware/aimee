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
#include "fact_mutation.h"  /* fact_evidence_input_t */

#ifdef __cplusplus
extern "C"
{
#endif

#define DB2_FACT_SUBJECT_MAX  128
#define DB2_FACT_REL_TYPE_MAX 64
#define DB2_FACT_OBJECT_MAX   128
#define DB2_FACT_ATTR_MAX     128

   typedef struct
   {
      char subject[DB2_FACT_SUBJECT_MAX];
      char rel_type[DB2_FACT_REL_TYPE_MAX];
      char object[DB2_FACT_OBJECT_MAX];
      int subject_kind;
      int object_kind;
   } db2_fact_candidate_t;

   typedef int (*db2_fact_extract_fn)(const char *text, db2_fact_candidate_t *out, int max,
                                      int *count);
   typedef int (*db2_fact_scan_fn)(const char *text, int *is_retraction, int *has_attr,
                                   char attr[DB2_FACT_ATTR_MAX]);

   /* Internal declarations of the host contracts exported publicly through
    * <aimee/db2/host_contracts.h>. NULL removes the corresponding provider. */
   void aimee_db2_register_fact_extract_provider(db2_fact_extract_fn provider);
   void aimee_db2_register_fact_scan_provider(db2_fact_scan_fn provider);

   /* Run pattern-first ingest on one turn's `text`: extract candidate triples
    * (§6) and route each through the typed gate db2_fact_commit with `authority`
    * (§5 class keying). `enabled` is the master gate (config.typed_facts_enabled):
    * when 0 the gate is observe-only and nothing is written. Returns the number of
    * extracted triples the gate accepted or staged (verdict ACCEPT or NOVEL) when
    * enabled — note a re-ingest of an already-known triple still counts (it bumps
    * the edge weight rather than inserting a row), so this is a "facts gated in"
    * count, not a "new rows inserted" count. 0 when disabled / nothing matched, -1
    * on bad args.
    *
    * Retraction is NOT handled here; the full ingress path obtains the memory
    * module's scan verdict before routing a correction through db2_fact_retract. */
   int db2_fact_ingest_text(const char *text, fact_authority_t authority, int enabled);
   int db2_fact_ingest_text_with_evidence(const char *text, fact_authority_t authority, int enabled,
                                          const fact_evidence_input_t *evidence,
                                          const char *assertion_kind, const char *valid_from,
                                          const char *valid_until);
   int db2_fact_ingest_text_as_actor(const char *text, const fact_actor_t *actor, int enabled,
                                     const fact_evidence_input_t *evidence,
                                     const char *assertion_kind, const char *valid_from,
                                     const char *valid_until);

   /* The full per-turn typed-fact ingress orchestration (the KB context_block
    * seam): when config.typed_facts_enabled, run §6 ingest — or §4 retraction on a
    * retraction-cue turn — then write the §7-PII-gated recall block (the user's
    * facts + facts about entities named in the turn) into facts_out. No-op with
    * facts_out="" when the flag is off or the query is empty. Returns the number
    * of recalled facts (>=0).
    *
    * `authority` is the WRITE authority of the retraction this turn may perform,
    * and is explicit because it decides whether a Class-A user fact can be deleted
    * here. It must be derived from the caller's authenticated identity — never
    * from the request body and never from `query`, which on every current surface
    * is a model-supplied string (verify-then-trust, kb_verifier.h). A caller that
    * cannot prove an authenticated human actor passes FACT_AUTHORITY_MODEL, and
    * db2_fact_retract's §4/§5 guards then leave user-stated facts standing. */
   int db2_typed_fact_ingress(const char *query, fact_authority_t authority, char *facts_out,
                              size_t facts_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FACT_INGEST_H */
