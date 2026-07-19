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

   /* Quorum counts NON-BLOCKING verdicts: approve and comment (a comment is by
    * definition "non-blocking remarks only" — the reviewer found nothing that
    * blocks the change). Requiring literal approve from every lens made a
    * required-4/quorum-4 gate practically unpassable for an autonomous run
    * (cautious reviewers emit comment, the gate loops to its attempt cap, and
    * every run parks pending_human). Fail-closed is preserved where it
    * matters: ANY effective request_changes loops (malformed/tampered verdicts
    * coerce to request_changes), any high-sev blocker loops, and at least one
    * lens must still explicitly approve — comments alone never pass a gate. */
   int approve = 0, high_sev = 0, changes = 0, nonblocking = 0;
   for (int i = 0; i < n; i++)
   {
      /* only trust blocker counts from structurally-valid verdicts; an
       * untrustworthy verdict already fails the gate via effective_kind. */
      if (structurally_valid(&verdicts[i], artifact_hash) && verdicts[i].high_sev_blockers > 0)
         high_sev += verdicts[i].high_sev_blockers;
      switch (effective_kind(&verdicts[i], artifact_hash))
      {
      case WFE_V_APPROVE:
         approve++;
         nonblocking++;
         break;
      case WFE_V_COMMENT:
         nonblocking++;
         break;
      default:
         changes++;
         break;
      }
   }

   if (high_sev > 0)
   {
      if (reason)
         snprintf(reason, rcap, "%d unresolved high-severity blocker(s)", high_sev);
      return WFE_GATE_CHANGES;
   }
   if (changes > 0)
   {
      if (reason)
         snprintf(reason, rcap, "%d reviewer(s) requested changes", changes);
      return WFE_GATE_CHANGES;
   }
   if (approve < 1 || nonblocking < quorum)
   {
      if (reason)
         snprintf(reason, rcap, "approve quorum not met (approve=%d, non-blocking=%d/%d)", approve,
                  nonblocking, quorum);
      return WFE_GATE_CHANGES;
   }
   if (reason)
      snprintf(reason, rcap, "approved (%d approve + %d comment >= %d, no blockers)", approve,
               nonblocking - approve, quorum);
   return WFE_GATE_APPROVE;
}
