package git

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func gitRequest(operation string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	request[4] = wireVersion
	request[6] = byte(len(operation))
	copy(request[8:], operation)
	return request
}

func TestGitOperationParity(t *testing.T) {
	tests := map[string]struct {
		operation   uint32
		credentials uint32
	}{
		"status": {OperationStatus, 0}, "log": {OperationLog, 0},
		"diff": {OperationDiff, 0}, "branch": {OperationBranch, 0},
		"fetch": {OperationFetch, 1}, "pull": {OperationPull, 1},
		"push": {OperationPush, 1}, "checkout": {OperationCheckout, 0},
		"commit": {OperationCommit, 0}, "pr": {OperationPR, 0},
		"unknown": {OperationUnsupported, 0},
	}
	for name, want := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageOperation}, gitRequest(name))
		if status != bus.ModuleStatusOK || len(response) != responseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != want.operation ||
			binary.LittleEndian.Uint32(response[8:12]) != want.credentials {
			t.Errorf("%q response = %x, status = %d, want %+v", name, response, status, want)
		}
	}
}

func TestGitRejectsInvalidAndExpiredWire(t *testing.T) {
	request := gitRequest("push")
	request[7] = 1
	if _, status := Handle(bus.ModuleInvocation{StageID: StageOperation}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("reserved-byte status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageOperation, DeadlineNS: 1},
		gitRequest("push")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}
