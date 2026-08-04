package runtimeweb

import (
	"encoding/binary"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/runtime-web/policy"
)

func classifyWire(kind string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(kind)))
	copy(request[requestKindOff:], kind)
	return request
}

func responseStatus(t *testing.T, response []byte) uint32 {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion ||
		binary.LittleEndian.Uint32(response[12:16]) != 0 {
		t.Fatalf("invalid response %x", response)
	}
	return binary.LittleEndian.Uint32(response[8:12])
}

func TestRPCFaultStatusParity(t *testing.T) {
	tests := []struct {
		kind string
		want uint32
	}{
		{"invalid_argument", 400},
		{"not_found", 404},
		{"permission_denied", 403},
		{"unavailable", 503},
		{"", 502},
		{"unknown", 502},
		{"INVALID_ARGUMENT", 502},
	}
	for _, test := range tests {
		t.Run(test.kind, func(t *testing.T) {
			if got := policy.HTTPStatusForRPCFault(test.kind); uint32(got) != test.want {
				t.Fatalf("shared policy status = %d, want %d", got, test.want)
			}
			response, status := Handle(bus.ModuleInvocation{StageID: StageClassify}, classifyWire(test.kind))
			if status != bus.ModuleStatusOK {
				t.Fatalf("handler status = %d", status)
			}
			if got := responseStatus(t, response); got != test.want {
				t.Fatalf("response status = %d, want %d", got, test.want)
			}
		})
	}
}

func TestRPCFaultRejectsMalformedWire(t *testing.T) {
	valid := func() []byte { return classifyWire("not_found") }
	tests := [][]byte{nil, valid()[:requestLen-1]}
	badMagic := valid()
	badMagic[0] = 0
	tests = append(tests, badMagic)
	badVersion := valid()
	badVersion[4]++
	tests = append(tests, badVersion)
	reserved := valid()
	reserved[12] = 1
	tests = append(tests, reserved)
	oversize := valid()
	binary.LittleEndian.PutUint32(oversize[8:12], ^uint32(0))
	tests = append(tests, oversize)
	embeddedZero := valid()
	embeddedZero[requestKindOff+1] = 0
	tests = append(tests, embeddedZero)
	padding := valid()
	padding[requestKindOff+len("not_found")] = 1
	tests = append(tests, padding)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageClassify}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed request %d status = %d", index, status)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageClassify + 1}, valid()); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestRPCFaultWireBoundAndCancellation(t *testing.T) {
	maxKind := strings.Repeat("x", kindMax)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageClassify}, classifyWire(maxKind)); status != bus.ModuleStatusOK {
		t.Fatalf("maximum canonical wire status = %d", status)
	}
	invocation := bus.ModuleInvocation{StageID: StageClassify, DeadlineNS: 1}
	if _, status := Handle(invocation, classifyWire("unavailable")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
