package delegates

import (
	"encoding/binary"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
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
// Whether this delegate writes, and which delegate it is, arrive as content.
// The module looks neither up: both are the caller's facts, and a module that
// remembered them would be tracking someone else's state.
func handleWorktreePlan(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	if len(request) < worktreePlanReqHeaderLen ||
		binary.LittleEndian.Uint32(request[0:4]) != worktreePlanRequestMagic ||
		request[4] != wireVersion {
		return nil, ring.ModuleStatusInvalidRequest
	}
	// The caller's COMPOSED answer, not the role. A write role whose prompt does
	// not ask for writes needs no worktree of its own.
	if request[5] > 1 {
		return nil, ring.ModuleStatusInvalidRequest
	}
	writesAllowed := request[5] == 1

	// The length lives in the fixed header; the body starts after it.
	idLen := int(binary.LittleEndian.Uint32(request[8:12]))
	if idLen > worktreePlanDelegateIDMax {
		return nil, ring.ModuleStatusInvalidRequest
	}
	c := &economicsCursor{buf: request, at: worktreePlanReqHeaderLen}
	delegateID := c.str(idLen)
	if c.bad || c.at != len(request) {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}

	plan, err := PlanWorktree(writesAllowed, delegateID)
	if err != nil {
		return nil, ring.ModuleStatusInvalidRequest
	}

	response := make([]byte, worktreePlanResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], worktreePlanResponseMagic)
	putBool(response[4:8], plan.Isolated)
	putBool(response[8:12], plan.ReadOnlyMount)
	putFixed(response[12:], plan.WorkName)
	return response, ring.ModuleStatusOK
}
