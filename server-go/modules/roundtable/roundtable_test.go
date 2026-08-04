package roundtable

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func rubricRequest(status uint32, factual bool, severity string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], status)
	if factual {
		binary.LittleEndian.PutUint32(request[12:16], 1)
	}
	binary.LittleEndian.PutUint32(request[16:20], uint32(len(severity)))
	copy(request[requestSeverityOff:], severity)
	return request
}

func decodeRubricResponse(t *testing.T, response []byte) (uint32, string) {
	t.Helper()
	if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic {
		t.Fatalf("invalid response %x", response)
	}
	severityLen := int(binary.LittleEndian.Uint32(response[8:12]))
	if severityLen > severityMax || !zeroPadding(response[responseSeverityOff+severityLen:]) {
		t.Fatalf("invalid response severity %x", response)
	}
	return binary.LittleEndian.Uint32(response[4:8]),
		string(response[responseSeverityOff : responseSeverityOff+severityLen])
}

func TestVerificationRubricParity(t *testing.T) {
	tests := []struct {
		name         string
		status       uint32
		factual      bool
		claimed      string
		wantAction   uint32
		wantSeverity string
	}{
		{"contradicted", ReplayContradicted, true, "blocking", VerifyReject, ""},
		{"vacuous", ReplayVacuous, true, "blocking", VerifyReject, ""},
		{"index unavailable", ReplayIndexUnavailable, true, "blocking", VerifyDegrade, "blocking"},
		{"index unavailable default", ReplayIndexUnavailable, true, "", VerifyDegrade, "suggestion"},
		{"no evidence", ReplayNoEvidence, false, "blocking", VerifyCap, "suggestion"},
		{"no evidence nit", ReplayNoEvidence, false, "nit", VerifyCap, "nit"},
		{"factual match", ReplayMatch, true, "blocking", VerifyKeep, "blocking"},
		{"factual corrected", ReplayCorrected, true, "nit", VerifyKeep, "nit"},
		{"interpretive match", ReplayMatch, false, "blocking", VerifyCap, "suggestion"},
		{"interpretive default", ReplayCorrected, false, "", VerifyCap, "suggestion"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response, status := Handle(bus.ModuleInvocation{StageID: StageDeliberate},
				rubricRequest(test.status, test.factual, test.claimed))
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %d", status)
			}
			action, severity := decodeRubricResponse(t, response)
			if action != test.wantAction || severity != test.wantSeverity {
				t.Fatalf("action/severity = %d/%q, want %d/%q", action, severity,
					test.wantAction, test.wantSeverity)
			}
		})
	}
}

func TestRoundtableRejectsMalformedWire(t *testing.T) {
	tests := [][]byte{
		nil,
		rubricRequest(ReplayIndexUnavailable+1, true, "blocking"),
		rubricRequest(ReplayMatch, true, "0123456789abcdef"),
	}
	reserved := rubricRequest(ReplayMatch, true, "blocking")
	reserved[20] = 1
	tests = append(tests, reserved)
	padding := rubricRequest(ReplayMatch, true, "nit")
	padding[requestSeverityOff+len("nit")] = 1
	tests = append(tests, padding)
	for index, request := range tests {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageDeliberate}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("malformed request %d status = %d", index, status)
		}
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageDeliberate + 1},
		rubricRequest(ReplayMatch, true, "blocking")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong-stage status = %d", status)
	}
}

func TestRoundtableHonorsCancellationAfterValidation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageDeliberate, DeadlineNS: 1}
	if _, status := Handle(invocation, rubricRequest(ReplayMatch, true, "blocking")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
