package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

// Asking the module what a delegate needs from the workspace.
//
// This is the first of the two calls that run a delegate: the caller asks what
// to request, puts that to the workspace module, and comes back with the answer
// to have the container specified. Splitting it here is not ceremony -- the
// worktree must exist before there is a path to mount, so the decision and the
// creation cannot be one round trip.
//
// A refusal is a refusal. When the delegate id cannot name a branch this
// returns invalid-request rather than a plan with the name left out, because a
// write delegate with no worktree of its own would silently edit its parent's.

const (
	StageWorktreePlan uint32 = 11
	EventWorktreePlan uint32 = 6667

	worktreePlanRequestMagic  uint32 = 0x51545744 /* "DWTQ" */
	worktreePlanResponseMagic uint32 = 0x53545744 /* "DWTS" */
	worktreePlanReqHeaderLen         = 16
	// isolated, read-only, then the work name. The name is bounded, so a fixed
	// field is enough and the response has no length arithmetic to get wrong.
	// The field is workNameMax plus room for the terminator: sized exactly, a
	// full-length name would be truncated into a DIFFERENT branch name than the
	// one decided above, and silently.
	worktreePlanWorkNameLen   = workNameMax + 4
	worktreePlanResponseLen   = 4 + 4 + 4 + worktreePlanWorkNameLen
	worktreePlanDelegateIDMax = 128
)

// handleWorktreePlan answers what to ask the workspace module for.
//
// Role and delegate id arrive as content. The module looks neither up: which
// delegate is running is the caller's fact, and a module that remembered it
// would be tracking someone else's state.
func handleWorktreePlan(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) < worktreePlanReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != worktreePlanRequestMagic ||
		request[4] != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	c := &economicsCursor{buf: request, at: 8}
	roleLen, idLen := c.u32(), c.u32()
	if roleLen > roleMax || idLen > worktreePlanDelegateIDMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	role := c.str(roleLen)
	delegateID := c.str(idLen)
	if c.bad || c.at != len(request) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	plan, err := PlanWorktree(role, delegateID)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}

	response := make([]byte, worktreePlanResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], worktreePlanResponseMagic)
	putBool(response[4:8], plan.Isolated)
	putBool(response[8:12], plan.ReadOnlyMount)
	putFixed(response[12:], plan.WorkName)
	return response, bus.ModuleStatusOK
}
