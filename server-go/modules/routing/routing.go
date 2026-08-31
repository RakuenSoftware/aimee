// Package routing implements the route-selection process wire contract.
package routing

import (
	"encoding/binary"
	"sync/atomic"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind        uint32 = 6401
	StageSelect      uint32 = 1
	requestMagic     uint32 = 0x54554f52
	responseMagic    uint32 = 0x4c455352
	wireVersion      uint16 = 1
	requestLen              = 12
	responseLen             = 8
	selectBalanced   uint16 = 1
	selectRandomized uint16 = 2
)

// Selector owns the round-robin cursor for one routing process.
type Selector struct {
	cursor atomic.Uint32
}

var defaultSelector Selector

func mix64(value uint64) uint64 {
	value ^= value >> 30
	value *= 0xbf58476d1ce4e5b9
	value ^= value >> 27
	value *= 0x94d049bb133111eb
	return value ^ (value >> 31)
}

// Handle mirrors the C routing adapter exactly.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	return defaultSelector.Handle(invocation, request)
}

// Handle selects a candidate using this selector's independent cursor.
func (s *Selector) Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageSelect {
		return nil, bus.ModuleStatusInvalidRequest
	}
	// The C adapter observes cancellation before decoding the route body. Preserve
	// that precedence so a cancelled malformed request has the same result.
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	if len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint16(request[4:6]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	mode := binary.LittleEndian.Uint16(request[6:8])
	candidateCount := binary.LittleEndian.Uint32(request[8:12])
	if candidateCount == 0 || (mode != selectBalanced && mode != selectRandomized) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var selected uint32
	if mode == selectBalanced {
		selected = (s.cursor.Add(1) - 1) % candidateCount
	} else {
		selected = uint32(mix64(invocation.TraceID) % uint64(candidateCount))
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], selected)
	return response, bus.ModuleStatusOK
}
