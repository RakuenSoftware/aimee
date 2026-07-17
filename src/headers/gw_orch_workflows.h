/* gw_orch_workflows.h -- the WORKFLOWS orchestration module: the second producer on the
 * gw_orchestration_seam (after gw_orch_delegates). It moves trigger-initiated workflow
 * dispatch out of trigger_scheduler.c's inline wfe_work_item_create call into a togglable,
 * registered orchestration hook driven through the same registry as delegates.
 *
 * NOTE on scope: gw_stage_router (router_advise.c) is an ADVISORY request-side stage -- it
 * observes a turn and records a routing decision; it does NOT dispatch a workflow, so it stays
 * a request stage. The genuine imperative workflow ACTION is wfe_work_item_create (start a
 * run), and trigger_scheduler's shared filing chokepoint is the honest first port; the other
 * creation sites (sweep, dev-submit intake, live-foreach) are sequenced follow-ups. */
#ifndef DEC_GW_ORCH_WORKFLOWS_H
#define DEC_GW_ORCH_WORKFLOWS_H 1

#include "gw_orchestration_seam.h"

/* Default-ON unless AIMEE_ORCH_WORKFLOWS is an explicit 0/off/false/no. Mirrors
 * gw_orch_delegates_enabled; the config-store becomes the canonical enablement surface in the
 * dedicated config-surface slice (roundtable ruling). */
int gw_orch_workflows_enabled(void);

/* Run the workflows orchestration module for one dispatch decision. Builds the single-hook
 * `workflows` catalog (gated by gw_orch_workflows_enabled), a turn snapshot tagged with
 * `turn_id`, and runs it through gw_orchestration_run over `caps`. The hook invokes
 * caps->dispatch_workflow(caps->ctx, lane, payload); the dispatch's own success/failure is
 * recorded by the caller's capability adapter in caps->ctx (this function does not surface it).
 * Returns:
 *   0   the module was enabled and the hook ran (read the dispatch outcome from your backing);
 *  -1   the module was DISABLED (no dispatch attempted) or the catalog failed to build, or
 *       `caps` was NULL. The caller decides what a skipped dispatch means. */
int gw_orch_workflows_run(const gw_turn_capabilities_t *caps, const char *turn_id, const char *lane,
                          const char *payload);

#endif /* DEC_GW_ORCH_WORKFLOWS_H */
