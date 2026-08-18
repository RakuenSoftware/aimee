package db2

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"testing"
)

type wireBaseline struct {
	CatalogSHA256 string `json:"catalog_sha256"`
	WireVersion   uint32 `json:"wire_version"`
	BodyEnvelope  struct {
		HeaderLen uint32 `json:"header_len"`
		Request   struct {
			Positive string `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"request"`
		Reply struct {
			Positive []struct {
				Result uint32 `json:"result"`
				Hex    string `json:"hex"`
			} `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"reply"`
	} `json:"body_envelope"`
	Operations []struct {
		Name    string `json:"name"`
		Request struct {
			Positive string `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"request"`
		Reply struct {
			Positive []struct {
				Flags         uint32 `json:"flags"`
				Result        uint32 `json:"result"`
				Dimension     uint32 `json:"dimension"`
				Size          uint32 `json:"size"`
				InUse         uint32 `json:"in_use"`
				Waiters       uint32 `json:"waiters"`
				LeaseGrants   uint64 `json:"lease_grants"`
				LeaseTimeouts uint64 `json:"lease_timeouts"`
				Stuck         uint64 `json:"stuck"`
				Poisoned      uint64 `json:"poisoned"`
				Hex           string `json:"hex"`
			} `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"reply"`
	} `json:"operations"`
}

func TestBodyEnvelopeMatchesEverySharedCVector(t *testing.T) {
	envelope := loadWireBaseline(t).BodyEnvelope
	if envelope.HeaderLen != EnvelopeHeaderLen {
		t.Fatalf("header length = %d, generated Go = %d", envelope.HeaderLen, EnvelopeHeaderLen)
	}
	payload := []byte{0xaa, 0xbb, 0xcc}
	requestHeader, err := EncodeRequestHeader(0x01020304, 5, uint32(len(payload)))
	if err != nil {
		t.Fatalf("EncodeRequestHeader: %v", err)
	}
	request := append(requestHeader, payload...)
	wantRequest := decodeHex(t, envelope.Request.Positive)
	if string(request) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", request, wantRequest)
	}
	decodedRequest, err := DecodeRequestHeader(wantRequest)
	if err != nil || decodedRequest != (RequestHeader{
		Operation: 0x01020304, Flags: 5, PayloadLen: 3,
	}) {
		t.Fatalf("decoded request = (%+v, %v)", decodedRequest, err)
	}
	for _, vector := range envelope.Request.Negative {
		t.Run("request_"+vector.Mutation, func(t *testing.T) {
			header, err := DecodeRequestHeader(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || header != (RequestHeader{}) {
				t.Fatalf("request = (%+v, %v), want zero/malformed", header, err)
			}
		})
	}
	if header, err := EncodeRequestHeader(0, 0, 0); !errors.Is(err, ErrMalformedEnvelope) || header != nil {
		t.Fatalf("zero-operation request = (%x, %v)", header, err)
	}

	for _, vector := range envelope.Reply.Positive {
		vector := vector
		t.Run("reply_result_"+string(rune('0'+vector.Result)), func(t *testing.T) {
			header, err := EncodeReplyHeader(0x01020304, vector.Result, uint32(len(payload)))
			if err != nil {
				t.Fatalf("EncodeReplyHeader: %v", err)
			}
			reply := append(header, payload...)
			want := decodeHex(t, vector.Hex)
			if string(reply) != string(want) {
				t.Fatalf("reply = %x, want %x", reply, want)
			}
			decoded, err := DecodeReplyHeader(want)
			if err != nil || decoded != (ReplyHeader{
				Operation: 0x01020304, Result: vector.Result, PayloadLen: 3,
			}) {
				t.Fatalf("decoded reply = (%+v, %v)", decoded, err)
			}
		})
	}
	for _, vector := range envelope.Reply.Negative {
		t.Run("reply_"+vector.Mutation, func(t *testing.T) {
			header, err := DecodeReplyHeader(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || header != (ReplyHeader{}) {
				t.Fatalf("reply = (%+v, %v), want zero/malformed", header, err)
			}
		})
	}
	if header, err := EncodeReplyHeader(1, ResultInvalidState+1, 0); !errors.Is(err, ErrMalformedEnvelope) || header != nil {
		t.Fatalf("unknown-result reply = (%x, %v)", header, err)
	}
}

func decodeHex(t *testing.T, value string) []byte {
	t.Helper()
	decoded, err := hex.DecodeString(value)
	if err != nil {
		t.Fatalf("decode fixture %q: %v", value, err)
	}
	return decoded
}

func loadWireBaseline(t *testing.T) wireBaseline {
	t.Helper()
	raw, err := os.ReadFile("../../tests/baselines/modules/db2-wire-v1.json")
	if err != nil {
		t.Fatalf("read shared C/Go wire baseline: %v", err)
	}
	var baseline wireBaseline
	if err := json.Unmarshal(raw, &baseline); err != nil {
		t.Fatalf("decode shared C/Go wire baseline: %v", err)
	}
	if len(baseline.Operations) != 3 || baseline.Operations[0].Name != "health" ||
		baseline.Operations[1].Name != "embedding_dimension" ||
		baseline.Operations[2].Name != "pool_status" {
		t.Fatalf("unexpected operations: %+v", baseline.Operations)
	}
	return baseline
}

func TestPoolStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[2]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodePoolStatusRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodePoolStatusRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		t.Run("request_"+vector.Mutation, func(t *testing.T) {
			if err := DecodePoolStatusRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
				t.Fatalf("negative request error = %v", err)
			}
		})
	}
	for _, vector := range operation.Reply.Positive {
		vector := vector
		t.Run("reply_"+vector.Hex, func(t *testing.T) {
			status := PoolStatus{vector.Size, vector.InUse, vector.Waiters, vector.LeaseGrants,
				vector.LeaseTimeouts, vector.Stuck, vector.Poisoned}
			got, err := EncodePoolStatusReply(vector.Result, status)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			want := decodeHex(t, vector.Hex)
			if string(got) != string(want) {
				t.Fatalf("reply = %x, want %x", got, want)
			}
			result, decoded, err := DecodePoolStatusReply(want)
			if err != nil || result != vector.Result || decoded != status {
				t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
			}
		})
	}
	for _, vector := range operation.Reply.Negative {
		t.Run("reply_"+vector.Mutation, func(t *testing.T) {
			result, status, err := DecodePoolStatusReply(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (PoolStatus{}) {
				t.Fatalf("negative reply = (%d, %+v, %v)", result, status, err)
			}
		})
	}
}

func TestEmbeddingDimensionMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[1]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEmbeddingDimensionRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEmbeddingDimensionRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		t.Run("request_"+vector.Mutation, func(t *testing.T) {
			if err := DecodeEmbeddingDimensionRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
				t.Fatalf("negative request error = %v", err)
			}
		})
	}
	for _, vector := range operation.Reply.Positive {
		vector := vector
		t.Run("reply_"+vector.Hex, func(t *testing.T) {
			got, err := EncodeEmbeddingDimensionReply(vector.Result, vector.Dimension)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			want := decodeHex(t, vector.Hex)
			if string(got) != string(want) {
				t.Fatalf("reply = %x, want %x", got, want)
			}
			result, dimension, err := DecodeEmbeddingDimensionReply(want)
			if err != nil || result != vector.Result || dimension != vector.Dimension {
				t.Fatalf("decode = (%d, %d, %v)", result, dimension, err)
			}
		})
	}
	for _, vector := range operation.Reply.Negative {
		t.Run("reply_"+vector.Mutation, func(t *testing.T) {
			result, dimension, err := DecodeEmbeddingDimensionReply(decodeHex(t, vector.Hex))
			if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || dimension != 0 {
				t.Fatalf("negative reply = (%d, %d, %v)", result, dimension, err)
			}
		})
	}
}

