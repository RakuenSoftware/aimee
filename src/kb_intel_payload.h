/* kb_intel_payload.h: shared JSON builders for intelligence readiness routes. */
#ifndef DEC_KB_INTEL_PAYLOAD_H
#define DEC_KB_INTEL_PAYLOAD_H 1

#include "cJSON.h"

cJSON *kb_intel_calibrate_readiness_response(void);
cJSON *kb_intel_demote_check_response(void);
cJSON *kb_intel_bandit_export_response(void);

/* Record the offline-replay result (output of tools/bandit_replay.py) as a
 * benchmark_trace artifact. body_json is {decision_point, result} — `result`
 * is the replay-tool output object.
 * Returns a response object (caller frees). */
cJSON *kb_intel_bandit_replay_record_response(const char *body_json, int body_len);

/* HTTP wrapper around the response builder: writes JSON into out_buf and
 * returns the HTTP status. */
int kb_intel_bandit_replay_record_http(const char *body, int body_len, char *out_buf, int out_cap);

/* bandit.sample / bandit.close: let server-side decision points reach the DB2
 * bandit. sample selects+logs an arm ({arm, decision_id}); close records the
 * observed reward [0,1]. Request/response objects (caller frees) + HTTP wrappers. */
cJSON *kb_intel_bandit_sample_response(const char *body_json, int body_len);
cJSON *kb_intel_bandit_close_response(const char *body_json, int body_len);
int kb_intel_bandit_sample_http(const char *body, int body_len, char *out_buf, int out_cap);
int kb_intel_bandit_close_http(const char *body, int body_len, char *out_buf, int out_cap);

/* bandit.promote: persist the production-default arm for a decision point
 * (body {decision_point, arm}); returns {status, rollback_arm}. */
cJSON *kb_intel_bandit_promote_response(const char *body_json, int body_len);
int kb_intel_bandit_promote_http(const char *body, int body_len, char *out_buf, int out_cap);

/* ranker.export_view: dump the §1 learning-to-rank training view (feature_rows
 * joined to retrieval outcomes) plus the wiring-gap diagnostic. Read-only. */
cJSON *kb_intel_ranker_export_view_response(void);

/* ranker.fit: materialize the training view, run the fitter sidecar, and
 * benchmark-gate/commit the fitted ranker_model. Writes the report into out_buf
 * and returns the HTTP status. Gated by intelligence.ranking.fit.enabled. */
int kb_intel_ranker_fit_http(const char *body, int body_len, char *out_buf, int out_cap);

#endif /* DEC_KB_INTEL_PAYLOAD_H */
