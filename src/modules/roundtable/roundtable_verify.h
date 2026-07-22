/* roundtable_verify.h: replay-verification pass over captured roundtable review
 * items (Part A). Pure-code: each item's structured evidence is replayed against
 * the read-only code index and the severity is re-derived from a fixed rubric —
 * the panel proposes, the code decides. There is no second model call (the verdict
 * is mechanical, so a "verifier model" would only add a jailbreak surface).
 *
 * Items whose factual evidence contradicts the index are MOVED to out->rejected[]
 * (never silently dropped); interpretive / unreproduced items are capped at
 * "suggestion" (can never be "blocking"); an unavailable index DEGRADES (keeps the
 * item unverified) rather than rejecting.
 */
#ifndef DEC_ROUNDTABLE_VERIFY_H
#define DEC_ROUNDTABLE_VERIFY_H 1

#include "evidence_replay.h" /* pulls aimee.h (MAX_PATH_LEN) + index types first */
#include "roundtable_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      VERIFY_KEEP = 0, /* reproduced factual evidence: keep at claimed severity */
      VERIFY_CAP,      /* interpretive/unreproduced: keep, capped at "suggestion" */
      VERIFY_DEGRADE,  /* index unavailable: keep claimed severity, mark unverified */
      VERIFY_REJECT    /* claim contradicts the index (or vacuous query): move to rejected[] */
   } verify_action_t;

   /* Pure rubric: decide the action + the final severity for an item from its
    * replay status, its factual flag, and the panelist's claimed severity.
    * out_sev (cap 16) receives the final severity for KEEP/CAP/DEGRADE; a NULL or
    * empty claimed_sev defaults to "suggestion". The only path to "blocking" is a
    * reproduced factual trigger; interpretation never escalates. */
   verify_action_t roundtable_grade_item(replay_status_t st, int factual, const char *claimed_sev,
                                         char *out_sev, size_t out_cap);

   /* Replay + grade every item in out->items, compacting kept items and filling
    * out->rejected[] / the counts. Uses the real code index. `require_evidence` is the
    * evidence gate: when non-zero, an item whose replay yields no structured evidence
    * (an unfalsifiable opinion) is REJECTED, not kept-and-capped. */
   void roundtable_verify_items(roundtable_result_t *out, int require_evidence);

   /* As above but against an explicit replay backend (for tests, no DB). */
   void roundtable_verify_items_with(roundtable_result_t *out, const replay_backend_t *be,
                                     int require_evidence);

   /* Markdown appendix listing rejected items + the verified/degraded/capped
    * counts. Returns a malloc'd string (caller frees), or NULL if nothing to add. */
   char *roundtable_render_rejected(const roundtable_result_t *out);

   /* Append roundtable_render_rejected() to *artifact in place (realloc); no-op if
    * there is nothing to append or *artifact is NULL. */
   void roundtable_artifact_append_rejected(char **artifact, const roundtable_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROUNDTABLE_VERIFY_H */
