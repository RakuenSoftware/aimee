/* db2/typed_facts.h: compatibility surface for the typed-fact knowledge layer.
 *
 * CSS migration callers retain this API, but assertions now use the canonical
 * entity_edges/fact_evidence store and the authority-aware fact mutation seam.
 * The former typed_facts table remains migration history only.
 *
 * The write gate validates each relation against a seed ontology (unknown
 * relation -> reject) and the subject/object kinds against the relation's
 * allowed kinds. A contradicting assert (same subject+relation, different
 * object) supersedes the prior assertion through an auditable graph commit.
 */
#ifndef DEC_DB2_TYPED_FACTS_H
#define DEC_DB2_TYPED_FACTS_H 1

#include "../headers/aimee.h"

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

   typedef enum
   {
      TYPED_FACT_OK = 0,             /* asserted (possibly superseding a prior value) */
      TYPED_FACT_UNCHANGED = 1,      /* identical active fact already present (no-op) */
      TYPED_FACT_REJECTED_REL = -2,  /* relation not in the ontology */
      TYPED_FACT_REJECTED_KIND = -3, /* subject/object kind not allowed for the relation */
      TYPED_FACT_ERROR = -1
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
