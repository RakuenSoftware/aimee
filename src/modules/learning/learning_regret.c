/* learning_regret.c: what became of a committed proposal, and what that costs
 * the detector that raised it.
 *
 * The commit ratio can only say a proposal was accepted. A detector that is
 * persuasive and wrong scores exactly like one that is right, so the gate it
 * passes through is tuned by hand and never learns. These functions close that
 * loop: a detector whose commits keep getting superseded, contradicted, or
 * reverted needs more corroboration before its next one lands, and loses the
 * immediate-commit path entirely once half of them fail to hold.
 *
 * The bar only ever moves UP. Being right is not a reason to ask for less
 * evidence — it is the expected case, and a loop that rewards agreement with
 * lower scrutiny is the echo chamber S0 exists to prevent.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <aimee/learning/learning.h>

#include "modules/db2/c/db2_learning.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char *const LEARNING_FATES[] = {
    LEARNING_FATE_STANDING,
    LEARNING_FATE_SUPERSEDED,
    LEARNING_FATE_CONTRADICTED,
    LEARNING_FATE_REVERTED,
    NULL,
};

/* Everything except STANDING means the commit did not hold. Kept as an
 * explicit list rather than "not standing" so an unrecognised fate falls
 * through to neither: a typo must not silently raise a detector's bar. */
static const char *const LEARNING_REGRET_FATES[] = {
    LEARNING_FATE_SUPERSEDED,
    LEARNING_FATE_CONTRADICTED,
    LEARNING_FATE_REVERTED,
    NULL,
};

static int fate_in(const char *fate, const char *const *list)
{
   if (!fate || !fate[0])
      return 0;
   for (int i = 0; list[i]; i++)
      if (strcmp(fate, list[i]) == 0)
         return 1;
   return 0;
}

int learning_fate_is_valid(const char *fate)
{
   return fate_in(fate, LEARNING_FATES);
}

int learning_fate_is_regret(const char *fate)
{
   return fate_in(fate, LEARNING_REGRET_FATES);
}

int learning_fate_record(int proposal_id, const char *fate, const char *reason)
{
   if (proposal_id <= 0 || !learning_fate_is_valid(fate))
      return -1;
   return db2_learning_fate_record(proposal_id, fate, reason);
}

/* The regret vocabulary as the one comma-separated list the SQL layer matches
 * against, so it cannot drift from LEARNING_REGRET_FATES above. */
static void regret_fate_list(char *buf, size_t cap)
{
   size_t o = 0;
   buf[0] = '\0';
   for (int i = 0; LEARNING_REGRET_FATES[i] && o + 1 < cap; i++)
      o += (size_t)snprintf(buf + o, cap - o, "%s%s", o ? "," : "", LEARNING_REGRET_FATES[i]);
}

int learning_metrics_regret(int window_days, learning_detector_regret_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (window_days <= 0)
      window_days = LEARNING_METRICS_DEFAULT_WINDOW_DAYS;

   char fates[128];
   regret_fate_list(fates, sizeof(fates));

   db2_learning_fate_count_t rows[LEARNING_REGRET_MAX_DETECTORS];
   int n = db2_learning_fate_counts(
       window_days, fates, rows,
       max < LEARNING_REGRET_MAX_DETECTORS ? max : LEARNING_REGRET_MAX_DETECTORS);
   if (n < 0)
      return -1;

   for (int i = 0; i < n; i++)
   {
      memset(&out[i], 0, sizeof(out[i]));
      snprintf(out[i].signal_type, sizeof(out[i].signal_type), "%s", rows[i].signal_type);
      out[i].committed = rows[i].committed;
      out[i].settled = rows[i].settled;
      out[i].regret = rows[i].regret;
      if (rows[i].settled > 0)
         out[i].regret_rate = (double)rows[i].regret / (double)rows[i].settled;
   }
   return n;
}

/* One detector's settled regret over the default window. Returns 0 and leaves
 * the out-params at zero when the detector has no settled commits, so callers
 * cannot mistake "not measured" for "never wrong". */
static int detector_regret(const char *signal_type, double *rate, int64_t *settled)
{
   if (rate)
      *rate = 0.0;
   if (settled)
      *settled = 0;
   if (!signal_type || !signal_type[0])
      return -1;

   learning_detector_regret_t rows[LEARNING_REGRET_MAX_DETECTORS];
   int n = learning_metrics_regret(LEARNING_METRICS_DEFAULT_WINDOW_DAYS, rows,
                                   LEARNING_REGRET_MAX_DETECTORS);
   if (n < 0)
      return -1;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(rows[i].signal_type, signal_type) != 0)
         continue;
      if (rate)
         *rate = rows[i].regret_rate;
      if (settled)
         *settled = rows[i].settled;
      return 0;
   }
   return 0; /* known query, no rows for this detector */
}

int learning_detector_corroboration_required(const char *signal_type)
{
   double rate = 0.0;
   int64_t settled = 0;
   if (detector_regret(signal_type, &rate, &settled) != 0)
      return LEARNING_CORROBORATION_DEFAULT; /* unmeasurable: do not move the bar */
   if (settled < LEARNING_REGRET_MIN_SAMPLE)
      return LEARNING_CORROBORATION_DEFAULT;
   if (rate >= LEARNING_REGRET_RAISE_BAR_RATE)
      return LEARNING_CORROBORATION_RAISED;
   return LEARNING_CORROBORATION_DEFAULT;
}

int learning_detector_may_fast_commit(const char *signal_type)
{
   double rate = 0.0;
   int64_t settled = 0;
   if (detector_regret(signal_type, &rate, &settled) != 0)
      return 1; /* unmeasurable: behave as before this slice existed */
   if (settled < LEARNING_REGRET_MIN_SAMPLE)
      return 1;
   return rate < LEARNING_REGRET_NO_FAST_RATE;
}
