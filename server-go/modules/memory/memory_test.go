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

// extractRequest mirrors aimee_memory_extract_request_encode byte for byte; the
// two encoders are the wire contract, so a drift in either must show up here.
func extractRequest(text string, max uint32) []byte {
	request := make([]byte, extractRequestHeaderLen, extractRequestHeaderLen+len(text))
	binary.LittleEndian.PutUint32(request[0:4], extractRequestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], max)
	binary.LittleEndian.PutUint32(request[12:16], uint32(len(text)))
	return append(request, text...)
}

// decodeExtractResponse mirrors aimee_memory_extract_response_decode, including
// its refusal of trailing bytes: a response the C would reject must not be read
// as agreeing here.
func decodeExtractResponse(t *testing.T, response []byte) []Triple {
	t.Helper()
	if len(response) < extractResponseHeaderLen ||
		binary.LittleEndian.Uint32(response[0:4]) != extractResponseMagic {
		t.Fatalf("malformed response header %x", response)
	}
	count := int(binary.LittleEndian.Uint32(response[4:8]))
	offset := extractResponseHeaderLen
	field := func(cap int) string {
		if offset+4 > len(response) {
			t.Fatalf("truncated field length at %d", offset)
		}
		length := int(binary.LittleEndian.Uint32(response[offset : offset+4]))
		if length >= cap || offset+4+length > len(response) {
			t.Fatalf("field of %d bytes does not fit a %d-byte buffer", length, cap)
		}
		value := string(response[offset+4 : offset+4+length])
		offset += 4 + length
		return value
	}
	triples := make([]Triple, 0, count)
	for i := 0; i < count; i++ {
		if offset+8 > len(response) {
			t.Fatalf("truncated kinds at %d", offset)
		}
		subjectKind := NodeKind(binary.LittleEndian.Uint32(response[offset : offset+4]))
		objectKind := NodeKind(binary.LittleEndian.Uint32(response[offset+4 : offset+8]))
		offset += 8
		triples = append(triples, Triple{
			Subject:     field(tripleSubjectMax),
			RelType:     field(tripleRelTypeMax),
			Object:      field(tripleObjectMax),
			SubjectKind: subjectKind,
			ObjectKind:  objectKind,
		})
	}
	if offset != len(response) {
		t.Fatalf("%d trailing bytes; the C decoder would refuse this response",
			len(response)-offset)
	}
	return triples
}

// The whole corpus goes over the wire, not a handful of hand-picked sentences:
// a stage that swapped the subject and object kinds, or the rel_type and object
// fields, would still agree on the many rows where those happen to match.
func TestExtractStageCarriesEveryTriple(t *testing.T) {
	const max = 16 // the bound the corpus was generated under
	rows := fixtureRows(t, "testdata/extract_corpus.tsv")
	carried, withTriples := 0, 0
	for _, row := range rows {
		text := unescapeField(row[0])
		want := ExtractPatterns(text, max)

		response, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex},
			extractRequest(text, max))
		if status != bus.ModuleStatusOK {
			t.Fatalf("extract(%q) status = %d", text, status)
		}
		got := decodeExtractResponse(t, response)
		if len(got) != len(want) {
			t.Fatalf("extract(%q) carried %d triples, want %d", text, len(got), len(want))
		}
		for i := range want {
			if got[i] != want[i] {
				t.Fatalf("extract(%q)[%d] = %+v over the wire, want %+v", text, i, got[i], want[i])
			}
		}
		carried += len(got)
		if len(got) > 0 {
			withTriples++
		}
	}
	if withTriples == 0 {
		t.Fatal("no text in the corpus produced a triple; the wire is untested")
	}
	t.Logf("carried %d triples across %d texts (%d non-empty)", carried, len(rows), withTriples)
}

func TestExtractStageRejectsMalformedRequests(t *testing.T) {
	valid := extractRequest("my name is Theo", 16)
	if _, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex}, valid); status != bus.ModuleStatusOK {
		t.Fatalf("valid extract request status = %d", status)
	}
	malformed := map[string][]byte{
		"wire magic":                    extractRequest("my name is Theo", 16),
		"wire version":                  extractRequest("my name is Theo", 16),
		"zero max":                      extractRequest("my name is Theo", 0),
		"short header":                  valid[:extractRequestHeaderLen-1],
		"text longer than the request":  append([]byte{}, valid...),
		"text shorter than the request": valid[:len(valid)-1],
	}
	binary.LittleEndian.PutUint32(malformed["wire magic"][0:4], extractRequestMagic+1)
	binary.LittleEndian.PutUint32(malformed["wire version"][4:8], wireVersion+1)
	binary.LittleEndian.PutUint32(malformed["text longer than the request"][12:16], 999)
	for name, request := range malformed {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex},
			request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s status = %d", name, status)
		}
	}
	// A gate-shaped request routed to the extract stage must not be misparsed.
	if _, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex},
		gateRequest(NodePerson, "works_for", NodeOrg)); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("gate request on extract stage status = %d", status)
	}
}

func TestExtractStageHonorsCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageExtractIndex, DeadlineNS: 1}
	if _, status := Handle(invocation, extractRequest("my name is Theo", 16)); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}

// An empty text is a real turn shape (an empty memory row), not a malformed
// request: the stage answers "no facts", which is different from failing.
func TestExtractStageAcceptsEmptyText(t *testing.T) {
	response, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex},
		extractRequest("", 16))
	if status != bus.ModuleStatusOK {
		t.Fatalf("empty text status = %d", status)
	}
	if triples := decodeExtractResponse(t, response); len(triples) != 0 {
		t.Fatalf("empty text yielded %d triples", len(triples))
	}
}
