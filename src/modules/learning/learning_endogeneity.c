/* learning_endogeneity.c: provenance accounting for committed learning
 * proposals, and the gate that refuses promotion when the loop is feeding on
 * its own output.
 *
 * The classification is deliberately coarse and inspectable: it reads the
 * originating signal's (source, signal_type) rather than attempting to walk
 * evidence_refs, because refs today carry memory row ids rather than typed
 * artifact roots. When refs become typed, this classifier gains a third input
 * without its callers changing.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <aimee/learning/learning.h>

#include "modules/db2/c/db2_learning.h"

#include <stddef.h>
#include <string.h>

/* Signal sources that root a proposal outside the system's own reasoning. */
static const char *const LEARNING_EXOGENOUS_SOURCES[] = {
    "explicit",         /* a human acting through the feedback surface */
    "operator",         /* an operator accept/reject */
    "human",    "test", /* a test's exit status */
    "verify",           /* the mechanical verify gate */
    "grader",           /* an official benchmark grader */
    "git",              /* an observed git outcome */
    NULL,
};

/* Signal types produced by the implicit detectors — aimee reading its own
 * transcript. These are endogenous whatever `source` claims, because the
 * capture API defaults an unset source to "explicit". */
static const char *const LEARNING_SELF_DERIVED_SIGNAL_TYPES[] = {
    "repeat_question",      "repeated_correction",
    "citation_then_repair", "citation_then_continuation",
    "workflow_repetition",  NULL,
};

static int str_in_list(const char *needle, const char *const *list)
{
   if (!needle || !needle[0])
      return 0;
   for (int i = 0; list[i]; i++)
      if (strcmp(needle, list[i]) == 0)
         return 1;
   return 0;
}

learning_evidence_origin_t learning_evidence_classify(const char *source, const char *signal_type)
{
   /* signal_type wins: a self-derived detector type cannot be laundered into
    * the exogenous count by omitting or overstating the source. */
   if (str_in_list(signal_type, LEARNING_SELF_DERIVED_SIGNAL_TYPES))
      return LEARNING_EVIDENCE_ENDOGENOUS;
   if (str_in_list(source, LEARNING_EXOGENOUS_SOURCES))
      return LEARNING_EVIDENCE_EXOGENOUS;
   /* Unknown provenance counts against us. */
   return LEARNING_EVIDENCE_ENDOGENOUS;
}

/* Groups are (source, signal_type) pairs; a window with more distinct pairs
 * than this is truncated, which can only understate one side of the ratio.
 * The vocabulary is small and closed, so the bound is generous. */
#define LEARNING_ENDOGENEITY_MAX_GROUPS 64

int learning_metrics_endogeneity_for_sink(int window_days, const char *sink,
                                          learning_endogeneity_t *out)
{
   if (!out)
      return -1;
   if (window_days <= 0)
      window_days = LEARNING_METRICS_DEFAULT_WINDOW_DAYS;

   memset(out, 0, sizeof(*out));
   out->window_days = window_days;

   db2_learning_source_count_t groups[LEARNING_ENDOGENEITY_MAX_GROUPS];
   int n = db2_learning_committed_source_counts(window_days, sink, groups,
                                                LEARNING_ENDOGENEITY_MAX_GROUPS);
   if (n < 0)
      return -1;

   for (int i = 0; i < n; i++)
   {
      if (groups[i].count <= 0)
         continue;
      if (learning_evidence_classify(groups[i].source, groups[i].signal_type) ==
          LEARNING_EVIDENCE_EXOGENOUS)
         out->committed_exogenous += groups[i].count;
      else
         out->committed_endogenous += groups[i].count;
   }
   out->committed_total = out->committed_exogenous + out->committed_endogenous;
   if (out->committed_total > 0)
      out->exogenous_ratio = (double)out->committed_exogenous / (double)out->committed_total;
   return 0;
}

int learning_metrics_endogeneity(int window_days, learning_endogeneity_t *out)
{
   return learning_metrics_endogeneity_for_sink(window_days, NULL, out);
}

learning_gate_state_t learning_gate_check_with(int window_days, double min_exogenous_ratio,
                                               int min_sample, learning_endogeneity_t *out)
{
   learning_endogeneity_t local;
   learning_endogeneity_t *m = out ? out : &local;

   if (learning_metrics_endogeneity(window_days, m) != 0)
      return LEARNING_GATE_UNAVAILABLE;
   if (min_sample < 0)
      min_sample = 0;
   /* Too few settled commits for the ratio to mean anything — including the
    * empty window a fresh installation starts from. */
   if (m->committed_total < (int64_t)min_sample)
      return LEARNING_GATE_OPEN;
   if (m->exogenous_ratio < min_exogenous_ratio)
      return LEARNING_GATE_CLOSED_ENDOGENOUS;
   return LEARNING_GATE_OPEN;
}

learning_gate_state_t learning_gate_check(learning_endogeneity_t *out)
{
   return learning_gate_check_with(LEARNING_METRICS_DEFAULT_WINDOW_DAYS,
                                   LEARNING_ENDOGENEITY_MIN_EXOGENOUS_RATIO,
                                   LEARNING_ENDOGENEITY_MIN_SAMPLE, out);
}
