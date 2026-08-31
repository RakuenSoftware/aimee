// Package runtimeweb implements the runtime-web process's bounded RPC fault classification.
package runtimeweb

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/runtime-web/policy"
)

const (
	EventClassify uint32 = 9985
	StageClassify uint32 = 1

	requestMagic   uint32 = 0x51455752
	responseMagic  uint32 = 0x53455752
	wireVersion    uint32 = 1
	kindMax               = 31
	requestKindOff        = 16
	requestLen            = 48
	responseLen           = 16
)

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

func decodeRequest(request []byte) (string, bool) {
	if len(request) != requestLen || binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[12:16]) != 0 {
		return "", false
	}
	wireLen := binary.LittleEndian.Uint32(request[8:12])
	if wireLen > kindMax {
		return "", false
	}
	kindLen := int(wireLen)
	slot := request[requestKindOff:]
	if !nonzeroText(slot[:kindLen]) || !zeroPadding(slot[kindLen:]) {
		return "", false
	}
	return string(slot[:kindLen]), true
}

// Handle maps one server-classified RPC fault kind onto runtime-web's HTTP status.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	kind, valid := decodeRequest(request)
	if invocation.StageID != StageClassify || !valid {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	binary.LittleEndian.PutUint32(response[8:12], uint32(policy.HTTPStatusForRPCFault(kind)))
	return response, bus.ModuleStatusOK
}