func TestGeneratedIdentityMatchesSharedCatalog(t *testing.T) {
	baseline := loadWireBaseline(t)
	if baseline.CatalogSHA256 != ContractSHA256 {
		t.Fatalf("catalog fingerprint = %q, generated Go = %q", baseline.CatalogSHA256, ContractSHA256)
	}
	if baseline.WireVersion != WireVersion {
		t.Fatalf("wire version = %d, generated Go = %d", baseline.WireVersion, WireVersion)
	}
	if EventHealth != 11521 || StageHealth != 1 || OperationHealth != 1 {
		t.Fatalf("health identity = (%d, %d, %d)", EventHealth, StageHealth, OperationHealth)
	}
}

func TestHealthRequestMatchesEverySharedCVector(t *testing.T) {
	request := loadWireBaseline(t).Operations[0].Request
	want := decodeHex(t, request.Positive)
	if got := EncodeHealthRequest(); string(got) != string(want) {
		t.Fatalf("encoded request = %x, want %x", got, want)
	}
	if err := DecodeHealthRequest(want); err != nil {
		t.Fatalf("positive C request rejected: %v", err)
	}
	for _, vector := range request.Negative {
		t.Run(vector.Mutation, func(t *testing.T) {
			if err := DecodeHealthRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedHealth) {
				t.Fatalf("negative C request error = %v, want ErrMalformedHealth", err)
			}
		})
	}
}

func evidenceForFlags(flags uint32) HealthEvidence {
	return HealthEvidence{
		SchemaOK:   flags&HealthFlagSchema != 0,
		HavePGTrgm: flags&HealthFlagPGTrgm != 0,
		KBTablesOK: flags&HealthFlagKBTables != 0,
	}
}

func TestHealthReplyMatchesEverySharedCVector(t *testing.T) {
	reply := loadWireBaseline(t).Operations[0].Reply
	if len(reply.Positive) != 8 {
		t.Fatalf("positive flag partitions = %d, want 8", len(reply.Positive))
	}
	for _, vector := range reply.Positive {
		t.Run(string(rune('0'+vector.Flags)), func(t *testing.T) {
			want := decodeHex(t, vector.Hex)
			evidence := evidenceForFlags(vector.Flags)
			if got := EncodeHealthResponse(evidence); string(got) != string(want) {
				t.Fatalf("encoded response = %x, want %x", got, want)
			}
			decoded, err := DecodeHealthResponse(want)
			if err != nil || decoded != evidence {
				t.Fatalf("decoded response = (%+v, %v), want (%+v, nil)", decoded, err, evidence)
			}
		})
	}
	for _, vector := range reply.Negative {
		t.Run(vector.Mutation, func(t *testing.T) {
			if _, err := DecodeHealthResponse(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedHealth) {
				t.Fatalf("negative C response error = %v, want ErrMalformedHealth", err)
			}
		})
	}
}

func TestHealthDecodersRejectNilAndDoNotExposePartialEvidence(t *testing.T) {
	if err := DecodeHealthRequest(nil); !errors.Is(err, ErrMalformedHealth) {
		t.Fatalf("nil request error = %v", err)
	}
	evidence, err := DecodeHealthResponse(nil)
	if !errors.Is(err, ErrMalformedHealth) || evidence != (HealthEvidence{}) {
		t.Fatalf("nil response = (%+v, %v)", evidence, err)
	}
}
