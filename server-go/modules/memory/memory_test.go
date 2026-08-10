package memory

import (
	"encoding/binary"
	"strings"
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

// piiRequest mirrors aimee_memory_pii_request_encode byte for byte.
func piiRequest(turnText string) []byte {
	request := make([]byte, piiRequestHeaderLen, piiRequestHeaderLen+len(turnText))
	binary.LittleEndian.PutUint32(request[0:4], piiRequestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(turnText)))
	return append(request, turnText...)
}

// The whole turn corpus crosses the wire. A stage that inverted the answer, or
// one that ignored the text, would agree with a handful of hand-picked turns.
func TestRetrieveStageCarriesTheTurnVerdict(t *testing.T) {
	asked, notAsked := 0, 0
	for _, row := range fixtureRows(t, "testdata/pii_turns.tsv") {
		text := unescapeField(row[0])
		want := atoi(t, row[1], row)

		response, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve}, piiRequest(text))
		if status != bus.ModuleStatusOK || len(response) != piiResponseLen ||
			binary.LittleEndian.Uint32(response[0:4]) != piiResponseMagic {
			t.Fatalf("pii(%q) response = %x, status = %d", text, response, status)
		}
		if got := int(binary.LittleEndian.Uint32(response[4:8])); got != want {
			t.Fatalf("pii(%q) = %d over the wire, C gate = %d", text, got, want)
		}
		if want == 1 {
			asked++
		} else {
			notAsked++
		}
	}
	if asked == 0 || notAsked == 0 {
		t.Errorf("only one answer crossed the wire: asked=%d not_asked=%d", asked, notAsked)
	}
}

func TestRetrieveStageRejectsMalformedRequests(t *testing.T) {
	valid := piiRequest("what is my email")
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve}, valid); status != bus.ModuleStatusOK {
		t.Fatalf("valid pii request status = %d", status)
	}
	malformed := map[string][]byte{
		"wire magic":      piiRequest("what is my email"),
		"wire version":    piiRequest("what is my email"),
		"declared length": piiRequest("what is my email"),
		"short header":    valid[:piiRequestHeaderLen-1],
		"truncated body":  valid[:len(valid)-1],
	}
	binary.LittleEndian.PutUint32(malformed["wire magic"][0:4], piiRequestMagic+1)
	binary.LittleEndian.PutUint32(malformed["wire version"][4:8], wireVersion+1)
	binary.LittleEndian.PutUint32(malformed["declared length"][8:12], 999)
	for name, request := range malformed {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve},
			request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s status = %d", name, status)
		}
	}
	// An extract-shaped request routed here must not be misparsed: both start
	// with a magic and a version, and both carry length-prefixed text.
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve},
		extractRequest("what is my email", 16)); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("extract request on retrieve stage status = %d", status)
	}
}

func TestRetrieveStageHonorsCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageRetrieve, DeadlineNS: 1}
	if _, status := Handle(invocation, piiRequest("what is my email")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}

// sensRequest mirrors aimee_memory_sens_request_encode byte for byte.
func sensRequest(relTypes []string) []byte {
	request := make([]byte, sensRequestHeaderLen)
	binary.LittleEndian.PutUint32(request[0:4], sensRequestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(relTypes)))
	for _, rel := range relTypes {
		var length [2]byte
		binary.LittleEndian.PutUint16(length[:], uint16(len(rel)))
		request = append(request, length[:]...)
		request = append(request, rel...)
	}
	return request
}

