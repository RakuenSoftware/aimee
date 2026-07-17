/* gw_orch_delegates.h -- the DELEGATES orchestration module: the first real consumer of the
 * gw_orchestration_seam. It moves on-demand delegate spawning OUT of the coord dispatcher's
 * inline imperative call into a togglable, registered orchestration hook. Disabling the module
 * stops delegate spawns without touching the spawn primitive (delegate_spawn_ondemand).
 *
 * Follow-up port of Slice 3 (docs/proposals/pending/orchestration-seam-delegate-firstport.md):
 * the coord dispatcher is the genuine imperative spawn site (the /v1 turn path only PREVENTS
 * subagents; it never spawns), so it is the honest first port. */
#ifndef DEC_GW_ORCH_DELEGATES_H
#define DEC_GW_ORCH_DELEGATES_H 1

#include "gw_orchestration_seam.h"

/* Default-ON unless AIMEE_ORCH_DELEGATES is an explicit 0/off/false/no. Mirrors
 * gw_response_governance_enabled / gw_stage_memory_enabled; the config-store becomes the
 * canonical enablement surface in the dedicated config-surface slice (roundtable ruling). */
int gw_orch_delegates_enabled(void);

/* Run the delegates orchestration module for one spawn decision. Builds the single-hook
 * `delegates` catalog (gated by gw_orch_delegates_enabled), a turn snapshot tagged with
 * `turn_id`, and runs it through gw_orchestration_run over `caps`. The hook invokes
 * caps->spawn_delegate(caps->ctx, role, brief); the spawn's own success/failure is recorded by
 * the caller's capability adapter in caps->ctx (this function does not surface it).
 * Returns:
 *   0   the module was enabled and the hook ran (read the spawn outcome from your backing);
 *  -1   the module was DISABLED (no spawn attempted) or the catalog failed to build, or `caps`
 *       was NULL. The runner is fail-open, so a hook failure never aborts here; the caller
 *       decides what a missed spawn means (the coord dispatcher releases the task claim and
 *       retries on the next sweep). */
int gw_orch_delegates_run(const gw_turn_capabilities_t *caps, const char *turn_id, const char *role,
                          const char *brief);

#endif /* DEC_GW_ORCH_DELEGATES_H */
