// Package workspace implements the workspace-access process wire contract.
package workspace

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind     uint32 = 7169
	StageAccess   uint32 = 1
	requestMagic  uint32 = 0x46455257
	responseMagic uint32 = 0x4b4f5757
	wireVersion   byte   = 1
	refMax               = 129
	requestLen           = 140
	responseLen          = 8
)

func asciiAlphanumeric(value byte) bool {
	return value >= 'A' && value <= 'Z' || value >= 'a' && value <= 'z' ||
		value >= '0' && value <= '9'
}

func nameValid(name []byte) bool {
	if len(name) == 0 || len(name) > 64 || string(name) == "." || string(name) == ".." ||
		!asciiAlphanumeric(name[0]) {
		return false
	}
	for _, value := range name {
		if !asciiAlphanumeric(value) && value != '.' && value != '_' && value != '-' {
			return false
		}
	}
	return true
}

func refValid(ref []byte) bool {
	if len(ref) == 0 || len(ref) > refMax {
		return false
	}
	slash := -1
	for index, value := range ref {
		if value == 0 {
			return false
		}
		if value == '/' {
			if slash >= 0 {
				return false
			}
			slash = index
		}
	}
	if slash < 0 {
		return nameValid(ref)
	}
	return nameValid(ref[:slash]) && nameValid(ref[slash+1:])
}

// Handle validates a bounded project or owner/project reference.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageAccess || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic || request[4] != wireVersion ||
		request[5] != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	length := int(binary.LittleEndian.Uint16(request[6:8]))
	if length == 0 || length > refMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	if refValid(request[8 : 8+length]) {
		binary.LittleEndian.PutUint32(response[4:8], 1)
	}
	return response, bus.ModuleStatusOK
}
