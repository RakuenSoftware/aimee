/* retrieval_outcome_bridge.h: turn the dogfood continuation/repair autolabel into
 * retrieval outcomes, closing the demotion + learning-to-rank loops.
 *
 * Flow (mirrors the dogfood g_last_record_id single-turn lifecycle):
 *   turn N   — a retrieval surfaces rows; the emitter calls _note() with the
 *              minted retrieval_event_id + surfaced ids (and, optionally, each
 *              row's snippet, per surface).
 *   turn N+1 — the user's next message is classified continuation (the answer
 *              stuck) or repair (it was corrected); _on_autolabel() consumes the
 *              pending note and records an outcome verdict for turn N's rows. When
 *              the prior turn's answer text and per-row snippets are available it
 *              attributes PER-DOCUMENT — rows whose content the answer actually
 *              used are `accepted`, surfaced-but-unused rows are the negatives —
 *              giving the within-query contrast a ranker needs. Without them it
 *              falls back to the flat turn-level verdict.
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
    * "memory" (-> retrieval_attribution) or "ranker" (-> ranker_outcome).
    * snippets (may be NULL, or individual entries NULL) carries each row's
    * content for per-document overlap attribution. Replaces any prior un-consumed
    * note for that surface. n<=0 clears it. */
   void retrieval_outcome_bridge_note(const char *surface, const char *event_id, const int64_t *ids,
                                      const char *const *snippets, int n);

   /* Consume the pending notes for a next turn classified continuation (the answer
    * stuck) or repair (it was corrected). prior_answer is the previous assistant
    * turn's text (may be NULL). With prior_answer + snippets, attributes per-doc
    * via content overlap; otherwise writes the flat turn verdict. Always clears
    * the pending state (consume-once) so a note never leaks onto a later turn. */
   void retrieval_outcome_bridge_on_autolabel(const char *prior_answer, int is_continuation,
                                              int is_repair);

   /* Pure overlap test: does `answer` appear to have used `snippet`? A snippet is
    * "used" when a sufficient fraction of its distinctive content tokens occur in
    * the answer. Exposed for unit testing. Returns 1 (used) / 0. */
   int retrieval_outcome_overlap_used(const char *answer, const char *snippet);

   /* Test seam: clear pending state. */
   void retrieval_outcome_bridge_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DEC_RETRIEVAL_OUTCOME_BRIDGE_H */
