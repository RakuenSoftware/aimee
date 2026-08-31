// Package kbsynthesis implements the KB synthesis process's bounded grounding decision.
package kbsynthesis

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventGrounding uint32 = 9729
	StageGrounding uint32 = 1

	requestMagic  uint32 = 0x5147534b
	responseMagic uint32 = 0x5247534b
	wireVersion   uint32 = 1

	claimCountMax  = 16
	calleeCountMax = 64
	textMax        = 63
	textSlot       = 64

	requestClaimLengthsOff  = 24
	requestCalleeLengthsOff = 88
	requestClaimsOff        = 344
	requestCalleesOff       = 1368
	requestLen              = 5464
	responseReasonOff       = 24
	responseLen             = 88
)

type claimKind uint32

const (
	claimNone claimKind = iota
	claimString
	claimStringArray
	claimNonString
)

type groundingRequest struct {
	kind    claimKind
	claims  []string
	callees []string
}

var sideEffectingCallees = map[string]struct{}{
	"accept": {}, "aimee_pg_exec": {}, "aimee_pg_step": {}, "bind": {},
	"chdir": {}, "chmod": {}, "chown": {}, "close": {}, "creat": {},
	"execle": {}, "execl": {}, "execlp": {}, "execv": {}, "execve": {}, "execvp": {},
	"fclose": {}, "fgets": {}, "fopen": {}, "fputs": {}, "fread": {}, "freopen": {},
	"fsync": {}, "fdatasync": {}, "fdopen": {}, "fflush": {}, "ftruncate": {},
	"fwrite": {}, "fprintf": {}, "fork": {}, "ioctl": {}, "kill": {}, "link": {},
	"listen": {}, "lseek": {}, "mkdir": {}, "mmap": {}, "munmap": {}, "open": {},
	"openat": {}, "pclose": {}, "popen": {}, "posix_spawn": {}, "pread": {},
	"PQexec": {}, "PQexecParams": {}, "putenv": {}, "pwrite": {}, "raise": {},
	"read": {}, "recv": {}, "recvfrom": {}, "remove": {}, "rename": {}, "renameat": {},
	"rewind": {}, "rmdir": {}, "send": {}, "sendto": {}, "setenv": {}, "sigaction": {},
	"signal": {}, "socket": {}, "sqlite3_exec": {}, "sqlite3_prepare_v2": {},
	"sqlite3_step": {}, "symlink": {}, "system": {}, "truncate": {}, "unlink": {},
	"unlinkat": {}, "unsetenv": {}, "vfprintf": {}, "vfork": {}, "write": {},
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

func claimShapeValid(kind claimKind, count int) bool {
	switch kind {
	case claimNone, claimNonString:
		return count == 0
	case claimString:
		return count == 1
	case claimStringArray:
		return count <= claimCountMax
	default:
		return false
	}
}

func decodeTextArray(request []byte, count, maximum, lengthsOff, valuesOff int) ([]string, bool) {
	values := make([]string, count)
	for index := range maximum {
		wireLen := binary.LittleEndian.Uint32(request[lengthsOff+index*4 : lengthsOff+index*4+4])
		if wireLen > textMax {
			return nil, false
		}
		textLen := int(wireLen)
		slot := request[valuesOff+index*textSlot : valuesOff+(index+1)*textSlot]
		if (index >= count && textLen != 0) ||
			!nonzeroText(slot[:textLen]) || !zeroPadding(slot[textLen:]) {
			return nil, false
		}
		if index < count {
			values[index] = string(slot[:textLen])
		}
	}
	return values, true
}

func decodeRequest(request []byte) (groundingRequest, bool) {
	if len(request) != requestLen || binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
		binary.LittleEndian.Uint32(request[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(request[8:12]) > uint32(claimNonString) ||
		binary.LittleEndian.Uint32(request[12:16]) > claimCountMax ||
		binary.LittleEndian.Uint32(request[16:20]) > calleeCountMax ||
		binary.LittleEndian.Uint32(request[20:24]) != 0 {
		return groundingRequest{}, false
	}
	kind := claimKind(binary.LittleEndian.Uint32(request[8:12]))
	claimCount := int(binary.LittleEndian.Uint32(request[12:16]))
	calleeCount := int(binary.LittleEndian.Uint32(request[16:20]))
	if !claimShapeValid(kind, claimCount) {
		return groundingRequest{}, false
	}
	claims, valid := decodeTextArray(request, claimCount, claimCountMax,
		requestClaimLengthsOff, requestClaimsOff)
	if !valid {
		return groundingRequest{}, false
	}
	callees, valid := decodeTextArray(request, calleeCount, calleeCountMax,
		requestCalleeLengthsOff, requestCalleesOff)
	if !valid {
		return groundingRequest{}, false
	}
	return groundingRequest{kind: kind, claims: claims, callees: callees}, true
}

func asciiEqualFold(value, expected string) bool {
	if len(value) != len(expected) {
		return false
	}
	for index := range value {
		left := value[index]
		if left >= 'A' && left <= 'Z' {
			left += 'a' - 'A'
		}
		if left != expected[index] {
			return false
		}
	}
	return true
}

func noneLike(value string) bool {
	for _, expected := range [...]string{"none", "no", "no side effects", "pure", "n/a"} {
		if asciiEqualFold(value, expected) {
			return true
		}
	}
	return false
}

func claimsNoSideEffects(request groundingRequest) bool {
	switch request.kind {
	case claimNone:
		return true
	case claimString:
		return noneLike(request.claims[0])
	case claimStringArray:
		for _, claim := range request.claims {
			if !noneLike(claim) {
				return false
			}
		}
		return true
	default:
		return false
	}
}

func decide(request groundingRequest) (bool, string) {
	if !claimsNoSideEffects(request) {
		return false, ""
	}
	for _, callee := range request.callees {
		if _, found := sideEffectingCallees[callee]; found {
			return true, callee
		}
	}
	return false, ""
}

// Handle rejects a false no-side-effects claim and names the first structural contradiction.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	decoded, valid := decodeRequest(request)
	if invocation.StageID != StageGrounding || !valid {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	contradicts, reason := decide(decoded)
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], wireVersion)
	if contradicts {
		binary.LittleEndian.PutUint32(response[8:12], 1)
	}
	binary.LittleEndian.PutUint32(response[12:16], uint32(len(reason)))
	copy(response[responseReasonOff:], reason)
	return response, bus.ModuleStatusOK
}
