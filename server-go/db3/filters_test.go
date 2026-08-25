package db3

import (
	"encoding/hex"
	"testing"
)

func filterSet() []ExactLabel {
	// Sorted by key, which the codec requires so one filter set has exactly one
	// encoding.
	return []ExactLabel{
		{Key: "project", Value: "project-a"},
		{Key: "record_type", Value: "memory"},
	}
}

func filteredRequest() SearchRequest {
	return SearchRequest{
		RequestID:          77,
		RequiredGeneration: 7,
		Workspace:          "workspace-a",
		Project:            "project-a",
		RecordType:         "memory",
		TopK:               2,
		Vector:             []float32{.3, .2, .1},
		Filters:            filterSet(),
	}
}

func TestFilteredRequestRoundTrips(t *testing.T) {
	encoded, err := EncodeSearchRequest(filteredRequest())
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	decoded, err := DecodeSearchRequest(encoded)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(decoded.Filters) != 2 ||
		decoded.Filters[0] != (ExactLabel{Key: "project", Value: "project-a"}) ||
		decoded.Filters[1] != (ExactLabel{Key: "record_type", Value: "memory"}) {
		t.Fatalf("filters = %v", decoded.Filters)
	}
	if decoded.Workspace != "workspace-a" || decoded.TopK != 2 || len(decoded.Vector) != 3 {
		t.Errorf("the rest of the request did not survive: %+v", decoded)
	}
}

// A request with no filters must encode exactly as v1 does. That is what lets a
// v1 peer keep reading traffic from a v2 sender unchanged.
func TestUnfilteredRequestStaysV1OnTheWire(t *testing.T) {
	request := filteredRequest()
	request.Filters = nil

	encoded, err := EncodeSearchRequest(request)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if got := int(encoded[4]) | int(encoded[5])<<8; got != int(WireVersion) {
		t.Errorf("version = %d, want v1 %d", got, WireVersion)
	}
	if got := int(encoded[6]) | int(encoded[7])<<8; got != searchRequestHeader {
		t.Errorf("header = %d, want v1 %d", got, searchRequestHeader)
	}

	baseline := loadBaseline(t)
	want, err := hex.DecodeString(baseline.SearchRequestHex)
	if err != nil {
		t.Fatalf("baseline hex: %v", err)
	}
	if string(encoded) != string(want) {
		t.Fatalf("unfiltered encoding drifted from the frozen v1 vector")
	}
}

// The frozen v2 vector is the contract. A codec change that alters these bytes
// changes what every existing peer reads, so it has to fail here first.
func TestFilteredRequestMatchesTheFrozenVector(t *testing.T) {
	baseline := loadBaseline(t)
	want, err := hex.DecodeString(baseline.SearchRequestV2Hex)
	if err != nil {
		t.Fatalf("baseline hex: %v", err)
	}
	encoded, err := EncodeSearchRequest(filteredRequest())
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if string(encoded) != string(want) {
		t.Fatalf("v2 encoding drifted from the frozen vector\n got %x\nwant %x", encoded, want)
	}
	decoded, err := DecodeSearchRequest(want)
	if err != nil {
		t.Fatalf("the frozen vector must decode: %v", err)
	}
	if len(decoded.Filters) != 2 {
		t.Errorf("filters = %v", decoded.Filters)
	}
}

// Unsorted or duplicated keys are refused, because either would give one filter
// set two encodings and let two peers disagree about whether they match.
func TestFiltersMustBeSortedAndUnique(t *testing.T) {
	for name, filters := range map[string][]ExactLabel{
		"unsorted":  {{Key: "record_type", Value: "memory"}, {Key: "project", Value: "p"}},
		"duplicate": {{Key: "project", Value: "a"}, {Key: "project", Value: "b"}},
	} {
		request := filteredRequest()
		request.Filters = filters
		if _, err := EncodeSearchRequest(request); err == nil {
			t.Errorf("%s filters were accepted", name)
		}
	}
}

// A v2 frame carrying no filters is a second encoding of a v1 request. Two
// encodings of one value defeat the point of a canonical wire.
func TestV2FrameWithNoFiltersIsRefused(t *testing.T) {
	encoded, err := EncodeSearchRequest(filteredRequest())
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	// Strip the filters and zero their counts, leaving a well-formed v2 header.
	filtersBytes := int(encoded[38]) | int(encoded[39])<<8
	stripped := append([]byte{}, encoded[:len(encoded)-filtersBytes]...)
	stripped[36], stripped[37] = 0, 0
	stripped[38], stripped[39] = 0, 0
	if _, err := DecodeSearchRequest(stripped); err == nil {
		t.Fatal("a v2 frame with no filters must be refused")
	}
}

// A frame claiming one version while carrying the other's header length is how
// a reader gets walked off the end of a buffer.
func TestVersionAndHeaderMustAgree(t *testing.T) {
	encoded, err := EncodeSearchRequest(filteredRequest())
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	mismatched := append([]byte{}, encoded...)
	mismatched[4], mismatched[5] = byte(WireVersion), 0 // claim v1, keep the v2 header
	if _, err := DecodeSearchRequest(mismatched); err == nil {
		t.Fatal("a version/header mismatch must be refused")
	}

	unknown := append([]byte{}, encoded...)
	unknown[4], unknown[5] = 99, 0
	if _, err := DecodeSearchRequest(unknown); err == nil {
		t.Fatal("an unknown version must be refused")
	}
}

// A declared filter length that does not match what the filters consume would
// let a frame hide trailing bytes.
func TestDeclaredFilterLengthMustMatch(t *testing.T) {
	encoded, err := EncodeSearchRequest(filteredRequest())
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	tampered := append([]byte{}, encoded...)
	tampered[38] = byte(int(tampered[38]) - 1)
	if _, err := DecodeSearchRequest(tampered); err == nil {
		t.Fatal("a mismatched filter length must be refused")
	}
}

func TestTooManyFiltersRefused(t *testing.T) {
	request := filteredRequest()
	request.Filters = make([]ExactLabel, MaxLabelCount+1)
	for i := range request.Filters {
		request.Filters[i] = ExactLabel{Key: string(rune('a'+i)) + "key", Value: "v"}
	}
	if _, err := EncodeSearchRequest(request); err == nil {
		t.Fatal("more filters than the contract allows must be refused")
	}
}
