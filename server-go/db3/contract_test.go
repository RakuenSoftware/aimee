package db3

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"os"
	"reflect"
	"strings"
	"testing"
)

type wireBaseline struct {
	ContractSHA256   string `json:"contract_sha256"`
	ProtocolID       uint32 `json:"protocol_id"`
	SearchRequestHex string `json:"search_request_hex"`
	SearchReplyHex   string `json:"search_reply_hex"`
	ApplyHex         string `json:"apply_hex"`
	Events           []struct {
		ID        uint32 `json:"id"`
		Name      string `json:"name"`
		EventKind uint32 `json:"event_kind"`
		Pattern   string `json:"pattern"`
		Source    string `json:"source"`
		Delivery  string `json:"delivery"`
	} `json:"events"`
}

func loadBaseline(t *testing.T) wireBaseline {
	t.Helper()
	raw, err := os.ReadFile("../../tests/baselines/modules/db3-wire-v1.json")
	if err != nil {
		t.Fatalf("read DB3 C/Go baseline: %v", err)
	}
	var baseline wireBaseline
	if err := json.Unmarshal(raw, &baseline); err != nil {
		t.Fatalf("decode DB3 C/Go baseline: %v", err)
	}
	return baseline
}

func fixtureBytes(t *testing.T, value string) []byte {
	t.Helper()
	decoded, err := hex.DecodeString(value)
	if err != nil {
		t.Fatalf("decode fixture: %v", err)
	}
	return decoded
}

func fixtureRequest() SearchRequest {
	return SearchRequest{
		RequestID: 77, RequiredGeneration: 7, Workspace: "workspace-a", Project: "project-a",
		RecordType: "memory", TopK: 2, Vector: []float32{0.3, 0.2, 0.1},
	}
}

func fixtureReply() SearchReply {
	return SearchReply{RequestID: 77, Generation: 7, Candidates: []Candidate{{41, 0.95}, {42, 0.75}}}
}

func fixtureApply() Apply {
	return Apply{OperationID: 1001, Generation: 7, PointID: 41, Kind: ApplyUpsert,
		Collection: "memory", Vector: []float32{0.1, 0.2, 0.3}}
}

func TestGeneratedIdentityAndProviderSemantics(t *testing.T) {
	baseline := loadBaseline(t)
	if baseline.ContractSHA256 != ContractSHA256 || baseline.ProtocolID != ProtocolID {
		t.Fatalf("baseline identity = (%q,%d), generated = (%q,%d)",
			baseline.ContractSHA256, baseline.ProtocolID, ContractSHA256, ProtocolID)
	}
	want := []uint32{EventCapabilities, EventApply, EventApplied, EventSearch, EventRoute}
	if len(baseline.Events) != len(want) {
		t.Fatalf("events = %d, want %d", len(baseline.Events), len(want))
	}
	seen := map[uint32]bool{}
	for index, event := range baseline.Events {
		if event.ID != uint32(index+1) || event.EventKind != want[index] || event.EventKind&0x80000000 == 0 {
			t.Fatalf("event[%d] = %+v, generated kind %#x", index, event, want[index])
		}
		if seen[event.EventKind] {
			t.Fatalf("duplicate event kind %#x", event.EventKind)
		}
		seen[event.EventKind] = true
	}
	if baseline.Events[1].Pattern != "notification" || baseline.Events[1].Delivery != "all-providers" ||
		baseline.Events[3].Pattern != "request-reply" || baseline.Events[3].Delivery != "selected-provider" {
		t.Fatalf("APPLY/SEARCH semantics drifted: %+v / %+v", baseline.Events[1], baseline.Events[3])
	}
}

func TestSearchRequestCGoReplay(t *testing.T) {
	want := fixtureBytes(t, loadBaseline(t).SearchRequestHex)
	got, err := EncodeSearchRequest(fixtureRequest())
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("encode = (%x,%v), want %x", got, err, want)
	}
	decoded, err := DecodeSearchRequest(want)
	if err != nil || !reflect.DeepEqual(decoded, fixtureRequest()) {
		t.Fatalf("decode = (%+v,%v)", decoded, err)
	}
}

