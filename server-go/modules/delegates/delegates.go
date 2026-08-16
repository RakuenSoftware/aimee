// Package delegates implements the delegate-invocation process wire contract.
package delegates

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	delegatecontract "github.com/JBailes/aimee/server-go/delegate"
)

const (
	EventKind      uint32 = 6657
	StageInvoke    uint32 = 1
	EventGroupPlan uint32 = delegatecontract.EventGroupPlan
	StageGroupPlan uint32 = delegatecontract.StageGroupPlan
	requestMagic   uint32 = 0x4c4f5244
	responseMagic  uint32 = 0x4e414344
	wireVersion    byte   = 1
	roleMax               = 63
	messageLen            = 72
)

var aliases = map[string]string{
	"implement": "code", "build": "code", "reviewer": "review",
	"verifier": "validate", "test": "validate", "check": "validate",
	"evaluate": "validate", "evaluate-optimize": "validate", "inspect": "diagnose",
	"research": "execute", "enforce": "execute", "recall": "search",
	"synthesize": "summarize", "rank-fuse": "reason", "classify-score": "reason",
	"planner": "plan", "planning": "plan",
}

// NewHandler adds real execution to the delegates module while preserving the
// fixed v1 role-canonicalization request used by older C callers. V2 is JSON
// because prompts and responses are variable-sized bus messages.
func NewHandler(executor Executor) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if invocation.StageID == StageGroupPlan {
			planner, ok := executor.(interface {
				PlanGroup(context.Context, []delegatecontract.GroupPlanSeat) ([]string, error)
			})
			if !ok {
				return nil, bus.ModuleStatusInternal
			}
			decoder := json.NewDecoder(bytes.NewReader(request))
			decoder.DisallowUnknownFields()
			var decoded delegatecontract.GroupPlan
			if decoder.Decode(&decoded) != nil || decoded.Version != delegatecontract.WireVersion ||
				len(decoded.Seats) == 0 || len(decoded.Seats) > 64 {
				return nil, bus.ModuleStatusInvalidRequest
			}
			var trailing any
			if err := decoder.Decode(&trailing); err != io.EOF {
				return nil, bus.ModuleStatusInvalidRequest
			}
			for i := range decoded.Seats {
				if canonical, exists := aliases[decoded.Seats[i].Role]; exists {
					decoded.Seats[i].Role = canonical
				}
				if strings.TrimSpace(decoded.Seats[i].Role) == "" ||
					strings.TrimSpace(decoded.Seats[i].Persona) == "" {
					return nil, bus.ModuleStatusInvalidRequest
				}
			}
			ctx, cancel := cancelledContext(invocation.Cancelled)
			defer cancel()
			models, err := planner.PlanGroup(ctx, decoded.Seats)
			if err != nil {
				if !delegatecontract.IsCapacityBackpressure(err) && !delegatecontract.IsCapacityDeadline(err) {
					return nil, bus.ModuleStatusInternal
				}
				response, marshalErr := json.Marshal(delegatecontract.GroupPlanResult{
					Version: delegatecontract.WireVersion,
					Error:   delegatecontract.SafeDiagnostic(err.Error()),
				})
				if marshalErr != nil {
					return nil, bus.ModuleStatusInternal
				}
				return response, bus.ModuleStatusOK
			}
			if len(models) != len(decoded.Seats) {
				return nil, bus.ModuleStatusInternal
			}
			for _, model := range models {
				if strings.TrimSpace(model) == "" {
					return nil, bus.ModuleStatusInternal
				}
			}
			response, err := json.Marshal(delegatecontract.GroupPlanResult{
				Version: delegatecontract.WireVersion, Models: models})
			if err != nil {
				return nil, bus.ModuleStatusInternal
			}
			return response, bus.ModuleStatusOK
		}
		if invocation.StageID != StageInvoke {
			return Handle(invocation, request)
		}
		// DROL v1 begins with its fixed little-endian magic (bytes "DROL"), so
		// discriminate it structurally before considering the variable JSON wire.
		if len(request) == messageLen && binary.LittleEndian.Uint32(request[:4]) == requestMagic {
			return Handle(invocation, request)
		}
		if len(bytes.TrimSpace(request)) == 0 || bytes.TrimSpace(request)[0] != '{' {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if executor == nil {
			return nil, bus.ModuleStatusInternal
		}
		decoder := json.NewDecoder(bytes.NewReader(request))
		decoder.DisallowUnknownFields()
		var decoded delegatecontract.Invocation
		if err := decoder.Decode(&decoded); err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var trailing any
		if err := decoder.Decode(&trailing); err != io.EOF ||
			decoded.Version != delegatecontract.WireVersion ||
			strings.TrimSpace(decoded.Role) == "" || strings.TrimSpace(decoded.Persona) == "" ||
			strings.TrimSpace(decoded.Prompt) == "" || decoded.MaxTurns < 0 ||
			decoded.ExecutionTimeoutMS < 0 || decoded.ExecutionTimeoutMS > int64((24*time.Hour)/time.Millisecond) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		if canonical, ok := aliases[decoded.Role]; ok {
			decoded.Role = canonical
		}
		ctx, cancel := cancelledContext(invocation.Cancelled)
		defer cancel()
		if decoded.ExecutionTimeoutMS > 0 {
			var deadlineCancel context.CancelFunc
			ctx, deadlineCancel = context.WithTimeout(ctx,
				time.Duration(decoded.ExecutionTimeoutMS)*time.Millisecond)
			defer deadlineCancel()
		}
		result := executor.Execute(ctx, decoded)
		if result.Version != delegatecontract.WireVersion ||
			(result.Status != "done" && result.Status != "failed") ||
			(result.Status == "done" && result.Termination != nil) ||
			(result.Termination != nil &&
				!delegatecontract.ValidTerminationDiagnostic(result.Termination)) {
			return nil, bus.ModuleStatusInternal
		}
		response, err := json.Marshal(result)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return response, bus.ModuleStatusOK
	}
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
	if invocation.StageID == StageReviewEvidence {
		return handleReviewEvidence(invocation, request)
	}
	if invocation.StageID == StageNamedFileDrift {
		return handleNamedFileDrift(invocation, request)
	}
	if invocation.StageID == StagePermissions {
		return handlePermissions(invocation, request)
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
