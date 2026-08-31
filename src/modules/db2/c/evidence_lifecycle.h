/* evidence_lifecycle.h: typed C boundary over the P1-P9 PostgreSQL contract. */
#ifndef DEC_DB2_EVIDENCE_LIFECYCLE_H
#define DEC_DB2_EVIDENCE_LIFECYCLE_H 1

#include "fact_mutation.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      EL_CHANGESET_SHOW = 1,
      EL_CHANGESET_DIFF,
      EL_CHANGESET_PREVIEW_REVERT,
      EL_CHANGESET_REVERT,
      EL_DOCUMENT_PREVIEW,
      EL_DOCUMENT_APPLY,
      EL_DERIVED_STATUS,
      EL_OUTCOME_RECORD,
      EL_REVIEW_LIST,
      EL_REVIEW_DECIDE,
      EL_ONTOLOGY_EXPORT,
      EL_ONTOLOGY_IMPORT,
      EL_ONTOLOGY_DRY_RUN,
      EL_ONTOLOGY_MIGRATE,
      EL_ONTOLOGY_REPORT,
      EL_ONTOLOGY_ROLLBACK,
      EL_RECALL_TRACE_RECORD,
      EL_RECALL_TRACE_GET,
   } evidence_lifecycle_op_t;

#define EL_MAX_ARGS 16

   /* Invoke one fixed operation. Arguments are data-only bound parameters; SQL
    * and actor/authority are selected internally. The result is one JSON value. */
   int db2_evidence_lifecycle_json(const fact_actor_t *actor, evidence_lifecycle_op_t op,
                                   const char *const *args, int nargs, char *out, int out_cap);

   /* P5 read projection used by the existing audit provenance API/CLI.  The
    * evaluator and task/correction fields are exported verbatim; prose memory
    * content is never copied into the outcome overlay. */
   int db2_work_outcomes_for_retrieval_json(const char *retrieval_event_id, char *out,
                                            int out_cap);

#ifdef __cplusplus
}
#endif
#endif
