/* learning_evidence.h: evidence capture into the charter artifact pipeline.
 *
 * These functions write evidence artifacts (state=proposed) into the KB_STORE
 * artifacts table.  They are called from aimee-kb handlers; they do not
 * block the interactive hot path and silently no-op when KB_STORE is unavailable.
 *
 * See docs/proposals/done/cross-source-learning-substrate.md */
#ifndef DEC_LEARNING_EVIDENCE_H
#define DEC_LEARNING_EVIDENCE_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Record explicit feedback as a charter evidence artifact.
    * polarity: "positive" or "negative".
    * title: short description of the feedback signal.
    * description: longer context (may be NULL).
    * operator_id: the operator this feedback belongs to (may be NULL).
    * Returns the artifact id in artifact_id_out (must be >= 37 bytes) or ""
    * on failure.  Return value: 0 on success, -1 on error. */
   int learning_evidence_write_feedback(const char *polarity, const char *title,
                                        const char *description, const char *operator_id,
                                        char *artifact_id_out, int artifact_id_out_len);

   /* Emit a retrieval_event artifact capturing one recall invocation.
    * query_fingerprint: opaque hash of the recall query (may be "").
    * role: charter recall role (e.g. "Recall").
    * surfaced_ids: array of n_surfaced memory row ids.
    * id_out: receives the new UUID (>= 37 bytes); may be NULL.
    * Returns 0 on success, -1 on error. Silently no-ops if KB_STORE is unavailable. */
   int learning_evidence_write_retrieval_event(const char *query_fingerprint, const char *role,
                                               const int64_t *surfaced_ids, int n_surfaced,
                                               char *id_out, int id_out_len);

   /* Emit a retrieval_attribution artifact linking one surfaced row to a verdict.
    * retrieval_event_id: UUID of the originating retrieval_event.
    * surfaced_row_id: memory row id that contributed.
    * verdict: one of the DEMOTION_VERDICT_* constants from kb_store/demotion.h.
    * weight: contribution fraction in [0, 1].
    * Returns 0 on success, -1 on error. Silently no-ops if KB_STORE is unavailable. */
   int learning_evidence_write_retrieval_attribution(const char *retrieval_event_id,
                                                     int64_t surfaced_row_id, const char *verdict,
                                                     double weight);

#ifdef __cplusplus
}
#endif

#endif /* DEC_LEARNING_EVIDENCE_H */
