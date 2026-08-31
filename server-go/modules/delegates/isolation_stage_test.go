package delegates

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func isolationRequest(report string, probeFailed bool) []byte {
	out := make([]byte, isolationReqHeaderLen)
	binary.LittleEndian.PutUint32(out[0:4], isolationRequestMagic)
	out[4] = wireVersion
	if probeFailed {
		out[5] |= isolationFlagProbeFailed
	}
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(report)))
	return append(out, report...)
}

func TestIsolationStageIsUnconditionallyFailClosed(t *testing.T) {
	for _, request := range [][]byte{isolationRequest("bridge=172.17.0.2;", false), isolationRequest("", true)} {
		response, status := Handle(bus.ModuleInvocation{StageID: StageIsolation}, request)
		if status != bus.ModuleStatusOK || len(response) < 16 || binary.LittleEndian.Uint32(response[4:8]) != 1 {
			t.Fatalf("unverified isolation did not refuse: status=%v response=%x", status, response)
		}
	}
	response, status := Handle(bus.ModuleInvocation{StageID: StageIsolation}, isolationRequest("none=;", false))
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint32(response[4:8]) != 0 {
		t.Fatalf("confirmed isolation refused: status=%v response=%x", status, response)
	}
}
