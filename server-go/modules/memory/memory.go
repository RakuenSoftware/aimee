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

	gateRequestMagic  uint32 = 0x54524757
	gateResponseMagic uint32 = 0x56524757
	relTypeMax               = 256
	gateRequestLen           = 20 + relTypeMax
	gateResponseLen          = 8
)

const (
	ConfidenceLow uint32 = iota + 1
	ConfidenceMedium
	ConfidenceHigh
)

// Handle dispatches a memory stage call.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID == StageWrite {
		return handleWrite(invocation, request)
	}
	return handleRerank(invocation, request)
}

// handleWrite validates a candidate typed fact against the seed ontology.
//
// A relation the wire cannot carry never reaches here: the encoder refuses a
// label over relTypeMax rather than truncating it, so a length past the bound is
// a malformed request, not a long fact.
func handleWrite(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if len(request) != gateRequestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != gateRequestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint16(request[18:20]) != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	length := int(binary.LittleEndian.Uint16(request[16:18]))
	if length > relTypeMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	headKind := NodeKind(binary.LittleEndian.Uint32(request[8:12]))
	tailKind := NodeKind(binary.LittleEndian.Uint32(request[12:16]))
	verdict := GateCheck(headKind, string(request[20:20+length]), tailKind)
	response := make([]byte, gateResponseLen)
	binary.LittleEndian.PutUint32(response[0:4], gateResponseMagic)
	binary.LittleEndian.PutUint32(response[4:8], uint32(verdict))
	return response, bus.ModuleStatusOK
}

// handleRerank classifies a fixed-point reranking score into a confidence band.
func handleRerank(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
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
