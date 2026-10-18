// Package governance implements the governance process wire contract.
package governance

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/ir"
)

const (
	EventEvaluate uint32 = 8961
	StageEvaluate uint32 = 1

	responseMagic         uint32 = 0x52564f47
	wireVersion           uint32 = 2
	toolCountMax                 = 16
	responseStopReasonOff        = 24
	responseLen                  = 56
)

type policyRequest struct {
	Version    uint32            `json:"version"`
	Active     *bool             `json:"policy_active"`
	Tools      []ir.ToolIdentity `json:"tools"`
	StopReason string            `json:"stop_reason"`
}

func canonicalStop(reason string) bool {
	switch reason {
	case "unknown", "end_turn", "max_tokens", "tool_use", "stop_sequence", "content_filter", "error":
		return true
	}
	return false
}

func decodeRequest(request []byte) (policyRequest, bool) {
	var decoded policyRequest
	if !ir.Decode(request, &decoded) || decoded.Version != wireVersion || decoded.Active == nil || decoded.Tools == nil || len(decoded.Tools) > toolCountMax || !canonicalStop(decoded.StopReason) {
		return policyRequest{}, false
	}
	for _, tool := range decoded.Tools {
		if tool.Name == "" {
			return policyRequest{}, false
		}
	}
	return decoded, true
}

func deniedTool(name string) bool {
	switch name {
	case "Agent", "spawn_agent", "RemoteTrigger", "Task":
		return true
	default:
		return false
	}
}

func evaluate(request policyRequest) (uint32, uint32, string) {
	count := len(request.Tools)
	keepMask := uint32(0)
	if count > 0 {
		keepMask = (1 << count) - 1
	}
	if !*request.Active || count == 0 {
		return keepMask, 0, request.StopReason
	}

	drops := uint32(0)
	keepMask = 0
	for index, tool := range request.Tools {
		if deniedTool(tool.Name) {
			drops++
		} else {
			keepMask |= 1 << index
		}
	}
	kept := count - int(drops)
	finalReason := request.StopReason
	if finalReason == "tool_use" && kept == 0 {
		finalReason = "end_turn"
	}
	return keepMask, drops, finalReason
}

// Handle evaluates which response tool calls survive the active governance policy.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	decoded, valid := decodeRequest(request)
	if invocation.StageID != StageEvaluate || !valid {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	keepMask, drops, finalReason := evaluate(decoded)
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	binary.LittleEndian.PutUint32(response[8:12], keepMask)
	binary.LittleEndian.PutUint32(response[12:16], drops)
	binary.LittleEndian.PutUint32(response[16:20], uint32(len(finalReason)))
	copy(response[responseStopReasonOff:], finalReason)
	return response, bus.ModuleStatusOK
}
