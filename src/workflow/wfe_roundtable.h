/* wfe_roundtable.h: the gate.roundtable executor + the panel-provider seam.
 * The live provider calls the ensemble engine and is gated on
 * roundtable-panel-composition (§0); tests inject a mock provider. */
#ifndef DEC_WFE_ROUNDTABLE_H
#define DEC_WFE_ROUNDTABLE_H 1

#include "wfe_verdict.h"

/* The composite context a panel reviews. The panel sees the artifact under review
 * AND the originating proposal/request, so it can validate the work AGAINST what
 * was asked (e.g. "does this plan satisfy the proposal?", "was the proposal
 * completed?") rather than inspecting it in isolation. `focus` is an optional
 * review lens carried from the gate node's `focus` param (e.g. "completion, code
 * quality, missing tests") so one block composes as both a plan gate and an
 * acceptance gate. Fields are borrowed (valid only for the duration of the call)
 * and never NULL — an absent value is the empty string. */
typedef struct
{
   const char *artifact_hash; /* work item's current content (plan/diff under review) */
   const char *proposal;      /* originating proposal/request text, "" if none */
   const char *focus;         /* review lens from params.focus, "" if unset */
   const char *workdir; /* the run's worktree (a live panel inspects the diff here), "" if none */
   const char *diff;    /* the change under review vs the base repo, pushed to the panel so it
                         * reads the delta directly (trusting the index for context) instead of
                         * re-running git; "" at a plan gate (no code change yet) */
} wfe_review_packet_t;

/* Panel-provider return sentinels (negative), distinct from a verdict count (>=0). */
#define WFE_PANEL_UNREACHABLE (-1) /* panel could not be reached/composed -> gate parks DEGRADED */
#define WFE_PANEL_PINNED_FAIL                                                                       \
   (-2) /* a PINNED seat model could not be fulfilled -> the run FAILS (no substitution) */

/* A panel provider runs the configured panel against the review packet and fills
 * up to `max` verdicts. Returns the number of verdicts (>=0), or a WFE_PANEL_*
 * sentinel: WFE_PANEL_UNREACHABLE (-1) -> park DEGRADED; WFE_PANEL_PINNED_FAIL
 * (-2) -> fail the run (a user-pinned seat model was unavailable). */
typedef int (*wfe_panel_run_fn)(const wfe_review_packet_t *pkt, const char *const *required,
                                int nreq, const char *const *eligible, int nelig,
                                wfe_verdict_t *out, int max);

/* Install a panel provider (tests inject a mock; NULL restores the default). */
void wfe_set_panel_provider(wfe_panel_run_fn fn);

/* Register the gate.roundtable executor behind the wfe_iface vtable. */
void wfe_register_roundtable_gate(void);

#endif /* DEC_WFE_ROUNDTABLE_H */
