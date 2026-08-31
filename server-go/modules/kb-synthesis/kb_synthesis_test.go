package kbsynthesis

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func groundingWire(kind claimKind, claims, callees []string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(kind))
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(claims)))
	binary.LittleEndian.PutUint32(request[16:20], uint32(len(callees)))
	for index, value := range claims {
		binary.LittleEndian.PutUint32(request[requestClaimLengthsOff+index*4:], uint32(len(value)))
		copy(request[requestClaimsOff+index*textSlot:], value)
	}
	for index, value := range callees {
		binary.LittleEndian.PutUint32(request[requestCalleeLengthsOff+index*4:], uint32(len(value)))
		copy(request[requestCalleesOff+index*textSlot:], value)
	}
	return request
}

func groundingDecision(t *testing.T, response []byte) (bool, string) {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(response[16:20]) != 0 ||
		binary.LittleEndian.Uint32(response[20:24]) != 0 {
		t.Fatalf("invalid response %x", response)
	}
	reasonLen := int(binary.LittleEndian.Uint32(response[12:16]))
	return binary.LittleEndian.Uint32(response[8:12]) == 1,
		string(response[responseReasonOff : responseReasonOff+reasonLen])
}

func TestGroundingDecisionParity(t *testing.T) {
	tests := []struct {
		name       string
		kind       claimKind
		claims     []string
		callees    []string
		wantReject bool
		wantReason string
	}{
		{"missing claim contradicts", claimNone, nil, []string{"strlen", "write"}, true, "write"},
		{"none string ignores case", claimString, []string{"No Side Effects"}, []string{"socket"}, true, "socket"},
		{"none array names first", claimStringArray, []string{"none", "n/a"}, []string{"strlen", "PQexec", "write"}, true, "PQexec"},
		{"empty array clean", claimStringArray, nil, []string{"strlen", "memcpy"}, false, ""},
		{"honest string permits effect", claimString, []string{"writes to disk"}, []string{"write"}, false, ""},
		{"mixed array permits effect", claimStringArray, []string{"none", "network"}, []string{"send"}, false, ""},
		{"nonstring permits effect", claimNonString, nil, []string{"open"}, false, ""},
		{"callee match is case sensitive", claimNone, nil, []string{"Write", "pqexec"}, false, ""},
		{"empty string is honest", claimString, []string{""}, []string{"unlink"}, false, ""},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, status := Handle(bus.ModuleInvocation{StageID: StageGrounding},
				groundingWire(test.kind, test.claims, test.callees))
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %d", status)
			}
			reject, reason := groundingDecision(t, response)
			if reject != test.wantReject || reason != test.wantReason {
				t.Fatalf("decision = %v/%q, want %v/%q", reject, reason,
					test.wantReject, test.wantReason)
			}
		})
	}
}

func TestGroundingSideEffectingCalleeSet(t *testing.T) {
	if len(sideEffectingCallees) != 74 {
		t.Fatalf("side-effecting callee count = %d, want 74", len(sideEffectingCallees))
	}
	for callee := range sideEffectingCallees {
		response, status := Handle(bus.ModuleInvocation{StageID: StageGrounding},
			groundingWire(claimNone, nil, []string{callee}))
		if status != bus.ModuleStatusOK {
			t.Fatalf("%s status = %d", callee, status)
		}
		reject, reason := groundingDecision(t, response)
		if !reject || reason != callee {
			t.Errorf("%s decision = %v/%q", callee, reject, reason)
		}
	}
}

func TestGroundingRejectsMalformedWire(t *testing.T) {
	valid := func() []byte { return groundingWire(claimNone, nil, []string{"strlen"}) }
	tests := [][]byte{nil, valid()[:requestLen-1]}
	badMagic := valid()
	badMagic[0] = 0
	tests = append(tests, badMagic)
	badVersion := valid()
	badVersion[4]++
	tests = append(tests, badVersion)
	badKind := valid()
	binary.LittleEndian.PutUint32(badKind[8:12], uint32(claimNonString)+1)
	tests = append(tests, badKind)
	badShape := valid()
	binary.LittleEndian.PutUint32(badShape[12:16], 1)
	tests = append(tests, badShape)
	tooManyClaims := groundingWire(claimStringArray, nil, nil)
	binary.LittleEndian.PutUint32(tooManyClaims[12:16], claimCountMax+1)
	tests = append(tests, tooManyClaims)
	tooManyCallees := valid()
	binary.LittleEndian.PutUint32(tooManyCallees[16:20], calleeCountMax+1)
	tests = append(tests, tooManyCallees)
	reserved := valid()
	reserved[20] = 1
	tests = append(tests, reserved)
	unusedLength := valid()
	binary.LittleEndian.PutUint32(unusedLength[requestClaimLengthsOff:], 1)
	tests = append(tests, unusedLength)
	oversizeLength := valid()
	binary.LittleEndian.PutUint32(oversizeLength[requestCalleeLengthsOff:], ^uint32(0))
	tests = append(tests, oversizeLength)
	padding := valid()
	padding[requestCalleesOff+len("strlen")] = 1
	tests = append(tests, padding)
	embeddedZero := groundingWire(claimString, []string{"none"}, nil)
	embeddedZero[requestClaimsOff+1] = 0
	tests = append(tests, embeddedZero)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageGrounding}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed request %d status = %d", index, status)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageGrounding + 1}, valid()); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestGroundingWireBounds(t *testing.T) {
	maxText := strings.Repeat("x", textMax)
	claims := make([]string, claimCountMax)
	callees := make([]string, calleeCountMax)
	for index := range claims {
		claims[index] = maxText
	}
	for index := range callees {
		callees[index] = maxText
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageGrounding},
		groundingWire(claimStringArray, claims, callees)); status != bus.ModuleStatusOK {
		t.Fatalf("maximum canonical wire status = %d", status)
	}
}

func TestGroundingHonorsCancellationAfterValidation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageGrounding, DeadlineNS: 1}
	valid := groundingWire(claimNone, nil, []string{"write"})
	if _, status := Handle(invocation, valid); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
