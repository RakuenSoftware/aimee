/* learning_evidence.h: evidence capture into the charter artifact pipeline.
 *
 * These functions write evidence artifacts (state=proposed) into the DB2
 * artifacts table.  They are called from aimee-kb handlers; they do not
 * block the interactive hot path and silently no-op when DB2 is unavailable.
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

   /* Promote a committed candidate into its charter target surface, writing a
    * target-surface row plus an audit_events row with before/after. `threshold`
    * is the per-surface promotion threshold: candidates at or above it
    * auto-apply; within a borderline window just below it they auto-apply with
    * the audit flagged_for_review; below the window they are held (no surface
    * write). Returns 1 if promoted, 0 if held for review, -1 on error / not a
    * committed candidate, -2 if the target surface is not yet wired. Wires the
    * anti_pattern surface (anti_pattern + mistake_pattern kinds, into the
    * anti_patterns table), the memory surface (preference kind, through the
    * memory-public-contract typed-verb store path,
    * db2_kb_service_memory_insert_json), and the workflow_pattern surface
    * (workflow kind, into the workflow_patterns table). */
   int learning_promote(const char *candidate_id, double threshold);

   /* Charter target surface a committed candidate of `candidate_kind` routes to
    * (preference->memory, workflow->workflow_pattern, anti_pattern->anti_pattern,
    * mistake_pattern->anti_pattern). Returns NULL for an unknown kind. */
   const char *learning_candidate_target_surface(const char *candidate_kind);

   /* Judge a proposed candidate: if it is corroborated by at least
    * min_corroboration distinct cited evidence sources, transition it
    * proposed -> committed and write an audit_events row routed to the
    * candidate kind's target surface (with before/after + corroboration count).
    * Returns 1 if committed, 0 if left proposed (insufficient corroboration),
    * -1 on error / unknown kind. */
   int learning_judge_commit(const char *candidate_id, const char *candidate_kind,
                             int min_corroboration);

   /* Retroactive thumbs-down on a committed candidate: roll it back to the
    * state recorded in its most recent audit before_snapshot (default
    * 'proposed') and capture verdict_tag / verdict_scope / counter_example in
    * the audit log. Returns 0 on success, -1 on error. */
   int learning_review_rollback(const char *candidate_id, const char *verdict_tag,
                                const char *verdict_scope, const char *counter_example);

   /* Idempotent ingest for a non-doc charter evidence kind. source_kind is one
    * of: session_turn, session_summary, feedback_positive, feedback_negative,
    * tool_outcome, guardrail_event, benchmark_trace, workflow_pattern. The
    * artifact records source_kind (= artifact kind), scope_kind/scope_id, and a
    * content_hash (FNV-1a of kind+content, in source_bundle_hash and payload);
    * re-ingesting identical content collapses to the existing artifact. Returns
    * the chosen id in artifact_id_out (>= 37 bytes; may be NULL).
    * Returns 0 on success, -1 on error / unknown kind. Does no LLM work, so it
    * is safe on the interactive hot path. */
   int learning_evidence_write_event(const char *source_kind, const char *scope_kind,
                                     const char *scope_id, const char *content,
                                     const char *operator_id, char *artifact_id_out,
                                     int artifact_id_out_len);

   /* Emit a retrieval_event artifact capturing one recall invocation.
    * query_fingerprint: opaque hash of the recall query (may be "").
    * role: charter recall role (e.g. "Recall").
    * surfaced_ids: array of n_surfaced memory row ids.
    * id_out: receives the new UUID (>= 37 bytes); may be NULL.
    * Returns 0 on success, -1 on error. Silently no-ops if DB2 is unavailable. */
   int learning_evidence_write_retrieval_event(const char *query_fingerprint, const char *role,
                                               const int64_t *surfaced_ids, int n_surfaced,
                                               char *id_out, int id_out_len);

   /* Emit a retrieval_attribution artifact linking one surfaced row to a verdict.
    * retrieval_event_id: UUID of the originating retrieval_event.
    * surfaced_row_id: memory row id that contributed.
    * verdict: one of the DEMOTION_VERDICT_* constants from db2/demotion.h.
    * weight: contribution fraction in [0, 1].
    * Returns 0 on success, -1 on error. Silently no-ops if DB2 is unavailable. */
   int learning_evidence_write_retrieval_attribution(const char *retrieval_event_id,
                                                     int64_t surfaced_row_id, const char *verdict,
                                                     double weight);

#ifdef __cplusplus
}
#endif

#endif /* DEC_LEARNING_EVIDENCE_H */
