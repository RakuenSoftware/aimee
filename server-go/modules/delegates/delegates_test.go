package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func delegateRequest(role string) []byte {
	request := make([]byte, messageLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	request[4] = wireVersion
	request[6] = byte(len(role))
	copy(request[8:], role)
	return request
}

func TestRoleCanonicalizationParity(t *testing.T) {
	tests := map[string]string{
		"implement": "code", "build": "code", "reviewer": "review",
		"verifier": "validate", "test": "validate", "check": "validate",
		"evaluate": "validate", "evaluate-optimize": "validate", "inspect": "diagnose",
		"research": "execute", "enforce": "execute", "recall": "search",
		"synthesize": "summarize", "rank-fuse": "reason", "classify-score": "reason",
		"planner": "plan", "planning": "plan", "custom": "custom",
	}
	for role, want := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageInvoke}, delegateRequest(role))
		if status != bus.ModuleStatusOK || len(response) != messageLen ||
			binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
			response[4] != wireVersion || int(response[6]) != len(want) ||
			string(response[8:8+int(response[6])]) != want {
			t.Errorf("%q response = %x, status = %d, want %q", role, response, status, want)
		}
	}
}

func TestDelegatesRejectInvalidAndExpiredWire(t *testing.T) {
	request := delegateRequest("code")
	request[5] = 1
	if _, status := Handle(bus.ModuleInvocation{StageID: StageInvoke}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("reserved-byte status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageInvoke, DeadlineNS: 1},
		delegateRequest("code")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}
