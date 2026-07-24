/* wfe_panel_roundtable.h: map a VERIFIED roundtable review result onto the wfe
 * gate's per-lens verdicts.
 *
 * The wfe rt_gate convenes its panel through the roundtable engine
 * (delegate_roundtable_run, REVIEW mode): panelists emit structured review
 * items with replayable evidence, the engine dedups them, and the gate replays
 * the evidence against the WORKTREE (wfe_replay_worktree) via
 * roundtable_verify_items_with — so interpretation never blocks and a
 * contradicted claim is rejected, exactly as in the compute roundtable. This
 * mapper is the last step: it turns the surviving items back into the
 * wfe_verdict_t contract that wfe_gate_decide scores (required-lens coverage,
 * artifact-hash integrity, quorum). Pure aside from the optional location
 * grounding (stat/fopen against the worktree), so it is unit-testable. */
#ifndef DEC_WFE_PANEL_ROUNDTABLE_H
#define DEC_WFE_PANEL_ROUNDTABLE_H 1

#include "aimee.h" /* MAX_PATH_LEN etc., needed before the agent/config types */

#include "delegate_ensemble.h"
#include "wfe_verdict.h"

/* Fill out[0..nlens-1] — one verdict per lens, seat_agent[i] having served
 * lens[i] — from the verified result `rt`:
 *   - a lens whose agent sourced at least one surviving BLOCKING item gets
 *     REQUEST_CHANGES with high_sev_blockers = that count; otherwise APPROVE
 *     (the panelist reviewed and reported nothing that blocks);
 *   - a surviving blocking item whose location parses as "file:line" must ALSO
 *     ground in the worktree (file exists, line within it) or it is demoted to
 *     a suggestion — the belt-and-braces file:line check (workdir NULL/""
 *     skips the check rather than demoting);
 *   - every item attributed to the lens is rendered into the verdict's
 *     feedback (threaded to the re-authoring delegate on a loop);
 *   - a blocking item attributable to NO seat fails closed onto lens 0;
 *   - artifact_hash is stamped on every verdict for the gate integrity check.
 * Returns nlens, or -1 on NULL args. */
int wfe_panel_verdicts_from_roundtable(const roundtable_result_t *rt, const char *const *lens,
                                       const char *const *seat_agent, int nlens,
                                       const char *artifact_hash, const char *workdir,
                                       wfe_verdict_t *out);

#endif /* DEC_WFE_PANEL_ROUNDTABLE_H */
