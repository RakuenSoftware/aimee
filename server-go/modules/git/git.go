// Package git implements the git-operation process wire contract.
package git

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

const (
	EventKind        uint32 = 7425
	EventRefValidate uint32 = 7426
	EventCIGrade     uint32 = 7427
	StageOperation   uint32 = 1
	StageRefValidate uint32 = 2
	StageCIGrade     uint32 = 3

	// Stages 4-6 are the destination for the I/O that still lives in C: the
	// forge HTTP client, credential resolution, and the verify pipeline. They
	// are declared with the rest of the wire contract so the port lands one
	// caller at a time against a fixed stage table rather than renumbering as
	// it goes. No handler serves them yet.
	EventForgeRequest uint32 = 7428
	EventCredResolve  uint32 = 7429
	EventVerifyRun    uint32 = 7430
	StageForgeRequest uint32 = 4
	StageCredResolve  uint32 = 5
	StageVerifyRun    uint32 = 6

	requestMagic     uint32 = 0x53504f47
	responseMagic    uint32 = 0x534c4347
	refRequestMagic  uint32 = 0x46455247
	refResponseMagic uint32 = 0x4c415647
	wireVersion      byte   = 1
	opMax                   = 15
	requestLen              = 24
	responseLen             = 12
	refMax                  = 200
	refRequestLen           = 208
	refResponseLen          = 8
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

func validRef(ref string) bool {
	if ref == "" || ref[0] == '-' || strings.Contains(ref, "..") {
		return false
	}
	for index := 0; index < len(ref); index++ {
		char := ref[index]
		if !((char >= 'A' && char <= 'Z') || (char >= 'a' && char <= 'z') ||
			(char >= '0' && char <= '9') || char == '.' || char == '_' || char == '/' || char == '-') {
			return false
		}
	}
	return true
}

// Handle classifies Git operations and validates refs without repository I/O.
func Handle(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
	switch invocation.StageID {
	case StageForgeRequest:
		return handleForgeRequest(invocation, request)
	case StageCredResolve:
		return handleCredResolve(invocation, request)
	case StageVerifyRun:
		return handleVerifyState(invocation, request)
	case StageCIGrade:
		// JSON, not the fixed binary framing the other two stages use: a forge
		// payload is arbitrarily large and its shape is the forge's, not ours.
		var decoded CIGradeRequest
		if err := json.Unmarshal(request, &decoded); err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		encoded, err := json.Marshal(CIGradeResponse{
			Verdict: GradeCI(decoded.CheckRuns, decoded.CombinedStatus),
		})
		if err != nil || uint32(len(encoded)) > bus.ModuleMessageMaxBody {
			return nil, bus.ModuleStatusInternal
		}
		return encoded, bus.ModuleStatusOK
	case StageOperation:
		if len(request) != requestLen || binary.LittleEndian.Uint32(request[0:4]) != requestMagic ||
			request[4] != wireVersion || request[5] != 0 || request[7] != 0 ||
			request[6] == 0 || request[6] > opMax {
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
	case StageRefValidate:
		if len(request) != refRequestLen || binary.LittleEndian.Uint32(request[0:4]) != refRequestMagic ||
			request[4] != wireVersion || request[5] != 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		refLen := int(binary.LittleEndian.Uint16(request[6:8]))
		if refLen == 0 || refLen > refMax {
			return nil, bus.ModuleStatusInvalidRequest
		}
		ref := request[8 : 8+refLen]
		if bytes.IndexByte(ref, 0) >= 0 {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		response := make([]byte, refResponseLen)
		binary.LittleEndian.PutUint32(response[0:4], refResponseMagic)
		if validRef(string(ref)) {
			binary.LittleEndian.PutUint32(response[4:8], 1)
		}
		return response, bus.ModuleStatusOK
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
}
