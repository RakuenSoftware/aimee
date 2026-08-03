// Package learning implements the learning-observation process wire contract.
package learning

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind     uint32 = 6145
	StageObserve  uint32 = 1
	requestMagic  uint32 = 0x53424f4c
	responseMagic uint32 = 0x4b53414c
	wireVersion   byte   = 1
	requestLen           = 40
	responseLen          = 8
	signalMax            = 31

	SinkReranker  uint32 = 0x01
	SinkSupersede uint32 = 0x02
	SinkRule      uint32 = 0x04
	SinkWorkflow  uint32 = 0x08
)

// Handle classifies one learning signal into its downstream sink mask.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageObserve || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic || request[4] != wireVersion ||
		request[5] != 0 || request[7] != 0 || request[6] == 0 || request[6] > signalMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	var mask uint32
	switch string(request[8 : 8+int(request[6])]) {
	case "thumb_up", "thumb_down":
		mask = SinkReranker
	case "correction":
		mask = SinkReranker | SinkSupersede | SinkRule
	case "preference_statement", "mark_rule":
		mask = SinkRule
	case "workflow_repetition":
		mask = SinkWorkflow
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], mask)
	return response, bus.ModuleStatusOK
}
