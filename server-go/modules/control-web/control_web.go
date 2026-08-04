// Package controlweb implements the control-web process's bounded proxy-route authorization.
package controlweb

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/control-web/policy"
)

const (
	EventAuthorize uint32 = 10241
	StageAuthorize uint32 = 1

	requestMagic  uint32 = 0x51455743
	responseMagic uint32 = 0x53455743
	wireVersion   uint32 = 1

	TargetConsoleAdmin uint32 = 1
	TargetFleet        uint32 = 2

	methodMax        = 15
	pathMax          = 511
	requestMethodOff = 32
	requestPathOff   = 48
	requestLen       = 560
	responseLen      = 16
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

func decodeText(slot []byte, wireLength uint32, maximum uint32) (string, bool) {
	if wireLength > maximum {
		return "", false
	}
	length := int(wireLength)
	if !nonzeroText(slot[:length]) || !zeroPadding(slot[length:]) {
		return "", false
	}
	return string(slot[:length]), true
}

func decodeRequest(request []byte) (uint32, string, string, bool) {
	if len(request) != requestLen || binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[20:24]) != 0 ||
		binary.LittleEndian.Uint32(request[24:28]) != 0 ||
		binary.LittleEndian.Uint32(request[28:32]) != 0 {
		return 0, "", "", false
	}
	target := binary.LittleEndian.Uint32(request[8:12])
	if target != TargetConsoleAdmin && target != TargetFleet {
		return 0, "", "", false
	}
	method, valid := decodeText(request[requestMethodOff:requestPathOff],
		binary.LittleEndian.Uint32(request[12:16]), methodMax)
	if !valid {
		return 0, "", "", false
	}
	path, valid := decodeText(request[requestPathOff:],
		binary.LittleEndian.Uint32(request[16:20]), pathMax)
	if !valid {
		return 0, "", "", false
	}
	return target, method, path, true
}

// Handle authorizes one control-web proxy route with the same policy used by
// the physical HTTPS/session provider.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	target, method, path, valid := decodeRequest(request)
	if invocation.StageID != StageAuthorize || !valid {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	allowed := policy.ConsoleAdminAllows(method, path)
	if target == TargetFleet {
		allowed = policy.FleetAllows(method, path)
	}
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	if allowed {
		binary.LittleEndian.PutUint32(response[8:12], 1)
	}
	return response, bus.ModuleStatusOK
}
