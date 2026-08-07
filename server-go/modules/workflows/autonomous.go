package workflows

// S4 autonomous-parity routing policy.
//
// Moved from C (src/modules/workflows/wfe_autonomous_route.c). It decides which
// workflows an AUTONOMOUS run may auto-select, which makes it a safety control
// rather than a convenience: everything it rejects is lifted to a floor that
// carries a human gate.
//
// The rulings it encodes come from the S4 design roundtable (2026-07-03) and are
// preserved exactly:
//   - Q2: the omitted-workflow floor is `managed-change`, a FULL-SPINE enforced
//     workflow (review + gate.roundtable + terminal gate.deliver), NOT `build`.
//     `build` keeps the weaker pre-gate.deliver posture and is reachable only by
//     explicit name.
//   - Q5: the selectable set is a fixed full-spine allowlist that excludes
//     read-only AND review-bypassing lanes, so a shaped proposal can at worst
//     pick among spine-carrying lanes, never bypass review.
//   - Q3: sweep candidates are unvetted and always hit a human gate.

import (
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
)

// AutonomousFloor is the omitted-workflow floor: a full-spine enforced workflow.
const AutonomousFloor = "managed-change"

// SweepWorkflowFloor is the sweep human-gate floor. Sweep candidates are
// unvetted heuristic finds, so filing is a fixed floor rather than a content
// router, which would only add misroute risk for no safety benefit.
const SweepWorkflowFloor = "manual-review"

// readOnlyLanes are never auto-selected even if a catalog entry were mis-marked
// enforced. By the I2 loader invariant an enforced workflow must terminate in
// gate.deliver, so this cannot happen -- it is defence in depth, not the primary
// control.
var readOnlyLanes = map[string]bool{"converse": true, "research": true}

type AutonomousRouteRequest struct {
	RouterID string `json:"router_id"`
	Enforced bool   `json:"enforced"`
}

type AutonomousRouteResponse struct {
	Selectable bool   `json:"selectable"`
	Workflow   string `json:"workflow"`
	Clamped    bool   `json:"clamped"`
}

// AutonomousSelectable reports whether a router-selected workflow may be
// auto-selected for an autonomous run.
//
// The predicate is the catalog's `enforced` flag: enforced lanes are exactly the
// full-spine ones. An empty id fails closed.
func AutonomousSelectable(id string, enforced bool) bool {
	if id == "" || !enforced {
		return false
	}
	return !readOnlyLanes[id]
}

// AutonomousClamp lifts a router decision into the selectable set, reporting
// whether the floor was substituted.
func AutonomousClamp(routerID string, enforced bool) AutonomousRouteResponse {
	if AutonomousSelectable(routerID, enforced) {
		return AutonomousRouteResponse{Selectable: true, Workflow: routerID, Clamped: false}
	}
	return AutonomousRouteResponse{Selectable: false, Workflow: AutonomousFloor, Clamped: true}
}

// handleAutonomousRoute serves StageAutonomousRoute.
//
// A request that cannot be read is refused rather than answered with a default.
// Inventing "selectable" here would let an autonomous run pick a lane the policy
// never approved; the C caller turns any refusal into the floor, which is the
// safe direction.
func handleAutonomousRoute(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	var decoded AutonomousRouteRequest
	if err := json.Unmarshal(request, &decoded); err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	encoded, err := json.Marshal(AutonomousClamp(decoded.RouterID, decoded.Enforced))
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return encoded, bus.ModuleStatusOK
}
