// Package delegates implements the delegate-invocation process wire contract.
package delegates

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind     uint32 = 6657
	StageInvoke   uint32 = 1
	requestMagic  uint32 = 0x4c4f5244
	responseMagic uint32 = 0x4e414344
	wireVersion   byte   = 1
	roleMax              = 63
	messageLen           = 72
)

var aliases = map[string]string{
	"implement": "code", "build": "code", "reviewer": "review",
	"verifier": "validate", "test": "validate", "check": "validate",
	"evaluate": "validate", "evaluate-optimize": "validate", "inspect": "diagnose",
	"research": "execute", "enforce": "execute", "recall": "search",
	"synthesize": "summarize", "rank-fuse": "reason", "classify-score": "reason",
	"planner": "plan", "planning": "plan",
}

// Handle canonicalizes a delegate role, or infers what a prompt needs of a
// model, without invoking a delegate.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID == StageCapabilities {
		return handleCapabilities(invocation, request)
	}
	if invocation.StageID == StageChain {
		return handleChain(invocation, request)
	}
	if invocation.StageID == StagePaths {
		return handlePaths(invocation, request)
	}
	if invocation.StageID == StageHandoff {
		return handleHandoff(invocation, request)
	}
	if invocation.StageID == StageRescue {
		return handleRescue(invocation, request)
	}
	if invocation.StageID == StageVerify {
		return handleVerify(invocation, request)
	}
	if invocation.StageID == StageEconomics {
		return handleEconomics(invocation, request)
	}
	if invocation.StageID == StagePatchCoord {
		return handlePatchCoord(invocation, request)
	}
	if invocation.StageID == StageRolePolicy {
		return handleRolePolicy(invocation, request)
	}
	if invocation.StageID == StageWorktreePlan {
		return handleWorktreePlan(invocation, request)
	}
	if invocation.StageID == StageLaunchArgs {
		return handleLaunchArgs(invocation, request)
	}
	if invocation.StageID == StageImageSpec {
		return handleImageSpec(invocation, request)
	}
	if invocation.StageID == StageIsolation {
		return handleIsolation(invocation, request)
	}
	if invocation.StageID == StageMayWrite {
		return handleMayWrite(invocation, request)
	}
	if invocation.StageID == StageImageGC {
		return handleImageGC(invocation, request)
	}
	if invocation.StageID == StageRouteFilter {
		return handleRouteFilter(invocation, request)
	}
	if invocation.StageID == StageNoopWrite {
		return handleNoopWrite(invocation, request)
	}
	if invocation.StageID == StageLaunchPlan {
		return handleLaunchPlan(invocation, request)
	}
	if invocation.StageID != StageInvoke || len(request) != messageLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic || request[4] != wireVersion ||
		request[5] != 0 || request[7] != 0 || request[6] == 0 || request[6] > roleMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	role := string(request[8 : 8+int(request[6])])
	if canonical, ok := aliases[role]; ok {
		role = canonical
	}
	response := make([]byte, messageLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	response[4] = wireVersion
	response[6] = byte(len(role))
	copy(response[8:], role)
	return response, bus.ModuleStatusOK
}
