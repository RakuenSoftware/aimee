/* wfe_panel_verdict.h -- map a panel reviewer's reply to a roundtable verdict.
 * Pure (cJSON + wfe_verdict): the live panel provider dispatches one review delegate
 * per required persona, then converts each reply here. Kept separate from the dispatch
 * machinery so the (risk-bearing) mapping is unit-testable without live agents. */
#ifndef DEC_WFE_PANEL_VERDICT_H
#define DEC_WFE_PANEL_VERDICT_H 1

#include "wfe_verdict.h"

/* Parse a panel reviewer's reply into `out`. The reviewer is told to end with EXACTLY
 * one JSON line: {"verdict":"approve"|"request_changes"|"comment","high_sev_blockers":N}.
 * Only the LAST non-empty line is parsed (so quoted JSON in the reasoning can't
 * false-approve). Fills persona, reviewed_content_hash (= the artifact under review,
 * so the gate's integrity check passes), schema, kind, and the high-severity blocker
 * count. FAIL CLOSED: an empty/unparseable/unknown verdict -> WFE_V_MALFORMED (which
 * wfe_gate_decide coerces to REQUEST_CHANGES). Never approves by default. */
void wfe_panel_verdict_from_review(const char *persona, const char *artifact_hash,
                                   const char *review_text, wfe_verdict_t *out);

#endif /* DEC_WFE_PANEL_VERDICT_H */
