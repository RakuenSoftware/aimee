/* wfe_verdict.c: the fail-closed roundtable gate decision rule (pure). */
#include "wfe_verdict.h"

#include <stdio.h>
#include <string.h>

int wfe_gate_effective_quorum(int requested, int nreq)
{
   int q = requested;
   if (q < nreq)
      q = nreq;
   if (q < 2)
      q = 2; /* single-lens floor */
   return q;
}

/* A verdict is structurally trustworthy iff its schema is known, it is not
 * malformed, and it reviewed the artifact actually under review. */
static int structurally_valid(const wfe_verdict_t *v, const char *artifact_hash)
{
   if (v->schema_version != WFE_VERDICT_SCHEMA)
      return 0;
   if (v->kind == WFE_V_MALFORMED)
      return 0;
   if (artifact_hash && artifact_hash[0] && strcmp(v->reviewed_content_hash, artifact_hash) != 0)
      return 0;
   return 1;
}

/* Coerce a verdict to its effective kind under the integrity rules
 * (structurally-untrustworthy verdicts fail closed to REQUEST_CHANGES). */
static wfe_verdict_kind_t effective_kind(const wfe_verdict_t *v, const char *artifact_hash)
{
   return structurally_valid(v, artifact_hash) ? v->kind : WFE_V_REQUEST_CHANGES;
}

wfe_gate_decision_t wfe_gate_decide(const wfe_verdict_t *verdicts, int n,
                                    const char *const *required, int nreq, int quorum,
                                    const char *artifact_hash, char *reason, size_t rcap)
{
   if (reason && rcap)
      reason[0] = '\0';

   /* required coverage: every required persona must have returned a STRUCTURALLY
    * VALID verdict. A malformed / wrong-schema / hash-mismatched response from a
    * required persona is an integrity failure, NOT coverage: treating its mere
    * presence as satisfied would let the gate approve via other panelists' quorum
    * while a required lens never actually reviewed the artifact. Fail closed to
    * DEGRADED (panel_degraded_required) exactly as if that persona were absent. */
   for (int r = 0; r < nreq; r++)
   {
      int present = 0;
      for (int i = 0; i < n; i++)
         if (strcmp(verdicts[i].persona, required[r]) == 0 &&
             structurally_valid(&verdicts[i], artifact_hash))
         {
            present = 1;
            break;
         }
      if (!present)
      {
         if (reason)
            snprintf(reason, rcap, "required persona '%s' missing or untrustworthy verdict",
                     required[r]);
         return WFE_GATE_DEGRADED;
      }
   }

   int approve = 0, high_sev = 0;
   for (int i = 0; i < n; i++)
   {
      /* only trust blocker counts from structurally-valid verdicts; an
       * untrustworthy verdict already fails the gate via effective_kind. */
      if (structurally_valid(&verdicts[i], artifact_hash) && verdicts[i].high_sev_blockers > 0)
         high_sev += verdicts[i].high_sev_blockers;
      if (effective_kind(&verdicts[i], artifact_hash) == WFE_V_APPROVE)
         approve++;
   }

   if (high_sev > 0)
   {
      if (reason)
         snprintf(reason, rcap, "%d unresolved high-severity blocker(s)", high_sev);
      return WFE_GATE_CHANGES;
   }
   if (approve < quorum)
   {
      if (reason)
         snprintf(reason, rcap, "approve quorum not met (%d/%d)", approve, quorum);
      return WFE_GATE_CHANGES;
   }
   if (reason)
      snprintf(reason, rcap, "approved (%d>=%d, no high-sev blockers)", approve, quorum);
   return WFE_GATE_APPROVE;
}
