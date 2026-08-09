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

func gitRefRequest(ref string) []byte {
	request := make([]byte, refRequestLen)
	binary.LittleEndian.PutUint32(request[0:4], refRequestMagic)
	request[4] = wireVersion
	binary.LittleEndian.PutUint16(request[6:8], uint16(len(ref)))
	copy(request[8:], ref)
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

func TestGitRefValidation(t *testing.T) {
	tests := map[string]bool{
		"main": true, "feature/topic-1": true, "release_1.2": true,
		"aimee/wi/wi_57186250728b511961573e5afb37cc93.s4263a4834d.g0.0": true,
		"-evil": false, "feature/../main": false, "has space": false,
		"unicode-é": false, "colon:ref": false,
	}
	for ref, want := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageRefValidate}, gitRefRequest(ref))
		if status != bus.ModuleStatusOK || len(response) != refResponseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != refResponseMagic ||
			(binary.LittleEndian.Uint32(response[4:8]) == 1) != want {
			t.Errorf("%q response = %x, status = %d, want allowed=%v", ref, response, status, want)
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
	refRequest := gitRefRequest("main")
	refRequest[5] = 1
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRefValidate}, refRequest); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("ref reserved-byte status = %d", status)
	}
	tooLong := make([]byte, refRequestLen)
	binary.LittleEndian.PutUint32(tooLong[0:4], refRequestMagic)
	tooLong[4] = wireVersion
	binary.LittleEndian.PutUint16(tooLong[6:8], refMax+1)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRefValidate}, tooLong); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("oversize ref status = %d", status)
	}
	embeddedNUL := gitRefRequest("main-x")
	embeddedNUL[12] = 0
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRefValidate}, embeddedNUL); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("embedded-NUL wire status = %d", status)
	}
}

// The stage table is the contract in src/modules/process-contracts.json, whose
// event kinds are derived rather than chosen: 4096 + principal_ref*256 + stage.
// Stages 4-6 are declared ahead of the C-to-Go port so callers land one at a
// time against fixed numbering; until a handler serves them they must be
// refused, not silently accepted.
func TestGitStageTableMatchesContract(t *testing.T) {
	const principalRef = 13
	stages := map[uint32]uint32{
		StageOperation:    EventKind,
		StageRefValidate:  EventRefValidate,
		StageCIGrade:      EventCIGrade,
		StageForgeRequest: EventForgeRequest,
		StageCredResolve:  EventCredResolve,
		StageVerifyRun:    EventVerifyRun,
	}
	if len(stages) != 6 {
		t.Fatalf("stage table has %d entries, want 6 dense stages", len(stages))
	}
	for stage, event := range stages {
		if want := 4096 + principalRef*256 + stage; event != want {
			t.Fatalf("stage %d event kind = %d, want %d", stage, event, want)
		}
	}
	// Forge and credential resolution stay unserved until the vault has a bus
	// surface: git_pr_api.c resolves its token in-process precisely so it never
	// reaches another process, and honouring that from a module process needs
	// the vault extraction in vault-bus-only-access.md.
	for _, stage := range []uint32{StageForgeRequest, StageCredResolve} {
		if _, status := Handle(bus.ModuleInvocation{StageID: stage}, nil); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("unserved stage %d status = %d, want refused", stage, status)
		}
	}
}
