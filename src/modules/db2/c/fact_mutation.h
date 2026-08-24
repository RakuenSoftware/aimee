/* fact_mutation.h: authority-aware, evidence-backed semantic fact transitions.
 *
 * This is the mandatory mutation seam for entity_edges rows whose edge_class is
 * "semantic".  It owns authority comparison, explicit lifecycle transitions,
 * assertion/evidence separation, graph commit diffs, rollback, invalidation,
 * erasure reporting, and the atomic WORM audit append.
 *
 * HTTP callers never pass an authority rank.  They call
 * db2_fact_actor_from_request(), which resolves the verified kb request context.
 * Background ingestion uses db2_fact_actor_internal() with a compile-time enum.
 */
#ifndef DEC_DB2_FACT_MUTATION_H
#define DEC_DB2_FACT_MUTATION_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FACT_COMMIT_ID_MAX 37

   typedef enum
   {
      FACT_ACTOR_MODEL = 10,
      FACT_ACTOR_SYSTEM = 20,
      FACT_ACTOR_USER = 30,
      FACT_ACTOR_OPERATOR = 40,
   } fact_actor_rank_t;

   typedef struct
   {
      char principal[577];
      char transport_identity[577];
      char role[32];
      fact_actor_rank_t rank;
      int authenticated;
   } fact_actor_t;

   /* Resolve the actor from the verifier-populated request context.  When
    * require_operator is true, a verified actor or verified console-admin scope
    * is elevated to operator for this already-operator-authorized route. */
   int db2_fact_actor_from_request(int require_operator, fact_actor_t *out);

   /* Trusted background actor.  principal is fixed by rank and cannot be
    * nominated by an ingest payload.  OPERATOR is intentionally refused. */
   int db2_fact_actor_internal(fact_actor_rank_t rank, fact_actor_t *out);

   /* Capture the verifier-derived actor beside a stored memory before its
    * asynchronous fact job is enqueued, then restore that exact principal in
    * the worker.  Missing request context is recorded as model authority.
    *
    * `user_authority` is the authority the WRITE itself claims (nonzero for
    * MEMORY_AUTHORITY_USER), and it CAPS the captured actor. The caller's
    * identity may lower the recorded authority but never raise it above what the
    * note claims: a person storing agent-composed text is still storing
    * agent-composed text.
    *
    * Without the cap the actor came from the request alone, so an authenticated
    * person storing a note with provenance `agent_message` recorded a USER actor
    * -- and the drain, which reads this row rather than the provenance, then
    * minted Class-A facts from model-composed text. That is the same hazard
    * §5 exists to prevent, reached from the store side instead of the write
    * side. The two derivations of one fact must not be able to disagree. */
   int db2_fact_actor_capture_memory(int64_t memory_id, int user_authority);
   int db2_fact_actor_for_memory(int64_t memory_id, fact_actor_t *out);

#define FACT_KIND_WORLD_FACT   "world_fact"
#define FACT_KIND_EPISODE      "episode"
#define FACT_KIND_EXPERIENCE   "experience"
#define FACT_KIND_MENTAL_MODEL "mental_model"
#define FACT_KIND_PREFERENCE   "preference"
#define FACT_KIND_INSTRUCTION  "instruction"
#define FACT_KIND_POLICY       "policy"
#define FACT_KIND_HYPOTHESIS   "hypothesis"
/* Read compatibility for data produced before P6. New writes use episode. */
#define FACT_KIND_OBSERVATION  "observation"

