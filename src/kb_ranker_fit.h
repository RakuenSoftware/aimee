/* kb_ranker_fit.h: the Calibrate half of the KB-hybrid ranking substrate —
 * turn accumulated feature_rows + retrieval outcomes into a fitted ranker_model.
 * See docs/proposals/done/learning-to-rank-weight-fitting.md */
#ifndef DEC_KB_RANKER_FIT_H
#define DEC_KB_RANKER_FIT_H 1

#include "config.h"
#include <cJSON.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Option B — kb_hybrid outcome capture (the loop-closing plumbing).
    * These let the surface that produced kb_document feature rows also report
    * which candidates were useful, keyed to a shared retrieval_event, WITHOUT
    * touching the kb.c retrieval hot path (endpoint-driven, mirroring the memory
    * surface's evidence pattern). The training view then joins these outcomes to
    * the already-written feature_rows. See
    * docs/proposals/pending/kb-hybrid-outcome-wiring.md. */

   /* Mint a kb_hybrid retrieval_event capturing the surfaced kb_document doc_ids.
    * id_out receives the event id (>= 37 bytes; may be NULL). 0 ok / -1 error. */
   int kb_ranker_emit_event(const int64_t *doc_ids, int n, const char *query_fingerprint,
                            char *id_out, int id_out_len);

   /* Record one outcome verdict for a surfaced doc_id, tied to event_id, as a
    * `ranker_outcome` artifact. verdict per db2/demotion.h (accepted = positive).
    * 0 on success, -1 on error. */
   int kb_ranker_outcome_write(const char *event_id, int64_t doc_id, const char *verdict,
                               double weight);

   /* §1 training view: join the retrieval_attribution outcome labels to the
    * feature_rows vector for each attributed candidate, grouped by
    * retrieval_event_id. Emits one row per (retrieval_event, candidate) that has
    * BOTH a v1 feature vector and an outcome verdict.
    *
    * subject_kind      — feature_rows.subject_kind to join on; NULL → "kb_document"
    *                     (the surface the ranker reorders).
    * feature_set_version — NULL → KB_FEATURE_SET_VERSION ("v1").
    * rows_out          — receives a cJSON array (caller owns; cJSON_Delete). Each
    *                     element: {"group":<event id>, "features":{...},
    *                     "label":0|1, "verdict":"...", "weight":<double>}.
    * *_out counters (may be NULL): distinct groups, total rows, positive labels.
    *
    * Returns 0 on success (possibly zero rows — an honest empty view), -1 on error.
    *
    * NOTE: on the current substrate the ranker's features live on 'kb_document'
    * subjects while outcome labels are attributed to 'memory' row ids, so the
    * honest join is typically empty until the kb_hybrid surface is wired to emit
    * grouped per-query feature rows + outcomes. export-view surfaces exactly that. */
   int kb_ranker_training_view(const char *subject_kind, const char *feature_set_version,
                               cJSON **rows_out, int *n_groups_out, int *n_rows_out,
                               int *n_positive_out);

   /* JSON dump of the training view for `aimee kb ranker export-view`.
    * {"status":"ok","subject_kind":..,"feature_set_version":..,"n_groups":..,
    *  "n_rows":..,"n_positive":..,"fittable":bool,"rows":[...]}.
    * Returns a malloc'd string (caller frees) or NULL on error. */
   char *kb_ranker_export_view_json(const char *subject_kind, const char *feature_set_version);

   /* Phases 2+3: materialize the view, run the fitter sidecar, and — when the fit
    * succeeds AND the fitted weights beat the incumbent on the benchmark gate —
    * write a proposed ranker_model and commit it. A benchmark_trace artifact
    * records the delta whenever a fit is scored.
    *
    * Returns:
    *   0  a model was committed (fit passed the gate);
    *   1  proposed-only / refused (no lift, below floor, version mismatch,
    *      degenerate labels, sidecar unavailable) — the {0.6,0.4} default is kept;
    *  -1  hard error (bad config, DB failure).
    * id_out (may be NULL) receives the written artifact id (committed or proposed).
    * report_out (may be NULL) receives a malloc'd JSON report (caller frees). */
   int kb_ranker_fit_run(const config_t *cfg, char *id_out, int id_out_len, char **report_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_RANKER_FIT_H */
