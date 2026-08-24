package postgres

import (
	"context"
	"encoding/binary"
	"errors"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func healthRequest() []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	return request
}

func TestHealthReportsSchemaAndExtensionEvidence(t *testing.T) {
	tests := []struct {
		name      string
		evidence  healthEvidence
		wantFlags uint32
	}{
		{"ready", healthEvidence{true, true, true}, flagSchema | flagPGTrgm | flagKBTables},
		{"schema-missing", healthEvidence{false, true, true}, flagPGTrgm | flagKBTables},
		{"extension-missing", healthEvidence{true, false, true}, flagSchema | flagKBTables},
		{"kb-tables-missing", healthEvidence{true, true, false}, flagSchema | flagPGTrgm},
		{"only-schema", healthEvidence{true, false, false}, flagSchema},
		{"only-extension", healthEvidence{false, true, false}, flagPGTrgm},
		{"only-kb-tables", healthEvidence{false, false, true}, flagKBTables},
		{"all-missing", healthEvidence{}, 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			handler := newHandler(func(context.Context) (healthEvidence, error) {
				return test.evidence, nil
			})
			response, status := handler(bus.ModuleInvocation{StageID: StageHealth}, healthRequest())
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if len(response) != responseLen || binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
				binary.LittleEndian.Uint32(response[4:8]) != wireVersion ||
				binary.LittleEndian.Uint32(response[8:12]) != test.wantFlags ||
				binary.LittleEndian.Uint32(response[12:16]) != 0 {
				t.Fatalf("response = %x, want flags %#x", response, test.wantFlags)
			}
		})
	}
}

func TestHealthRejectsMalformedOrWrongStageRequests(t *testing.T) {
	handler := newHandler(func(context.Context) (healthEvidence, error) {
		return healthEvidence{true, true, true}, nil
	})
	valid := healthRequest()
	cases := [][]byte{
		nil,
		valid[:len(valid)-1],
		append(append([]byte(nil), valid...), 0),
		append([]byte{0}, valid[1:]...),
		append(append([]byte(nil), valid[:4]...), 2, 0, 0, 0),
	}
	for _, request := range cases {
		if _, status := handler(bus.ModuleInvocation{StageID: StageHealth}, request); status != bus.ModuleStatusInvalidRequest {
			t.Fatalf("request %x status = %v", request, status)
		}
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageHealth + 1}, valid); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong stage status = %v", status)
	}
	if _, status := newHandler(nil)(bus.ModuleInvocation{StageID: StageHealth}, valid); status != bus.ModuleStatusInternal {
		t.Fatalf("nil probe status = %v", status)
	}
}

func TestHealthMapsProbeFailureAndCancellation(t *testing.T) {
	failing := newHandler(func(context.Context) (healthEvidence, error) {
		return healthEvidence{}, errors.New("unavailable")
	})
	if response, status := failing(bus.ModuleInvocation{StageID: StageHealth}, healthRequest()); status != bus.ModuleStatusInternal || response != nil {
		t.Fatalf("failure = (%x, %v)", response, status)
	}

	invocation := bus.ModuleInvocation{StageID: StageHealth, DeadlineNS: 1}
	if response, status := failing(invocation, healthRequest()); status != bus.ModuleStatusCancelled || response != nil {
		t.Fatalf("cancelled = (%x, %v)", response, status)
	}
}

func TestTheExportedHandleReachesTheHandler(t *testing.T) {
	// A thin wrapper is the thing that can point at the wrong place with every
	// test underneath it green, because the tests drive the handler directly.
	// This asserts the wiring rather than the logic: main.go registers Handle,
	// and a Handle that reached nothing would refuse nothing.
	body, status := Handle(bus.ModuleInvocation{StageID: StageHealth}, nil)
	if status == bus.ModuleStatusOK {
		t.Fatal("a malformed health request was accepted; Handle is not reaching " +
			"the handler that refuses it")
	}
	if body != nil {
		t.Errorf("a refusal carried a body of %d bytes", len(body))
	}
}
