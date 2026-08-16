package db3

import (
	"encoding/binary"
	"errors"
	"reflect"
	"testing"
)

func fixtureCapabilities() Capabilities {
	return Capabilities{
		Generation: 7, Operations: OperationSearch | OperationApply, Metrics: MetricCosine,
		MaxDimension: MaxDimension, MaxBatch: 64, MaxTopK: MaxTopK, Ready: true,
	}
}

func TestControlFramesReplayGeneratedBaseline(t *testing.T) {
	baseline := loadBaseline(t)
	applyWire, err := EncodeApply(fixtureApply())
	if err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		name   string
		hex    string
		value  any
		encode func() ([]byte, error)
		decode func([]byte) (any, error)
	}{
		{"capabilities", baseline.CapabilitiesHex, fixtureCapabilities(),
			func() ([]byte, error) { return EncodeCapabilities(fixtureCapabilities()) },
			func(v []byte) (any, error) { return DecodeCapabilities(v) }},
		{"apply-chunk", baseline.ApplyChunkHex,
			ApplyChunk{OperationID: 1001, Total: uint32(len(applyWire)), Data: applyWire},
			func() ([]byte, error) {
				return EncodeApplyChunk(ApplyChunk{OperationID: 1001, Total: uint32(len(applyWire)), Data: applyWire})
			}, func(v []byte) (any, error) { return DecodeApplyChunk(v) }},
		{"applied", baseline.AppliedHex,
			Applied{OperationID: 1001, Generation: 7, Watermark: 7, Result: AppliedOK},
			func() ([]byte, error) {
				return EncodeApplied(Applied{OperationID: 1001, Generation: 7, Watermark: 7, Result: AppliedOK})
			}, func(v []byte) (any, error) { return DecodeApplied(v) }},
		{"search-failure", baseline.SearchFailureHex,
			SearchFailure{RequestID: 77, Code: SearchFailureUnavailable},
			func() ([]byte, error) {
				return EncodeSearchFailure(SearchFailure{RequestID: 77, Code: SearchFailureUnavailable})
			}, func(v []byte) (any, error) { return DecodeSearchFailure(v) }},
		{"route-request", baseline.RouteRequestHex,
			RouteRequest{RequestID: 91, Action: RouteSelect, Principal: 1001, CapabilityGeneration: 7, Fallback: true},
			func() ([]byte, error) {
				return EncodeRouteRequest(RouteRequest{RequestID: 91, Action: RouteSelect, Principal: 1001, CapabilityGeneration: 7, Fallback: true})
			}, func(v []byte) (any, error) { return DecodeRouteRequest(v) }},
		{"route-reply", baseline.RouteReplyHex,
			RouteReply{RequestID: 91, Result: RouteOK, SelectedPrincipal: 1001, ProviderGeneration: 7, Fallback: true},
			func() ([]byte, error) {
				return EncodeRouteReply(RouteReply{RequestID: 91, Result: RouteOK, SelectedPrincipal: 1001, ProviderGeneration: 7, Fallback: true})
			}, func(v []byte) (any, error) { return DecodeRouteReply(v) }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			want := fixtureBytes(t, test.hex)
			wire, err := test.encode()
			if err != nil || !reflect.DeepEqual(wire, want) {
				t.Fatalf("encode = (%x, %v), want %x", wire, err, want)
			}
			decoded, err := test.decode(want)
			if err != nil || !reflect.DeepEqual(decoded, test.value) {
				t.Fatalf("decode = (%#v, %v), want %#v", decoded, err, test.value)
			}
		})
	}
}