#define FACT_LIFECYCLE_CANDIDATE   "candidate"
#define FACT_LIFECYCLE_PERSISTENT  "persistent"
#define FACT_LIFECYCLE_PROMOTED    "promoted"
#define FACT_LIFECYCLE_SUPERSEDED  "superseded"
#define FACT_LIFECYCLE_INVALIDATED "invalidated"

   typedef struct
   {
      const char *source_kind;     /* message, document, table_cell, observation... */
      const char *source_id;       /* durable message/document id; commit id fallback */
      const char *source_span;     /* byte/line/page span, serialized by the caller */
      const char *evidence_hash;   /* content/span hash */
      const char *actor_principal; /* captured source actor; never grants authority */
      const char *observed_at;     /* valid observation time */
      const char *ingest_run_id;   /* batch identity */
      const char *stance;          /* supports (default) or contradicts */
   } fact_evidence_input_t;

   typedef struct
   {
      const char *source;
      const char *relation;
      const char *target;
      int relation_id;
      int subject_kind;
      int object_kind;
      const char *confidence_class;
      double confidence;
      const char *assertion_kind;
      const char *valid_from;
      const char *valid_until;
      const fact_evidence_input_t *evidence;
      int functional; /* explicit functional relation for non-seed ingestion surfaces */
   } fact_assertion_input_t;

   typedef struct
   {
      int64_t assertion_id;
      char commit_id[FACT_COMMIT_ID_MAX];
      char lifecycle[24];
      int changed;
      int quarantined;
      int evidence_added;
   } fact_mutation_result_t;

   /* Assert/corroborate one canonical proposition.  Functional contradictions
    * are superseded only when the authenticated/trusted actor rank is at least
    * the incumbent rank; otherwise the new assertion is quarantined candidate. */
   int db2_fact_mutation_assert(const fact_actor_t *actor, const fact_assertion_input_t *input,
                                fact_mutation_result_t *out);

   /* Ordinary removal contract: reversible invalidation.  Empty target matches
    * every current value for source+relation.  Lower authority cannot invalidate
    * a higher-authority assertion. Returns rows changed, or -1. */
   int db2_fact_mutation_invalidate(const fact_actor_t *actor, const char *source,
                                    const char *relation, const char *target,
                                    fact_mutation_result_t *out);

   /* Episode/experience correction is annotation, never retraction.  Returns
    * -2 from invalidate when this route must be offered to the caller. */
   int db2_fact_mutation_annotate(const fact_actor_t *actor, int64_t assertion_id,
                                  const char *annotation, fact_mutation_result_t *out);

   typedef enum
   {
      FACT_REVIEW_APPROVE = 1,
      FACT_REVIEW_REJECT = 2,
      FACT_REVIEW_UNDO = 3,
   } fact_review_action_t;

   /* Operator review. approve -> promoted, reject -> invalidated, undo restores
    * the latest non-undone review's prior lifecycle. */
   int db2_fact_mutation_review(const fact_actor_t *actor, int64_t assertion_id,
                                fact_review_action_t action, fact_mutation_result_t *out);

   typedef struct
   {
      int64_t assertion_id;
      char object_kind[32];
      char object_key[128];
      char action[32];
      char before_lifecycle[24];
      char after_lifecycle[24];
      int before_authority_rank;
      int after_authority_rank;
      int existed_before;
      int existed_after;
      char detail[160];
   } fact_commit_change_t;

   /* Preview an applied commit's structured diff. */
   int db2_fact_commit_preview(const char *commit_id, fact_commit_change_t *out, int max);

   /* Operator-only batch rollback.  Insertions are reversibly invalidated;
    * updates restore their before envelope; evidence remains but is invalidated. */
   int db2_fact_commit_rollback(const fact_actor_t *actor, const char *commit_id,
                                char rollback_commit_id[FACT_COMMIT_ID_MAX]);

   /* Preview/rollback every commit produced by one ingest run as one atomic
    * operator action.  The returned preview is globally ordered. */
   int db2_fact_ingest_run_preview(const char *ingest_run_id, fact_commit_change_t *out, int max);
   int db2_fact_ingest_run_rollback(const fact_actor_t *actor, const char *ingest_run_id,
                                    char rollback_commit_id[FACT_COMMIT_ID_MAX]);

   typedef struct
   {
      int assertion_count;
      int evidence_count;
      char residual_data[512];
   } fact_erasure_impact_t;

   /* Preview and execute the permanent-removal contract.  Erasure is
    * operator-only, non-reversible, and emits a residual-data report. */
   int db2_fact_erasure_preview(const char *source, const char *relation, const char *target,
                                fact_erasure_impact_t *out);
   int db2_fact_erasure_execute(const fact_actor_t *actor, const char *source, const char *relation,
                                const char *target, fact_erasure_impact_t *out,
                                char commit_id[FACT_COMMIT_ID_MAX]);

   typedef struct
   {
      int64_t id;
      char source[128];
      char relation[128];
      char target[128];
      char assertion_kind[24];
      char lifecycle[24];
      int authority_rank;
      int evidence_count;
      char commit_id[FACT_COMMIT_ID_MAX];
   } fact_candidate_t;

   int db2_fact_candidates(fact_candidate_t *out, int max);

   /* Maintenance transitions also use one commit/diff/audit batch.  Recurrent
    * candidates become persistent; stale unsupported candidates are invalidated. */
   int db2_fact_mutation_promote_supported(const fact_actor_t *actor, int threshold);
   int db2_fact_mutation_expire_candidates(const fact_actor_t *actor, const char *cutoff_iso);

   /* Register a non-assertion graph mutation (ontology decision/entity merge)
    * inside the caller's already-open DB transaction.  This keeps its commit id,
    * diff and WORM row atomic with the owning mutation. */
   int db2_fact_graph_record_external_in_txn(const fact_actor_t *actor, const char *operation,
                                             const char *object_kind, const char *object_key,
                                             const char *action, const char *before_state,
                                             const char *after_state, int reversible,
                                             char commit_id[FACT_COMMIT_ID_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FACT_MUTATION_H */
