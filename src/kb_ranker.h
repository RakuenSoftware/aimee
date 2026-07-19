/* kb_ranker.h: linear in-process ranker for KB-hybrid retrieval.
 * See
 * docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md
 */
#ifndef DEC_KB_RANKER_H
#define DEC_KB_RANKER_H 1

#include "config.h"
#include "kb_features.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Re-rank up to n candidates using the active ranker model.
    * doc_ids[], lex_scores[], dense_scores[], age_days[]: per-candidate inputs.
    * ranked_out[]:  receives re-ordered doc_ids (caller must provide n slots).
    * scores_out[]:  receives per-candidate ranker scores; may be NULL.
    * Returns n on success, 0 if disabled or no model loaded (caller uses RRF order). */
   int kb_ranker_rerank(const config_t *cfg, const int64_t *doc_ids, const double *lex_scores,
                        const double *dense_scores, const double *age_days, int n,
                        int64_t *ranked_out, double *scores_out);

   int kb_ranker_rerank_with_sketch(const config_t *cfg, const int64_t *doc_ids,
                                    const double *lex_scores, const double *dense_scores,
                                    const double *age_days,
                                    const kb_sketch_features_t *sketch_features, int n,
                                    int64_t *ranked_out, double *scores_out);

   /* Write a ranker_model artifact with linear weights for surface kb_hybrid.
    * weights_json: e.g. {"dense.cos":0.6,"lex.cos":0.4,"temp.recency":0.0}
    * id_out: receives artifact UUID (>= 37 bytes); may be NULL.
    * Returns 0 on success, -1 on error. */
   int kb_ranker_model_write(const char *weights_json, char *id_out, int id_out_len);

   /* Load the most recently committed ranker_model artifact for surface kb_hybrid.
    * Populates an in-process weight cache.
    * Returns 0 if a model was loaded, -1 if none found (caller uses RRF). */
   int kb_ranker_model_load(void);

   /* Write a ranker_model artifact in the 'proposed' state (stamps
    * target_surface='kb_hybrid' but NOT committed_at). Unlike kb_ranker_model_write
    * — which force-commits — this leaves the model behind the promotion gate: a
    * fitted model must clear the benchmark before kb_ranker_model_commit lands it.
    * fit_metrics_json (may be NULL) is embedded under "fit_metrics" for provenance.
    * id_out receives the artifact UUID (>= 37 bytes); may be NULL.
    * Returns 0 on success, -1 on error. */
   int kb_ranker_model_write_proposed(const char *weights_json, const char *fit_metrics_json,
                                      char *id_out, int id_out_len);

   /* Promote a proposed ranker_model to 'committed' (stamps committed_at so
    * kb_ranker_model_load's ORDER BY committed_at DESC selects it). Idempotent.
    * Returns 0 on success, -1 on error. */
   int kb_ranker_model_commit(const char *id);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_RANKER_H */
