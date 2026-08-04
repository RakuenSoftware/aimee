package memory

import (
	"encoding/binary"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func memoryRequest(score int64) []byte {
	request := make([]byte, requestLen)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint64(request[8:16], uint64(score))
	return request
}

func TestRerankConfidenceParity(t *testing.T) {
	tests := []struct {
		score int64
		want  uint32
	}{
		{-1, ConfidenceLow},
		{0, ConfidenceLow},
		{329999, ConfidenceLow},
		{330000, ConfidenceMedium},
		{659999, ConfidenceMedium},
		{660000, ConfidenceHigh},
	}
	for _, test := range tests {
		response, status := Handle(bus.ModuleInvocation{StageID: StageRerank}, memoryRequest(test.score))
		if status != bus.ModuleStatusOK || len(response) != responseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != responseMagic ||
			binary.LittleEndian.Uint32(response[4:8]) != test.want {
			t.Errorf("score %d response = %x, status = %d, want %d", test.score, response, status, test.want)
		}
	}
}

func TestMemoryRejectsUnimplementedAndMalformedStages(t *testing.T) {
	for _, stage := range []uint32{StageExtractIndex, StageWrite, StageEmbed, StageRetrieve} {
		if _, status := Handle(bus.ModuleInvocation{StageID: stage},
			memoryRequest(660000)); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("unimplemented stage %d status = %d", stage, status)
		}
	}
	request := memoryRequest(660000)
	binary.LittleEndian.PutUint32(request[0:4], requestMagic+1)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRerank}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wire-magic status = %d", status)
	}
	request = memoryRequest(660000)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion+1)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRerank}, request); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wire-version status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRerank}, request[:len(request)-1]); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("short-request status = %d", status)
	}
}

func TestMemoryHonorsCancellationAfterValidation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageRerank, DeadlineNS: 1}
	if _, status := Handle(invocation, memoryRequest(660000)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
	if _, status := Handle(invocation, nil); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("malformed expired-request status = %d", status)
	}
}
