/* wfe_autonomous_route.h -- S4 autonomous-parity routing policy (PURE).
 *
 * S4 of primary-as-manager: route the AUTONOMOUS surfaces (/v1/dev/submit with an
 * omitted workflow, and sweep filing) through the same request->workflow router
 * as the interactive surface, instead of silently defaulting to "build" /
 * "manual-review". This is the pure decision policy; the catalog load + the
 * wfe_router_decide call + the lifecycle_event logging are integration layers in
 * the server (rh_dev_submit / server_sweep) built ON TOP of this core, kept out
 * of here so the policy is unit-testable with no I/O.
 *
 * Rulings folded from the S4 design roundtable (2026-07-03, non-degraded 6/6):
 *  - Q2: the omitted-workflow floor is `managed-change` (a FULL-SPINE enforced
 *    workflow: review + gate.roundtable + terminal gate.deliver), NOT `build`.
 *    Autonomous submissions get the same manager spine as interactive ones;
 *    `build` preserves the weaker pre-gate.deliver posture and is reachable only
 *    by explicit name.
 *  - Q5 (the fourth invariant): the autonomous router's selectable set is a
 *    fixed FULL-SPINE allowlist that excludes read-only AND review-bypassing
 *    lanes -- a shaped proposal can at worst pick among spine-carrying lanes,
 *    never bypass review.
 *  - Q3: sweep candidates are UNVETTED; they always hit a human gate
 *    (`manual-review`), never an auto-executing lane.
 */
#ifndef DEC_WFE_AUTONOMOUS_ROUTE_H
#define DEC_WFE_AUTONOMOUS_ROUTE_H 1

/* The autonomous omitted-workflow floor: a full-spine enforced workflow. */
#define WFE_AUTONOMOUS_FLOOR "managed-change"

/* 1 if a router-selected workflow may be AUTO-selected for an autonomous run.
 * The predicate is the catalog `enforced` flag: by the I2 loader invariant an
 * enforced workflow MUST terminate in gate.deliver, so `enforced` lanes are
 * exactly the full-spine (review + roundtable + deliver) lanes -- read-only
 * (converse/research) and pre-gate.deliver (build) lanes are enforced:false and
 * are never auto-selected. A defensive name check additionally rejects the
 * (I2-impossible) enforced-but-known-read-only case. `id` may be NULL. */
int wfe_autonomous_selectable(const char *id, int enforced);

/* Clamp a router decision to the autonomous selectable set. Returns `router_id`
 * when it is selectable, else WFE_AUTONOMOUS_FLOOR. *out_clamped (nullable) is
 * set to 1 when the floor was substituted, 0 otherwise. */
const char *wfe_autonomous_clamp(const char *router_id, int enforced, int *out_clamped);

/* The sweep human-gate floor ("manual-review"). Sweep candidates are UNVETTED
 * heuristic finds; they must ALWAYS hit a human gate, never an auto-executing
 * lane -- so sweep filing is a fixed floor, not a content router (which would
 * only add misroute risk for zero safety benefit). */
const char *wfe_sweep_workflow_floor(void);

#endif /* DEC_WFE_AUTONOMOUS_ROUTE_H */
