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
	Operations    []struct {
		Request struct {
			Positive string `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"request"`
		Reply struct {
			Positive []struct {
				Flags uint32 `json:"flags"`
				Hex   string `json:"hex"`
			} `json:"positive"`
			Negative []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"reply"`
	} `json:"operations"`
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
	if len(baseline.Operations) != 1 {
		t.Fatalf("operation count = %d, want 1", len(baseline.Operations))
	}
	return baseline
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