func TestSearchReplyCGoReplay(t *testing.T) {
	want := fixtureBytes(t, loadBaseline(t).SearchReplyHex)
	got, err := EncodeSearchReply(fixtureReply())
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("encode = (%x,%v), want %x", got, err, want)
	}
	decoded, err := DecodeSearchReply(want)
	if err != nil || !reflect.DeepEqual(decoded, fixtureReply()) {
		t.Fatalf("decode = (%+v,%v)", decoded, err)
	}
	if err := ValidateSearchReply(fixtureRequest(), decoded); err != nil {
		t.Fatalf("request-bound validation: %v", err)
	}
}

func TestApplyCGoReplay(t *testing.T) {
	want := fixtureBytes(t, loadBaseline(t).ApplyHex)
	got, err := EncodeApply(fixtureApply())
	if err != nil || !reflect.DeepEqual(got, want) {
		t.Fatalf("encode = (%x,%v), want %x", got, err, want)
	}
	decoded, err := DecodeApply(want)
	if err != nil || !reflect.DeepEqual(decoded, fixtureApply()) {
		t.Fatalf("decode = (%+v,%v)", decoded, err)
	}
}

func requireMalformed(t *testing.T, err error) {
	t.Helper()
	if !errors.Is(err, ErrMalformed) {
		t.Fatalf("error = %v, want ErrMalformed", err)
	}
}

func TestSearchRequestRejectsMalformedFramesAndValues(t *testing.T) {
	valid, _ := EncodeSearchRequest(fixtureRequest())
	mutations := map[string]func([]byte) []byte{
		"bad-magic":       func(v []byte) []byte { v[0] ^= 1; return v },
		"bad-version":     func(v []byte) []byte { v[4] ^= 1; return v },
		"bad-header":      func(v []byte) []byte { v[6] ^= 1; return v },
		"reserved":        func(v []byte) []byte { v[34] = 1; return v },
		"zero-request":    func(v []byte) []byte { clear(v[8:16]); return v },
		"zero-generation": func(v []byte) []byte { clear(v[16:24]); return v },
		"zero-top-k":      func(v []byte) []byte { clear(v[32:34]); return v },
		"bad-ascii":       func(v []byte) []byte { v[searchRequestHeader] = ' '; return v },
		"nan-vector": func(v []byte) []byte {
			binary.LittleEndian.PutUint32(v[len(v)-4:], math.Float32bits(float32(math.NaN())))
			return v
		},
		"short": func(v []byte) []byte { return v[:len(v)-1] },
		"long":  func(v []byte) []byte { return append(v, 0) },
	}
	for name, mutate := range mutations {
		t.Run(name, func(t *testing.T) {
			copyOf := append([]byte(nil), valid...)
			_, err := DecodeSearchRequest(mutate(copyOf))
			requireMalformed(t, err)
		})
	}
	invalid := []SearchRequest{
		{},
		{RequestID: 1, RequiredGeneration: 1, Project: "p", RecordType: "r", TopK: 1,
			Vector: []float32{float32(math.Inf(1))}},
		{RequestID: 1, RequiredGeneration: 1, Project: "p", RecordType: "r", TopK: MaxTopK + 1,
			Vector: []float32{1}},
		{RequestID: 1, RequiredGeneration: 1, Workspace: strings.Repeat("x", MaxScopeBytes),
			RecordType: "r", TopK: 1, Vector: []float32{1}},
	}
	for index, request := range invalid {
		if _, err := EncodeSearchRequest(request); !errors.Is(err, ErrMalformed) {
			t.Errorf("invalid[%d] error = %v", index, err)
		}
	}
}