// Every label the C classifier was asked about goes over the wire in one batch,
// and each tier has to land against its own label. A stage that returned the
// tiers in the wrong order, or dropped one, would still look right on a batch
// where the labels happen to share a tier -- so the batch is the whole corpus.
func TestRetrieveStageClassifiesAWholeBatch(t *testing.T) {
	rows := fixtureRows(t, "testdata/pii_sensitivity.tsv")
	labels := make([]string, 0, len(rows))
	want := make([]RelSensitivity, 0, len(rows))
	for _, row := range rows {
		labels = append(labels, unescapeField(row[0]))
		want = append(want, RelSensitivity(atoi(t, row[1], row)))
	}

	response, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve}, sensRequest(labels))
	if status != bus.ModuleStatusOK {
		t.Fatalf("batch status = %d", status)
	}
	if len(response) != sensResponseHeaderLen+len(labels) ||
		binary.LittleEndian.Uint32(response[0:4]) != sensResponseMagic ||
		int(binary.LittleEndian.Uint32(response[4:8])) != len(labels) {
		t.Fatalf("malformed batch response header %x", response[:8])
	}
	for i, label := range labels {
		if got := RelSensitivity(response[sensResponseHeaderLen+i]); got != want[i] {
			t.Fatalf("batch[%d] (%q) = %d over the wire, C gate = %d", i, label, got, want[i])
		}
	}

	// A batch of one, and a batch whose tiers differ from their neighbours', are
	// where an off-by-one in the walk shows up.
	single, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve},
		sensRequest([]string{"home_password"}))
	if status != bus.ModuleStatusOK || len(single) != sensResponseHeaderLen+1 ||
		RelSensitivity(single[sensResponseHeaderLen]) != SensSecret {
		t.Fatalf("single-label batch = %x, status = %d", single, status)
	}
	mixed, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve},
		sensRequest([]string{"works_for", "ssn", "api_key"}))
	if status != bus.ModuleStatusOK || len(mixed) != sensResponseHeaderLen+3 {
		t.Fatalf("mixed batch = %x, status = %d", mixed, status)
	}
	for i, tier := range []RelSensitivity{SensNormal, SensPII, SensSecret} {
		if got := RelSensitivity(mixed[sensResponseHeaderLen+i]); got != tier {
			t.Fatalf("mixed batch[%d] = %d, want %d", i, got, tier)
		}
	}
}

func TestRetrieveStageRejectsMalformedBatches(t *testing.T) {
	valid := sensRequest([]string{"works_for", "ssn"})
	if _, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve}, valid); status != bus.ModuleStatusOK {
		t.Fatalf("valid batch status = %d", status)
	}
	malformed := map[string][]byte{
		"wire version":               sensRequest([]string{"works_for"}),
		"count of zero":              sensRequest(nil),
		"count larger than the body": sensRequest([]string{"works_for"}),
		"label length past the body": sensRequest([]string{"works_for"}),
		"label over the bound":       sensRequest([]string{strings.Repeat("a", relTypeMax+1)}),
		"trailing bytes":             append(sensRequest([]string{"works_for"}), 0),
		"short header":               valid[:sensRequestHeaderLen-1],
	}
	binary.LittleEndian.PutUint32(malformed["wire version"][4:8], wireVersion+1)
	binary.LittleEndian.PutUint32(malformed["count larger than the body"][8:12], 9)
	binary.LittleEndian.PutUint16(malformed["label length past the body"][12:14], 999)
	for name, request := range malformed {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageRetrieve},
			request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s status = %d", name, status)
		}
	}
}

func TestRetrieveStageBatchHonorsCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageRetrieve, DeadlineNS: 1}
	if _, status := Handle(invocation, sensRequest([]string{"works_for"})); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}

// scanRequest mirrors aimee_memory_scan_request_encode byte for byte.
func scanRequest(text string) []byte {
	request := make([]byte, scanRequestHeaderLen, scanRequestHeaderLen+len(text))
	binary.LittleEndian.PutUint32(request[0:4], scanRequestMagic)
	binary.LittleEndian.PutUint32(request[4:8], wireVersion)
	binary.LittleEndian.PutUint32(request[8:12], uint32(len(text)))
	return append(request, text...)
}

