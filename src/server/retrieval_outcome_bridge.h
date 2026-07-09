/* retrieval_outcome_bridge.h: turn the dogfood continuation/repair autolabel into
 * retrieval outcomes, closing the demotion + learning-to-rank loops.
 *
 * Flow (mirrors the dogfood g_last_record_id single-turn lifecycle):
 *   turn N   — a retrieval surfaces rows; the emitter calls _note() with the
 *              minted retrieval_event_id + surfaced ids (per surface).
 *   turn N+1 — the user's next message is classified continuation (the answer
 *              stuck) or repair (it was corrected); _on_autolabel() consumes the
 *              pending note and records an outcome verdict for turn N's rows.
 *
 * Default-off behind config learning_implicit_retrieval_outcome. Observation-only:
 * writes evidence artifacts, never changes the turn's answer.
 * See docs/proposals/pending/kb-hybrid-outcome-wiring.md. */
#ifndef DEC_RETRIEVAL_OUTCOME_BRIDGE_H
#define DEC_RETRIEVAL_OUTCOME_BRIDGE_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Record the rows surfaced this turn for later attribution. surface is
    * "memory" (-> retrieval_attribution) or "ranker" (-> ranker_outcome). Replaces
    * any prior un-consumed note for that surface. n<=0 clears it. */
   void retrieval_outcome_bridge_note(const char *surface, const char *event_id, const int64_t *ids,
                                      int n);

   /* Consume the pending notes and, when the next turn is a clear continuation
    * (accepted) or repair (corrected), write the outcome verdict. Always clears
    * the pending state (consume-once), so an unlabelled turn drops the signal
    * rather than mis-attributing it to a later turn. */
   void retrieval_outcome_bridge_on_autolabel(int is_continuation, int is_repair);

   /* Test seam: clear pending state. */
   void retrieval_outcome_bridge_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_RETRIEVAL_OUTCOME_BRIDGE_H */
