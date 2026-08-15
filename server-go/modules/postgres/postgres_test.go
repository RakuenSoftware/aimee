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
		schema    bool
		pgTrgm    bool
		wantFlags uint32
	}{
		{"ready", true, true, flagSchema | flagPGTrgm},
		{"schema-missing", false, true, flagPGTrgm},
		{"extension-missing", true, false, flagSchema},
		{"both-missing", false, false, 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			handler := newHandler(func(context.Context) (bool, bool, error) {
				return test.schema, test.pgTrgm, nil
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
	handler := newHandler(func(context.Context) (bool, bool, error) { return true, true, nil })
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
	if _, status := newHandler(nil)(bus.ModuleInvocation{StageID: StageHealth}, valid); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("nil probe status = %v", status)
	}
}

func TestHealthMapsProbeFailureAndCancellation(t *testing.T) {
	failing := newHandler(func(context.Context) (bool, bool, error) {
		return false, false, errors.New("unavailable")
	})
	if response, status := failing(bus.ModuleInvocation{StageID: StageHealth}, healthRequest()); status != bus.ModuleStatusInternal || response != nil {
		t.Fatalf("failure = (%x, %v)", response, status)
	}

	invocation := bus.ModuleInvocation{StageID: StageHealth, DeadlineNS: 1}
	if response, status := failing(invocation, healthRequest()); status != bus.ModuleStatusCancelled || response != nil {
		t.Fatalf("cancelled = (%x, %v)", response, status)
	}
}
