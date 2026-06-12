/* roundtable_pipeline_capture.h: server-worker-owned result capture for the
 * authoring pipeline (proposal section 4, #10/#12/#18). The worker that already
 * runs the roundtable persists every terminal attempt into the durable DB1
 * ledger BEFORE the bounded /v1/runs record can be evicted, so the done-bar is
 * read from the ledger, never from a (possibly-evicted) live run. */
#ifndef DEC_ROUNDTABLE_PIPELINE_CAPTURE_H
#define DEC_ROUNDTABLE_PIPELINE_CAPTURE_H 1

#include "roundtable_pipeline_eval.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Parse a roundtable child-run result payload (the object produced by
    * handle_delegate_roundtable: artifact/converged/items[]/... ) into a
    * captured envelope. is_draft selects draft vs review interpretation.
    * Returns 0 always; the envelope's present/parse_ok/has_error fields record
    * what happened (an empty payload -> present=0). */
   int rtp_capture_parse(const char *result_json, int is_draft, rtp_envelope_t *out);

   /* Submission seam (#20/#21): a pipeline-owned op-run carries a
    * `pipeline_pass_id`. Validate it against an active pipeline/pass and create
    * the attempt row keyed to run_id (the marker the finalize hook looks up).
    * Returns 1 if registered, 0 if pipeline_pass_id<=0 (not a pipeline run),
    * -1 if the pass id is unknown / not owned by an active pipeline. */
   int rtp_seam_register_attempt(int pipeline_pass_id, const char *run_id);

   /* Finalize seam (#18/#30): called from the op-run worker at terminal. No-ops
    * (returns 0) when run_id has no pipeline attempt row, so ordinary
    * ensemble_review/roundtable calls are untouched. Otherwise it records the
    * terminal envelope durably and, for the current non-superseded attempt,
    * updates the pass aggregate. Returns 1 if a pipeline attempt was finalized. */
   int rtp_seam_finalize(const char *run_id, int http_ok, int cancelled, const char *result_json);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROUNDTABLE_PIPELINE_CAPTURE_H */
