package learning

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func learningRequest(signal string) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	request[4] = wireVersion
	request[6] = byte(len(signal))
	copy(request[8:], signal)
	return request
}

func TestLearningSignalParity(t *testing.T) {
	tests := map[string]uint32{
		"thumb_up":             SinkReranker,
		"thumb_down":           SinkReranker,
		"correction":           SinkReranker | SinkSupersede | SinkRule,
		"preference_statement": SinkRule,
		"mark_rule":            SinkRule,
		"workflow_repetition":  SinkWorkflow,
		"unknown":              0,
	}
	for signal, want := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageObserve}, learningRequest(signal))
		if status != bus.ModuleStatusOK || len(response) != responseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != want {
			t.Errorf("%q response = %x, status = %d, want mask %d", signal, response, status, want)
		}
	}
}

func TestLearningRejectsInvalidWire(t *testing.T) {
	request := learningRequest("thumb_up")
	request[5] = 1
	if _, status := Handle(bus.ModuleInvocation{StageID: StageObserve}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("reserved-byte status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageObserve, DeadlineNS: 1},
		learningRequest("thumb_up")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}