// The whole corpus crosses the wire, with both answers checked per turn: a
// stage that swapped the two flags would agree on every turn where they happen
// to coincide, and most turns in the corpus have a possessive.
func TestExtractStageCarriesTheTurnScan(t *testing.T) {
	retractions, possessives, neither := 0, 0, 0
	for _, row := range fixtureRows(t, "testdata/extract_corpus.tsv") {
		text := unescapeField(row[0])
		wantRetraction := atoi(t, row[1], row)
		wantHasAttr := atoi(t, row[2], row)
		wantAttr := unescapeField(row[3])

		response, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex},
			scanRequest(text))
		if status != bus.ModuleStatusOK || len(response) < scanResponseHeaderLen ||
			binary.LittleEndian.Uint32(response[0:4]) != scanResponseMagic {
			t.Fatalf("scan(%q) response = %x, status = %d", text, response, status)
		}
		gotRetraction := int(binary.LittleEndian.Uint32(response[4:8]))
		gotHasAttr := int(binary.LittleEndian.Uint32(response[8:12]))
		attrLen := int(binary.LittleEndian.Uint32(response[12:16]))
		if len(response) != scanResponseHeaderLen+attrLen {
			t.Fatalf("scan(%q) declares %d attr bytes in a %d-byte response", text, attrLen,
				len(response))
		}
		gotAttr := string(response[scanResponseHeaderLen:])
		if gotRetraction != wantRetraction || gotHasAttr != wantHasAttr || gotAttr != wantAttr {
			t.Fatalf("scan(%q) = (%d, %d, %q) over the wire, C = (%d, %d, %q)", text,
				gotRetraction, gotHasAttr, gotAttr, wantRetraction, wantHasAttr, wantAttr)
		}
		// The flag and the attribute have to agree, or the C decoder refuses.
		if (gotHasAttr == 0) != (attrLen == 0) {
			t.Fatalf("scan(%q) has_attr=%d with %d attr bytes", text, gotHasAttr, attrLen)
		}
		switch {
		case wantRetraction == 1:
			retractions++
		case wantHasAttr == 1:
			possessives++
		default:
			neither++
		}
	}
	// A corpus that never retracted, or always did, would agree with a stage that
	// ignored the text.
	if retractions == 0 || possessives == 0 || neither == 0 {
		t.Errorf("corpus is one-sided: retractions=%d possessive_only=%d neither=%d",
			retractions, possessives, neither)
	}
}

func TestExtractStageRejectsMalformedScans(t *testing.T) {
	valid := scanRequest("forget my email")
	if _, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex}, valid); status != bus.ModuleStatusOK {
		t.Fatalf("valid scan status = %d", status)
	}
	malformed := map[string][]byte{
		"wire version":    scanRequest("forget my email"),
		"declared length": scanRequest("forget my email"),
		"short header":    valid[:scanRequestHeaderLen-1],
		"truncated body":  valid[:len(valid)-1],
	}
	binary.LittleEndian.PutUint32(malformed["wire version"][4:8], wireVersion+1)
	binary.LittleEndian.PutUint32(malformed["declared length"][8:12], 999)
	for name, request := range malformed {
		if _, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex},
			request); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("%s status = %d", name, status)
		}
	}
	// An extract request and a scan request are the same shape but for the magic;
	// each stage half must refuse the other's.
	response, status := Handle(bus.ModuleInvocation{StageID: StageExtractIndex},
		extractRequest("forget my email", 16))
	if status != bus.ModuleStatusOK ||
		binary.LittleEndian.Uint32(response[0:4]) != extractResponseMagic {
		t.Errorf("extract request answered with %x, status = %d", response[:4], status)
	}
}

func TestExtractStageScanHonorsCancellation(t *testing.T) {
	invocation := bus.ModuleInvocation{StageID: StageExtractIndex, DeadlineNS: 1}
	if _, status := Handle(invocation, scanRequest("forget my email")); status != bus.ModuleStatusCancelled {
		t.Fatalf("expired invocation status = %d", status)
	}
}
