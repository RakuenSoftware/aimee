// Package governance implements the governance process wire contract.
package governance

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventEvaluate uint32 = 8961
	StageEvaluate uint32 = 1

	requestMagic          uint32 = 0x51564f47
	responseMagic         uint32 = 0x52564f47
	wireVersion           uint32 = 1
	toolCountMax                 = 16
	toolNameMax                  = 31
	toolNameSlot                 = 32
	stopReasonMax                = 31
	requestToolLengthsOff        = 24
	requestToolNamesOff          = 88
	requestStopReasonOff         = 600
	requestLen                   = 632
	responseStopReasonOff        = 24
	responseLen                  = 56
)

type policyRequest struct {
	active     bool
	toolNames  []string
	stopReason string
}

func zeroPadding(value []byte) bool {
	for _, item := range value {
		if item != 0 {
			return false
		}
	}
	return true
}

func nonzeroText(value []byte) bool {
	for _, item := range value {
		if item == 0 {
			return false
		}
	}
	return true
}

func decodeRequest(request []byte) (policyRequest, bool) {
	if len(request) != requestLen || binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[8:12]) > 1 ||
		binary.LittleEndian.Uint32(request[12:16]) > toolCountMax ||
		binary.LittleEndian.Uint32(request[16:20]) > stopReasonMax ||
		binary.LittleEndian.Uint32(request[20:24]) != 0 {
		return policyRequest{}, false
	}
	count := int(binary.LittleEndian.Uint32(request[12:16]))
	names := make([]string, count)
	for index := range toolCountMax {
		lengthOffset := requestToolLengthsOff + index*4
		nameLen := int(binary.LittleEndian.Uint32(request[lengthOffset : lengthOffset+4]))
		slotOffset := requestToolNamesOff + index*toolNameSlot
		slot := request[slotOffset : slotOffset+toolNameSlot]
		if nameLen > toolNameMax || (index >= count && nameLen != 0) ||
			!nonzeroText(slot[:nameLen]) || !zeroPadding(slot[nameLen:]) {
			return policyRequest{}, false
		}
		if index < count {
			names[index] = string(slot[:nameLen])
		}
	}
	reasonLen := int(binary.LittleEndian.Uint32(request[16:20]))
	reasonSlot := request[requestStopReasonOff:]
	if !nonzeroText(reasonSlot[:reasonLen]) || !zeroPadding(reasonSlot[reasonLen:]) {
		return policyRequest{}, false
	}
	return policyRequest{
		active:     binary.LittleEndian.Uint32(request[8:12]) == 1,
		toolNames:  names,
		stopReason: string(reasonSlot[:reasonLen]),
	}, true
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
	count := len(request.toolNames)
	keepMask := uint32(0)
	if count > 0 {
		keepMask = (1 << count) - 1
	}
	if !request.active || count == 0 {
		return keepMask, 0, request.stopReason
	}

	drops := uint32(0)
	keepMask = 0
	for index, name := range request.toolNames {
		if deniedTool(name) {
			drops++
		} else {
			keepMask |= 1 << index
		}
	}
	kept := count - int(drops)
	finalReason := request.stopReason
	if finalReason == "" || kept == 0 {
		if kept > 0 {
			finalReason = "tool_use"
		} else {
			finalReason = "end_turn"
		}
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
