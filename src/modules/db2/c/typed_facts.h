/* db2/typed_facts.h: typed-fact knowledge layer (first slice).
 *
 * A write-validated store of typed (subject, relation, object) propositions with
 * entity kinds, provenance, confidence (0-100), and correctable history. This is
 * the buildable core of docs/proposals/pending/typed-fact-knowledge-layer.md
 * (§1 ontology + write gate, §4 correction-by-supersede), scoped to unblock the
 * CSS migration assistant's #2 convention-model upgrade. The broader proposal
 * integrates typed facts with entity_edges + a self-extending ontology; this
 * slice uses a dedicated table and a fixed seed ontology for a clean base.
 *
 * The write gate validates each relation against a seed ontology (unknown
 * relation -> reject) and the subject/object kinds against the relation's
 * allowed kinds. A contradicting assert (same subject+relation, different
 * object) SUPERSEDES the prior active row (active=0, superseded_by=new id)
 * rather than deleting it (always-retain-origin).
 */
#ifndef DEC_DB2_TYPED_FACTS_H
#define DEC_DB2_TYPED_FACTS_H 1

#include "../headers/aimee.h"
#include "../headers/aimee_result.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define TYPED_FACT_STR_MAX 512

   typedef struct
   {
      int64_t id;
      char subject[TYPED_FACT_STR_MAX];
      char subject_kind[64];
      char relation[128];
      char object[TYPED_FACT_STR_MAX];
      char object_kind[64];
      int confidence; /* 0-100 */
      char source[128];
      char asserted_at[40];
   } typed_fact_t;

   /* The write gate's outcomes, in the project result convention
    * (../headers/aimee_result.h).
    *
    * The two rejections used to be negative, which said the gate malfunctioned
    * when in fact it ran and answered. A relation outside the ontology is the
    * gate working: it was asked whether this proposition may be stored and it
    * said no. They are positives now, and they carry band codes rather than the
    * universal AIMEE_DONE_REFUSED because a caller has to be able to tell "that
    * relation is not a relation" from "that relation does not accept those
    * kinds" -- the first is a vocabulary error, the second a typing one, and
    * they are fixed differently.
    *
    * Nothing here is zero, because zero is now reserved for work that has not
    * finished, and this gate always finishes. */
   typedef enum
   {
      /* Asserted, possibly superseding a prior value. */
      TYPED_FACT_ASSERTED = AIMEE_DONE,
      /* An identical active fact was already present, so nothing ran. */
      TYPED_FACT_UNCHANGED = AIMEE_DONE_ALREADY,
      /* The relation is not in the seed ontology. */
      TYPED_FACT_REJECTED_REL = AIMEE_BAND_MEMORY + 1,
      /* The subject or object kind is not allowed for this relation. */
      TYPED_FACT_REJECTED_KIND = AIMEE_BAND_MEMORY + 2,
      /* The arguments were incoherent -- no subject, relation, or object -- so
       * the gate was never asked anything. Distinct from a rejection: one is an
       * answer about the proposition, the other is that there was none. */
      TYPED_FACT_INVALID = AIMEE_INVALID,
      /* There was no connection to ask. */
      TYPED_FACT_UNAVAILABLE = AIMEE_UNAVAILABLE,
      /* A statement failed partway. The transaction is rolled back. */
      TYPED_FACT_ERROR = AIMEE_FAILED
   } typed_fact_assert_t;

   /* Is `relation` known to the seed ontology? (exposed for callers/tests) */
   int typed_fact_relation_known(const char *relation);

   /* Validate + assert a typed fact. now_iso is the caller's timestamp. On a
    * contradiction (same subject+relation, different object) the prior active row
    * is superseded. Returns a typed_fact_assert_t. */
   int db2_typed_fact_assert(const char *subject, const char *subject_kind, const char *relation,
                             const char *object, const char *object_kind, int confidence,
                             const char *source, const char *now_iso);

   /* Recall active facts for a subject; relation_filter NULL/"" = all relations.
    * Returns count written (<= max), -1 on error. */
   int db2_typed_fact_recall(const char *subject, const char *relation_filter, typed_fact_t *out,
                             int max);

   /* Active facts for a relation across all subjects (e.g. every component's
    * should_match). relation required. */
   int db2_typed_fact_by_relation(const char *relation, typed_fact_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_TYPED_FACTS_H */
