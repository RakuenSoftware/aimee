/* kb_detect.c: shadow-mode drift detection for KB retrieval.
 * See
 * docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md
 */

#include "kb_detect.h"
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "aimee.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EMA state for mean dense score; reset per process lifetime. */
static struct
{
   double ema;        /* exponential moving average of mean_dense_score */
   double ema_sq;     /* EMA of squared score for variance estimate */
   int n;             /* observations seen */
   int n_since_alert; /* observations since last drift_signal was emitted */
} g_ema = {0.0, 0.0, 0, 0};

#define EMA_ALPHA         0.05 /* smoothing factor */
#define DRIFT_Z_THRESHOLD 3.0  /* z-score threshold to emit drift_signal */
#define DRIFT_MIN_OBS     20   /* min observations before checking drift */
#define DRIFT_COOLDOWN    50   /* min observations between consecutive signals */

int kb_detect_observe(double mean_dense_score, int n_candidates)
{
   (void)n_candidates;
   if (!config_drift_detect_shadow_enabled())
      return 0;
   if (isnan(mean_dense_score))
      return 0;

   if (g_ema.n == 0)
   {
      g_ema.ema = mean_dense_score;
      g_ema.ema_sq = mean_dense_score * mean_dense_score;
   }
   else
   {
      g_ema.ema = EMA_ALPHA * mean_dense_score + (1.0 - EMA_ALPHA) * g_ema.ema;
      g_ema.ema_sq =
          EMA_ALPHA * (mean_dense_score * mean_dense_score) + (1.0 - EMA_ALPHA) * g_ema.ema_sq;
   }
   g_ema.n++;
   g_ema.n_since_alert++;

   if (g_ema.n < DRIFT_MIN_OBS || g_ema.n_since_alert < DRIFT_COOLDOWN)
      return 0;

   double variance = g_ema.ema_sq - g_ema.ema * g_ema.ema;
   if (variance <= 0.0)
      return 0;
   double stddev = sqrt(variance);
   double z = fabs(mean_dense_score - g_ema.ema) / stddev;
   if (z < DRIFT_Z_THRESHOLD)
      return 0;

   /* Write drift_signal evidence artifact (advisory; does not demote anything). */
   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"surface\":\"kb_hybrid\",\"signal_kind\":\"dense_score_drift\","
            "\"z_score\":%.4f,\"ema\":%.6f,\"observed\":%.6f,\"n_obs\":%d}",
            z, g_ema.ema, mean_dense_score, g_ema.n);

   int rc =
       db2_artifact_write(id, "drift_signal", "proposed", "system", "kb_hybrid", "", 1.0, payload);
   if (rc == 0)
   {
      aimee_log(LOG_DEBUG, "kb_detect", "drift_signal emitted id=%s z=%.2f ema=%.4f obs=%.4f", id,
                z, g_ema.ema, mean_dense_score);
      g_ema.n_since_alert = 0;
   }
   return rc;
}
