// Package memory implements the memory process wire contract.
package memory

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventExtractIndex uint32 = 5889
	EventWrite        uint32 = 5890
	EventEmbed        uint32 = 5891
	EventRetrieve     uint32 = 5892
	EventRerank       uint32 = 5893

	StageExtractIndex uint32 = 1
	StageWrite        uint32 = 2
	StageEmbed        uint32 = 3
	StageRetrieve     uint32 = 4
	StageRerank       uint32 = 5

	requestMagic  uint32 = 0x4b4e524d
	responseMagic uint32 = 0x464e434d
	wireVersion   uint32 = 1
	requestLen           = 16
	responseLen          = 8
)

const (
	ConfidenceLow uint32 = iota + 1
	ConfidenceMedium
	ConfidenceHigh
)

// Handle classifies a fixed-point reranking score into a confidence band.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageRerank || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}

	score := int64(binary.LittleEndian.Uint64(request[8:16]))
	confidence := uint32(ConfidenceLow)
	if score >= 660000 {
		confidence = ConfidenceHigh
	} else if score >= 330000 {
		confidence = ConfidenceMedium
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], confidence)
	return response, bus.ModuleStatusOK
}