func TestSearchReplyRejectsMalformedAndUnboundResponses(t *testing.T) {
	valid, _ := EncodeSearchReply(fixtureReply())
	mutations := map[string]func([]byte) []byte{
		"bad-magic":    func(v []byte) []byte { v[0] ^= 1; return v },
		"bad-version":  func(v []byte) []byte { v[4] ^= 1; return v },
		"bad-header":   func(v []byte) []byte { v[6] ^= 1; return v },
		"zero-request": func(v []byte) []byte { clear(v[8:16]); return v },
		"zero-point":   func(v []byte) []byte { clear(v[searchReplyHeader : searchReplyHeader+8]); return v },
		"duplicate-point": func(v []byte) []byte {
			copy(v[searchReplyHeader+candidateBytes:searchReplyHeader+candidateBytes+8], v[searchReplyHeader:searchReplyHeader+8])
			return v
		},
		"nan-score": func(v []byte) []byte {
			binary.LittleEndian.PutUint64(v[searchReplyHeader+8:searchReplyHeader+16], math.Float64bits(math.NaN()))
			return v
		},
		"short": func(v []byte) []byte { return v[:len(v)-1] },
		"long":  func(v []byte) []byte { return append(v, 0) },
	}
	for name, mutate := range mutations {
		t.Run(name, func(t *testing.T) {
			_, err := DecodeSearchReply(mutate(append([]byte(nil), valid...)))
			requireMalformed(t, err)
		})
	}
	request := fixtureRequest()
	for name, reply := range map[string]SearchReply{
		"request":    {RequestID: 78, Generation: 7},
		"generation": {RequestID: 77, Generation: 8},
		"top-k":      {RequestID: 77, Generation: 7, Candidates: []Candidate{{1, .9}, {2, .8}, {3, .7}}},
	} {
		t.Run(name, func(t *testing.T) { requireMalformed(t, ValidateSearchReply(request, reply)) })
	}
}

func TestApplyRejectsMalformedFramesAndSemantics(t *testing.T) {
	valid, _ := EncodeApply(fixtureApply())
	mutations := map[string]func([]byte) []byte{
		"bad-magic":      func(v []byte) []byte { v[0] ^= 1; return v },
		"bad-version":    func(v []byte) []byte { v[4] ^= 1; return v },
		"reserved":       func(v []byte) []byte { v[7] = 1; return v },
		"bad-kind":       func(v []byte) []byte { v[6] = 9; return v },
		"zero-operation": func(v []byte) []byte { clear(v[8:16]); return v },
		"zero-point":     func(v []byte) []byte { clear(v[24:32]); return v },
		"bad-ascii":      func(v []byte) []byte { v[applyHeader] = ' '; return v },
		"nan-vector": func(v []byte) []byte {
			binary.LittleEndian.PutUint32(v[len(v)-4:], math.Float32bits(float32(math.NaN())))
			return v
		},
		"short": func(v []byte) []byte { return v[:len(v)-1] },
		"long":  func(v []byte) []byte { return append(v, 0) },
	}
	for name, mutate := range mutations {
		t.Run(name, func(t *testing.T) {
			_, err := DecodeApply(mutate(append([]byte(nil), valid...)))
			requireMalformed(t, err)
		})
	}
	deleteWithVector := fixtureApply()
	deleteWithVector.Kind = ApplyDelete
	if _, err := EncodeApply(deleteWithVector); !errors.Is(err, ErrMalformed) {
		t.Fatalf("delete with vector error = %v", err)
	}
	deleteApply := fixtureApply()
	deleteApply.Kind, deleteApply.Vector = ApplyDelete, nil
	wire, err := EncodeApply(deleteApply)
	if err != nil {
		t.Fatalf("encode delete: %v", err)
	}
	decoded, err := DecodeApply(wire)
	if err != nil || !reflect.DeepEqual(decoded, deleteApply) {
		t.Fatalf("delete round trip = (%+v,%v)", decoded, err)
	}
}

func TestBoundarySizedFramesRoundTrip(t *testing.T) {
	request := SearchRequest{RequestID: 1, RequiredGeneration: 1,
		Workspace: strings.Repeat("w", MaxScopeBytes-1), RecordType: strings.Repeat("r", MaxRecordTypeBytes-1),
		TopK: MaxTopK, Vector: make([]float32, MaxDimension)}
	wire, err := EncodeSearchRequest(request)
	if err != nil {
		t.Fatalf("encode max request: %v", err)
	}
	decoded, err := DecodeSearchRequest(wire)
	if err != nil || !reflect.DeepEqual(decoded, request) {
		t.Fatalf("max request round trip = (%+v,%v)", decoded, err)
	}
}
