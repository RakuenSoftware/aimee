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
	for _, stage := range []uint32{StageExtractIndex, StageEmbed, StageRetrieve} {
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

// gateRequest mirrors aimee_memory_gate_request_encode byte for byte; the two
// encoders are the wire contract, so a drift in either must show up here.
func gateRequest(head NodeKind, relType string, tail NodeKind) []byte {
	request := make([]byte, gateRequestLen)
	binary.LittleEndian.PutUint32(request[0:4], gateRequestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(head))
	binary.LittleEndian.PutUint32(request[12:16], uint32(tail))
	binary.LittleEndian.PutUint16(request[16:18], uint16(len(relType)))
	copy(request[20:], relType)
	return request
}

// The wire path must carry every verdict the gate can reach, not just the ones
// a handful of hand-picked triples happen to hit — a stage that decoded the
// kinds in the wrong order would still agree on a symmetric case.
func TestWriteStageCarriesTheGateVerdict(t *testing.T) {
	cases := gateMatrixCases(t)
	if len(cases) == 0 {
		t.Fatal("fixture was empty; the comparison would pass vacuously")
	}
	seen := map[FactVerdict]int{}
	for _, test := range cases {
		if len(test.relType) > relTypeMax {
			continue // the C encoder refuses these; they never reach the stage
		}
		response, status := Handle(bus.ModuleInvocation{StageID: StageWrite},
			gateRequest(test.head, test.relType, test.tail))
		if status != bus.ModuleStatusOK || len(response) != gateResponseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != gateResponseMagic {
			t.Fatalf("(%d, %q, %d) response = %x, status = %d", test.head, test.relType, test.tail,
				response, status)
		}
		if got := FactVerdict(binary.LittleEndian.Uint32(response[4:8])); got != test.want {
			t.Fatalf("(%d, %q, %d) verdict = %d, C gate = %d", test.head, test.relType, test.tail,
				got, test.want)
		}
		seen[test.want]++
	}
	for _, verdict := range []FactVerdict{FactAccept, FactRejectKind, FactNovel, FactBadArg} {
		if seen[verdict] == 0 {
			t.Errorf("verdict %d never crossed the wire; the stage is undercovered", verdict)
		}
	}
}

func TestWriteStageRejectsMalformedRequests(t *testing.T) {
	valid := gateRequest(NodePerson, "works_for", NodeOrg)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageWrite}, valid); status != bus.ModuleStatusOK {
		t.Fatalf("valid gate request status = %d", status)
	}
	malformed := map[string]func([]byte){
		"wire magic":     func(r []byte) { binary.LittleEndian.PutUint32(r[0:4], gateRequestMagic+1) },
		"wire version":   func(r []byte) { binary.LittleEndian.PutUint32(r[4:8], wireVersion+1) },
		"reserved bytes": func(r []byte) { binary.LittleEndian.PutUint16(r[18:20], 1) },
		"label length":   func(r []byte) { binary.LittleEndian.PutUint16(r[16:18], relTypeMax+1) },
	}
	for name, corrupt := range malformed {
		request := gateRequest(NodePerson, "works_for", NodeOrg)
		corrupt(request)
		if _, status := Handle(bus.ModuleInvocation{StageID: StageWrite}, request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s status = %d", name, status)
		}
	}
	// A rerank-shaped request routed to the write stage must not be misparsed.
	if _, status := Handle(bus.ModuleInvocation{StageID: StageWrite},
		memoryRequest(660000)); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("rerank request on write stage status = %d", status)
	}
	if _, status := Handle(bus.ModuleInvocation{StageID: StageWrite},
		valid[:len(valid)-1]); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("short gate request status = %d", status)
	}
}

func TestWriteStageHonorsCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageWrite, DeadlineNS: 1}
	if _, status := Handle(invocation, gateRequest(NodePerson, "works_for", NodeOrg)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
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
