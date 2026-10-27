// Package tools implements the tool-dispatch process wire contract.
package tools

import (
	"bytes"
	"encoding/binary"

	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

const (
	EventKind     uint32 = 6913
	StageDispatch uint32 = 1
	requestMagic  uint32 = 0x53494454
	responseMagic uint32 = 0x534c4354
	wireVersion   byte   = 1
	requestLen           = 104
	responseLen          = 8
	nameMax              = 95
)

const (
	ClassUnknown uint32 = iota
	ClassRead
	ClassWrite
	ClassExec
	ClassControl
	ClassRemote
)

// Keep the original classification contract, now backed by the complete catalog.
func classify(name string) uint32 { return metadata(name).Class }

const metadataVersion byte = 2
const metadataMagic uint32 = 0x4154454d
const metadataLen = 16

// Handle classifies a tool name without dispatching the tool itself.
func Handle(invocation ring.ModuleInvocation, request []byte) ([]byte, ring.ModuleStatus) {
	if invocation.StageID == StageActionResource {
		return handleActionResource(invocation, request)
	}
	if invocation.StageID != StageDispatch || len(request) < 8 ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic || request[5] != 0 {
		return nil, ring.ModuleStatusInvalidRequest
	}
	enriched := request[4] == metadataVersion
	nameLength := int(request[6])
	if enriched {
		nameLength = int(binary.LittleEndian.Uint16(request[6:8]))
		if nameLength == 0 || nameLength > 4095 || len(request) != nameLength+8 || bytes.IndexByte(request[8:], 0) >= 0 {
			return nil, ring.ModuleStatusInvalidRequest
		}
	} else if request[4] != wireVersion || len(request) != requestLen || request[7] != 0 || nameLength == 0 || nameLength > nameMax {
		return nil, ring.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, ring.ModuleStatusCancelled
	}
	name := string(request[8 : 8+nameLength])
	if enriched {
		if catalogError != nil {
			return nil, ring.ModuleStatusInternal
		}
		value := metadata(name)
		response := make([]byte, metadataLen)
		binary.LittleEndian.PutUint32(response[0:4], metadataMagic)
		binary.LittleEndian.PutUint32(response[4:8], value.Class)
		binary.LittleEndian.PutUint32(response[8:12], value.Primary)
		binary.LittleEndian.PutUint32(response[12:16], value.Categories)
		return response, ring.ModuleStatusOK
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], classify(name))
	return response, ring.ModuleStatusOK
}
