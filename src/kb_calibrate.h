/* kb_calibrate.h: Bayesian promotion-threshold calibration runner.
 *
 * Runs in aimee-kb: reads audit outcomes, calls the sidecar, and writes
 * calibration_profile artifacts. Promotion gates consume profiles according
 * to the configured calibration rollout mode.
 *
 * See docs/proposals/done/bayesian-promotion-threshold-calibration.md */
#ifndef DEC_KB_CALIBRATE_H
#define DEC_KB_CALIBRATE_H 1

#include "config.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* Run one calibration pass over all (target_surface, kind, scope) triples
    * that have enough audit data.  Writes calibration_profile artifacts.
    * Returns the number of profiles written (>= 0) or -1 on fatal error. */
   int kb_calibrate_run(const config_t *cfg);

   /* Sweep proposed drift_signal artifacts emitted by kb_detect and stamp
    * them reflected so the calibration loop knows they were acted on.
    * Returns the number of signals consumed (>= 0). */
   int kb_calibrate_consume_drift_signals(const config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_CALIBRATE_H */
