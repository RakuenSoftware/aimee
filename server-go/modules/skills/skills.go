// Package skills implements the skill-context process wire contract.
package skills

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind     uint32 = 7681
	StageContext  uint32 = 1
	requestMagic  uint32 = 0x58435453
	responseMagic uint32 = 0x57454956
	wireVersion   uint32 = 1
	requestLen           = 16
	responseLen          = 8
)

// Handle decides whether the configured review interval fires at this hook.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageContext || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	count := int32(binary.LittleEndian.Uint32(request[8:12]))
	interval := int32(binary.LittleEndian.Uint32(request[12:16]))
	var fire uint32
	if interval > 0 && count > 0 && count%interval == 0 {
		fire = 1
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], fire)
	return response, bus.ModuleStatusOK
}
