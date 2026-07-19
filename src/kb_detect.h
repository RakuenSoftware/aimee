/* kb_detect.h: shadow-mode drift detection for KB retrieval.
 * See
 * docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md
 */
#ifndef DEC_KB_DETECT_H
#define DEC_KB_DETECT_H 1

#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Feed one search event into the drift detector.
    * mean_dense_score: mean dense score across candidates for this query.
    * n_candidates:     number of candidates returned.
    * In shadow mode (drift_detect_shadow_enabled == 1), emits a drift_signal
    * artifact when the score distribution shifts significantly.
    * Returns 0 on success, -1 on error. */
   int kb_detect_observe(const config_t *cfg, double mean_dense_score, int n_candidates);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_DETECT_H */