func TestControlFramesRejectNoncanonicalWire(t *testing.T) {
	valid, _ := EncodeCapabilities(fixtureCapabilities())
	for name, mutate := range map[string]func([]byte){
		"magic":      func(v []byte) { v[0] ^= 1 },
		"version":    func(v []byte) { v[4] ^= 1 },
		"header":     func(v []byte) { v[6] ^= 1 },
		"flags":      func(v []byte) { binary.LittleEndian.PutUint32(v[28:32], 2) },
		"operations": func(v []byte) { binary.LittleEndian.PutUint32(v[16:20], 0x80000000) },
		"reserved":   func(v []byte) { v[44] = 1 },
	} {
		t.Run(name, func(t *testing.T) {
			wire := append([]byte(nil), valid...)
			mutate(wire)
			_, err := DecodeCapabilities(wire)
			requireMalformed(t, err)
		})
	}
	for name, wire := range map[string][]byte{
		"short-capabilities":    valid[:len(valid)-1],
		"invalid-applied":       make([]byte, appliedHeader),
		"invalid-failure":       make([]byte, searchFailureHeader),
		"invalid-route-request": make([]byte, routeRequestHeader),
		"invalid-route-reply":   make([]byte, routeReplyHeader),
	} {
		t.Run(name, func(t *testing.T) {
			var err error
			switch name {
			case "short-capabilities":
				_, err = DecodeCapabilities(wire)
			case "invalid-applied":
				_, err = DecodeApplied(wire)
			case "invalid-failure":
				_, err = DecodeSearchFailure(wire)
			case "invalid-route-request":
				_, err = DecodeRouteRequest(wire)
			default:
				_, err = DecodeRouteReply(wire)
			}
			if !errors.Is(err, ErrMalformed) {
				t.Fatalf("error = %v", err)
			}
		})
	}
}

func TestControlSemanticValidation(t *testing.T) {
	invalidCapabilities := []Capabilities{
		{},
		{Operations: OperationSearch, Metrics: MetricCosine, MaxDimension: 1, MaxTopK: 1, Ready: true},
		{Operations: OperationSearch, MaxDimension: 1, MaxTopK: 1},
		{Operations: OperationApply},
		{Operations: OperationSet(1 << 31)},
		{Operations: OperationSearch, Metrics: MetricCosine, MaxDimension: MaxDimension + 1, MaxTopK: 1},
	}
	for i, value := range invalidCapabilities {
		if _, err := EncodeCapabilities(value); !errors.Is(err, ErrMalformed) {
			t.Errorf("capabilities[%d] error = %v", i, err)
		}
	}
	for i, value := range []RouteRequest{
		{},
		{RequestID: 1, Action: RouteSelect},
		{RequestID: 1, Action: RouteClear, Principal: 2},
		{RequestID: 1, Action: RouteQuery, Fallback: true},
	} {
		if _, err := EncodeRouteRequest(value); !errors.Is(err, ErrMalformed) {
			t.Errorf("route[%d] error = %v", i, err)
		}
	}
	if _, err := EncodeApplied(Applied{OperationID: 1, Generation: 7, Watermark: 6}); !errors.Is(err, ErrMalformed) {
		t.Fatalf("regressing watermark error = %v", err)
	}
	if _, err := EncodeRouteReply(RouteReply{RequestID: 1, SelectedPrincipal: 2}); !errors.Is(err, ErrMalformed) {
		t.Fatalf("selected route without generation error = %v", err)
	}
}

func TestApplyChunkBoundsAndCopiesInput(t *testing.T) {
	data := []byte{1, 2, 3}
	wire, err := EncodeApplyChunk(ApplyChunk{OperationID: 9, Total: 5, Offset: 2, Data: data})
	if err != nil {
		t.Fatal(err)
	}
	decoded, err := DecodeApplyChunk(wire)
	if err != nil || !reflect.DeepEqual(decoded.Data, data) {
		t.Fatalf("decode = (%+v, %v)", decoded, err)
	}
	wire[len(wire)-1] = 99
	if decoded.Data[2] != 3 {
		t.Fatal("decoded chunk aliases the bus frame")
	}
	for _, invalid := range []ApplyChunk{
		{},
		{OperationID: 1, Total: MaxEncodedApply + 1, Data: []byte{1}},
		{OperationID: 1, Total: 2, Offset: 2, Data: []byte{1}},
	} {
		if _, err := EncodeApplyChunk(invalid); !errors.Is(err, ErrMalformed) {
			t.Errorf("invalid chunk %+v: %v", invalid, err)
		}
	}
}
