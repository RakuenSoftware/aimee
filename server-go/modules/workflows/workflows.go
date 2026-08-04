// Package workflows implements the workflows process's bounded advance decision.
package workflows

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventAdvance uint32 = 9217
	StageAdvance uint32 = 1

	requestMagic  uint32 = 0x51414657
	responseMagic uint32 = 0x52414657
	wireVersion   uint32 = 1

	workItemMax  = 79
	workItemSlot = 80
	stageMax     = 63
	stageSlot    = 64
	stateMax     = 31
	stateSlot    = 32
	nonceMax     = 63
	nonceSlot    = 64

	requestBoundOff       = 44
	requestWorkItemOff    = 124
	requestObservedOff    = 204
	requestActualStageOff = 268
	requestActualStateOff = 332
	requestNonceOff       = 364
	requestLastNonceOff   = 428
	requestLen            = 492
	responseLen           = 16
)

// AdvanceOutcome is the workflow driver's decision for one requested transition.
type AdvanceOutcome uint32

const (
	AdvanceOK AdvanceOutcome = iota
	AdvanceReplay
	AdvanceStale
	AdvanceUnbound
	AdvanceTerminal
	AdvanceBadArgs
)

type advanceRequest struct {
	boundWorkItem string
	workItem      string
	observedStage string
	actualStage   string
	actualState   string
	nonce         string
	lastNonce     string
	haveNonce     bool
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

func decodeSlot(request []byte, offset, slotSize, length int) (string, bool) {
	if length >= slotSize {
		return "", false
	}
	slot := request[offset : offset+slotSize]
	if !nonzeroText(slot[:length]) || !zeroPadding(slot[length:]) {
		return "", false
	}
	return string(slot[:length]), true
}

func decodeRequest(request []byte) (advanceRequest, bool) {
	if len(request) != requestLen || binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[8:12]) > 1 ||
		binary.LittleEndian.Uint32(request[40:44]) != 0 {
		return advanceRequest{}, false
	}
	lengths := [7]int{}
	maxima := [...]int{workItemMax, workItemMax, stageMax, stageMax, stateMax, nonceMax, nonceMax}
	for index := range lengths {
		lengths[index] = int(binary.LittleEndian.Uint32(request[12+index*4 : 16+index*4]))
		if lengths[index] > maxima[index] {
			return advanceRequest{}, false
		}
	}
	offsets := [...]int{requestBoundOff, requestWorkItemOff, requestObservedOff,
		requestActualStageOff, requestActualStateOff, requestNonceOff, requestLastNonceOff}
	slots := [...]int{workItemSlot, workItemSlot, stageSlot, stageSlot, stateSlot, nonceSlot, nonceSlot}
	values := [7]string{}
	for index := range values {
		var valid bool
		values[index], valid = decodeSlot(request, offsets[index], slots[index], lengths[index])
		if !valid {
			return advanceRequest{}, false
		}
	}
	haveNonce := binary.LittleEndian.Uint32(request[8:12]) == 1
	if haveNonce != (values[5] != "") {
		return advanceRequest{}, false
	}
	return advanceRequest{
		boundWorkItem: values[0],
		workItem:      values[1],
		observedStage: values[2],
		actualStage:   values[3],
		actualState:   values[4],
		nonce:         values[5],
		lastNonce:     values[6],
		haveNonce:     haveNonce,
	}, true
}

func decide(request advanceRequest) AdvanceOutcome {
	if request.workItem == "" || request.observedStage == "" {
		return AdvanceBadArgs
	}
	if request.boundWorkItem == "" || request.boundWorkItem != request.workItem {
		return AdvanceUnbound
	}
	if request.haveNonce && request.lastNonce != "" && request.nonce == request.lastNonce {
		return AdvanceReplay
	}
	switch request.actualState {
	case "accepted", "rejected", "abandoned":
		return AdvanceTerminal
	}
	if request.actualStage == "" || request.observedStage != request.actualStage {
		return AdvanceStale
	}
	return AdvanceOK
}

// Handle classifies an already-parsed advance request without performing the transition.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	decoded, valid := decodeRequest(request)
	if invocation.StageID != StageAdvance || !valid {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	binary.LittleEndian.PutUint32(response[8:12], uint32(decide(decoded)))
	return response, bus.ModuleStatusOK
}
