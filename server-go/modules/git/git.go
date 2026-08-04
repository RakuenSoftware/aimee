// Package git implements the git-operation process wire contract.
package git

import (
	"encoding/binary"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind      uint32 = 7425
	StageOperation uint32 = 1
	requestMagic   uint32 = 0x53504f47
	responseMagic  uint32 = 0x534c4347
	wireVersion    byte   = 1
	opMax                 = 15
	requestLen            = 24
	responseLen           = 12
)

const (
	OperationUnsupported uint32 = iota
	OperationStatus
	OperationLog
	OperationDiff
	OperationBranch
	OperationFetch
	OperationPull
	OperationPush
	OperationCheckout
	OperationCommit
	OperationPR
)

var operations = map[string]uint32{
	"status": OperationStatus, "log": OperationLog, "diff": OperationDiff,
	"branch": OperationBranch, "fetch": OperationFetch, "pull": OperationPull,
	"push": OperationPush, "checkout": OperationCheckout, "commit": OperationCommit,
	"pr": OperationPR,
}

// Handle classifies a Git operation without performing repository I/O.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	if invocation.StageID != StageOperation || len(request) != requestLen ||
		binary.LittleEndian.Uint32(request[0:4]) != requestMagic || request[4] != wireVersion ||
		request[5] != 0 || request[7] != 0 || request[6] == 0 || request[6] > opMax {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	operation := operations[string(request[8:8+int(request[6])])]
	response := make([]byte, responseLen)
	binary.LittleEndian.PutUint32(response[0:4], responseMagic)
	binary.LittleEndian.PutUint32(response[4:8], operation)
	if operation == OperationFetch || operation == OperationPull || operation == OperationPush {
		binary.LittleEndian.PutUint32(response[8:12], 1)
	}
	return response, bus.ModuleStatusOK
}
