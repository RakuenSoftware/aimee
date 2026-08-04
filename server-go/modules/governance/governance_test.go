package governance

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func policyWire(active bool, names []string, stopReason string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	if active {
		binary.LittleEndian.PutUint32(request[8:12], 1)
	}
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(names)))
	binary.LittleEndian.PutUint32(request[16:20], uint32(len(stopReason)))
	for index, name := range names {
		lengthOffset := requestToolLengthsOff + index*4
		binary.LittleEndian.PutUint32(request[lengthOffset:lengthOffset+4], uint32(len(name)))
		nameOffset := requestToolNamesOff + index*toolNameSlot
		copy(request[nameOffset:nameOffset+toolNameSlot], name)
	}
	copy(request[requestStopReasonOff:], stopReason)
	return request
}

func decision(t *testing.T, response []byte) (uint32, uint32, string) {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
		binary.LittleEndian.Uint32(response[4:8]) != wireVersion {
		t.Fatalf("invalid response %x", response)
	}
	reasonLen := int(binary.LittleEndian.Uint32(response[16:20]))
	return binary.LittleEndian.Uint32(response[8:12]),
		binary.LittleEndian.Uint32(response[12:16]),
		string(response[responseStopReasonOff : responseStopReasonOff+reasonLen])
}

func TestGovernanceDecisionParity(t *testing.T) {
	tests := []struct {
		name, stopReason, wantReason string
		active                       bool
		tools                        []string
		wantMask, wantDrops          uint32
	}{
		{"inactive preserves response", "", "", false, []string{"Agent", "read_file"}, 3, 0},
		{"empty response remains empty", "", "", true, nil, 0, 0},
		{"all denied ends turn", "tool_use", "end_turn", true,
			[]string{"spawn_agent", "Task"}, 0, 2},
		{"partial preserves explicit reason", "max_tokens", "max_tokens", true,
			[]string{"read_file", "RemoteTrigger", "write_file"}, 5, 1},
		{"partial derives tool use", "", "tool_use", true,
			[]string{"Agent", "bash"}, 2, 1},
		{"none denied preserves reason", "refusal", "refusal", true,
			[]string{"agent", "delegate"}, 3, 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, status := Handle(bus.ModuleInvocation{StageID: StageEvaluate},
				policyWire(test.active, test.tools, test.stopReason))
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %d", status)
			}
			mask, drops, reason := decision(t, response)
			if mask != test.wantMask || drops != test.wantDrops || reason != test.wantReason {
				t.Fatalf("decision = %#x/%d/%q, want %#x/%d/%q",
					mask, drops, reason, test.wantMask, test.wantDrops, test.wantReason)
			}
		})
	}
}

func TestGovernanceRejectsMalformedWire(t *testing.T) {
	tests := [][]byte{nil}
	badMagic := policyWire(true, nil, "")
	badMagic[0] = 0
	tests = append(tests, badMagic)
	badVersion := policyWire(true, nil, "")
	badVersion[4]++
	tests = append(tests, badVersion)
	badActive := policyWire(true, nil, "")
	binary.LittleEndian.PutUint32(badActive[8:12], 2)
	tests = append(tests, badActive)
	badCount := policyWire(true, nil, "")
	binary.LittleEndian.PutUint32(badCount[12:16], toolCountMax+1)
	tests = append(tests, badCount)
	badReason := policyWire(true, nil, "")
	binary.LittleEndian.PutUint32(badReason[16:20], stopReasonMax+1)
	tests = append(tests, badReason)
	reserved := policyWire(true, nil, "")
	reserved[20] = 1
	tests = append(tests, reserved)
	unusedLength := policyWire(true, nil, "")
	binary.LittleEndian.PutUint32(unusedLength[requestToolLengthsOff:requestToolLengthsOff+4], 1)
	tests = append(tests, unusedLength)
	padding := policyWire(true, []string{"Task"}, "")
	padding[requestToolNamesOff+len("Task")] = 1
	tests = append(tests, padding)
	embeddedZero := policyWire(true, []string{"Task"}, "")
	embeddedZero[requestToolNamesOff+1] = 0
	tests = append(tests, embeddedZero)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageEvaluate}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed request %d status = %d", index, status)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageEvaluate + 1},
		policyWire(true, nil, "")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestGovernanceHonorsCancellationAfterValidation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageEvaluate, DeadlineNS: 1}
	if _, status := Handle(invocation, policyWire(true, []string{"Task"}, "")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
