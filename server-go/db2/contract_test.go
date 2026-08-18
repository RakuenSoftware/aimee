package db2

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
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
			Positive         string `json:"positive"`
			SourceSession    string `json:"source_session"`
			Key              string `json:"key"`
			Kind             string `json:"kind"`
			TierA            string `json:"tier_a"`
			TierB            string `json:"tier_b"`
			MemoryID         uint64 `json:"memory_id"`
			HasValue         uint32 `json:"has_value"`
			ValueBits        uint64 `json:"value_bits"`
			ThresholdBits    uint64 `json:"threshold_bits"`
			LowThresholdBits uint64 `json:"low_threshold_bits"`
			MaximumIDs       uint32 `json:"maximum_ids"`
			Negative         []struct {
				Mutation string `json:"mutation"`
				Hex      string `json:"hex"`
			} `json:"negative"`
		} `json:"request"`
		Reply struct {
			Positive []struct {
				Flags                 uint32   `json:"flags"`
				Result                uint32   `json:"result"`
				Dimension             uint32   `json:"dimension"`
				Count                 uint64   `json:"count"`
				DeletedCount          uint32   `json:"deleted_count"`
				DemotedCount          uint32   `json:"demoted_count"`
				AvgEffectivenessBits  uint64   `json:"avg_effectiveness_bits"`
				LowEffectivenessCount uint32   `json:"low_effectiveness_count"`
				HighImpactCount       uint32   `json:"high_impact_count"`
				MemoryIDs             []uint64 `json:"memory_ids"`
				Exists                uint32   `json:"exists"`
				Found                 uint32   `json:"found"`
				ID                    uint64   `json:"id"`
				Size                  uint32   `json:"size"`
				InUse                 uint32   `json:"in_use"`
				Waiters               uint32   `json:"waiters"`
				LeaseGrants           uint64   `json:"lease_grants"`
				LeaseTimeouts         uint64   `json:"lease_timeouts"`
				Stuck                 uint64   `json:"stuck"`
				Poisoned              uint64   `json:"poisoned"`
				RefusedCount          uint64   `json:"refused_count"`
				LastOffered           uint32   `json:"last_offered"`
				Available             uint32   `json:"available"`
				Active                uint32   `json:"active_connections"`
				Maximum               uint32   `json:"max_connections"`
				IsReplica             uint32   `json:"is_replica"`
				ReplicaLag            uint64   `json:"replica_lag_bytes"`
				TargetDim             uint32   `json:"target_dimension"`
				StartedEpoch          uint64   `json:"started_epoch"`
				WasInProgress         uint32   `json:"was_in_progress"`
				RecordedDim           uint32   `json:"recorded_dimension"`
				RunningDim            uint32   `json:"running_dimension"`
				ServingID             string   `json:"serving_id"`
				Hex                   string   `json:"hex"`
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
	if len(baseline.Operations) != 23 || baseline.Operations[0].Name != "health" ||
		baseline.Operations[1].Name != "embedding_dimension" ||
		baseline.Operations[2].Name != "pool_status" ||
		baseline.Operations[3].Name != "embedding_refusals" ||
		baseline.Operations[4].Name != "postgres_status" ||
		baseline.Operations[5].Name != "reembed_status" ||
		baseline.Operations[6].Name != "reembed_clear" ||
		baseline.Operations[7].Name != "reembed_clear_maintenance" ||
		baseline.Operations[8].Name != "embedder_serving_id" ||
		baseline.Operations[9].Name != "dimension_reset" ||
		baseline.Operations[10].Name != "level3_count" ||
		baseline.Operations[11].Name != "level2_count" ||
		baseline.Operations[12].Name != "orphaned_l0_count" ||
		baseline.Operations[13].Name != "total_count" ||
		baseline.Operations[14].Name != "session_l2_count" ||
		baseline.Operations[15].Name != "key_exists" ||
		baseline.Operations[16].Name != "find_id_by_key_kind" ||
		baseline.Operations[17].Name != "key_exists_in_tier_pair" ||
		baseline.Operations[18].Name != "effectiveness_update" ||
		baseline.Operations[19].Name != "retention_enforce" ||
		baseline.Operations[20].Name != "effectiveness_demote" ||
		baseline.Operations[21].Name != "effectiveness_stats" ||
		baseline.Operations[22].Name != "l2_memory_ids" {
		t.Fatalf("unexpected operations: %+v", baseline.Operations)
	}
	return baseline
}

func TestLevel3CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[10]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeLevel3CountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeLevel3CountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeLevel3CountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeLevel3CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeLevel3CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeLevel3CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestLevel2CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[11]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeLevel2CountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeLevel2CountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeLevel2CountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeLevel2CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeLevel2CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeLevel2CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestOrphanedL0CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[12]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeOrphanedL0CountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeOrphanedL0CountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeOrphanedL0CountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeOrphanedL0CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeOrphanedL0CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeOrphanedL0CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestTotalCountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[13]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeTotalCountRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeTotalCountRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeTotalCountRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeTotalCountReply(vector.Count)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeTotalCountReply(got)
		if err != nil || count != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeTotalCountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestSessionL2CountMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[14]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeSessionL2CountRequest(operation.Request.SourceSession)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	if session, err := DecodeSessionL2CountRequest(wantRequest); err != nil || session != operation.Request.SourceSession {
		t.Fatalf("positive request = (%q, %v)", session, err)
	}
	for _, vector := range operation.Request.Negative {
		session, err := DecodeSessionL2CountRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || session != "" {
			t.Fatalf("negative request %s = (%q, %v)", vector.Mutation, session, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeSessionL2CountReply(uint32(vector.Count))
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		count, err := DecodeSessionL2CountReply(got)
		if err != nil || uint64(count) != vector.Count {
			t.Fatalf("decode = (%d, %v)", count, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		count, err := DecodeSessionL2CountReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || count != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, count, err)
		}
	}
}

func TestKeyExistsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[15]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeKeyExistsRequest(operation.Request.Key)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	if key, err := DecodeKeyExistsRequest(wantRequest); err != nil || key != operation.Request.Key {
		t.Fatalf("positive request = (%q, %v)", key, err)
	}
	for _, vector := range operation.Request.Negative {
		key, err := DecodeKeyExistsRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || key != "" {
			t.Fatalf("negative request %s = (%q, %v)", vector.Mutation, key, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeKeyExistsReply(vector.Exists)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		exists, err := DecodeKeyExistsReply(got)
		if err != nil || exists != vector.Exists {
			t.Fatalf("decode = (%d, %v)", exists, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		exists, err := DecodeKeyExistsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || exists != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, exists, err)
		}
	}
}

func TestFindIDByKeyKindMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[16]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeFindIDByKeyKindRequest(operation.Request.Key, operation.Request.Kind)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	key, kind, err := DecodeFindIDByKeyKindRequest(wantRequest)
	if err != nil || key != operation.Request.Key || kind != operation.Request.Kind {
		t.Fatalf("positive request = (%q, %q, %v)", key, kind, err)
	}
	for _, vector := range operation.Request.Negative {
		key, kind, err := DecodeFindIDByKeyKindRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || key != "" || kind != "" {
			t.Fatalf("negative request %s = (%q, %q, %v)", vector.Mutation, key, kind, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeFindIDByKeyKindReply(vector.Found, vector.ID)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		found, id, err := DecodeFindIDByKeyKindReply(got)
		if err != nil || found != vector.Found || id != vector.ID {
			t.Fatalf("decode = (%d, %d, %v)", found, id, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		found, id, err := DecodeFindIDByKeyKindReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || found != 0 || id != 0 {
			t.Fatalf("negative reply %s = (%d, %d, %v)", vector.Mutation, found, id, err)
		}
	}
}

func TestKeyExistsInTierPairMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[17]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeKeyExistsInTierPairRequest(
		operation.Request.Key, operation.Request.TierA, operation.Request.TierB)
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	key, tierA, tierB, err := DecodeKeyExistsInTierPairRequest(wantRequest)
	if err != nil || key != operation.Request.Key || tierA != operation.Request.TierA ||
		tierB != operation.Request.TierB {
		t.Fatalf("positive request = (%q, %q, %q, %v)", key, tierA, tierB, err)
	}
	for _, vector := range operation.Request.Negative {
		key, tierA, tierB, err := DecodeKeyExistsInTierPairRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || key != "" || tierA != "" || tierB != "" {
			t.Fatalf("negative request %s = (%q, %q, %q, %v)",
				vector.Mutation, key, tierA, tierB, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeKeyExistsInTierPairReply(vector.Exists)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		exists, err := DecodeKeyExistsInTierPairReply(got)
		if err != nil || exists != vector.Exists {
			t.Fatalf("decode = (%d, %v)", exists, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		exists, err := DecodeKeyExistsInTierPairReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || exists != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, exists, err)
		}
	}
}

func TestEffectivenessUpdateMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[18]
	wantRequest := decodeHex(t, operation.Request.Positive)
	request, err := EncodeEffectivenessUpdateRequest(
		operation.Request.MemoryID, operation.Request.HasValue,
		math.Float64frombits(operation.Request.ValueBits))
	if err != nil || string(request) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", request, err, wantRequest)
	}
	memoryID, hasValue, value, err := DecodeEffectivenessUpdateRequest(wantRequest)
	if err != nil || memoryID != operation.Request.MemoryID ||
		hasValue != operation.Request.HasValue || math.Float64bits(value) != operation.Request.ValueBits {
		t.Fatalf("positive request = (%d, %d, %x, %v)",
			memoryID, hasValue, math.Float64bits(value), err)
	}
	for _, vector := range operation.Request.Negative {
		memoryID, hasValue, value, err := DecodeEffectivenessUpdateRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || memoryID != 0 || hasValue != 0 || value != 0 {
			t.Fatalf("negative request %s = (%d, %d, %v, %v)",
				vector.Mutation, memoryID, hasValue, value, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEffectivenessUpdateReply(vector.Result)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, err := DecodeEffectivenessUpdateReply(got)
		if err != nil || result != vector.Result {
			t.Fatalf("decode = (%d, %v)", result, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, err := DecodeEffectivenessUpdateReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, result, err)
		}
	}
}

func TestRetentionEnforceMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[19]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeRetentionEnforceRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeRetentionEnforceRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeRetentionEnforceRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeRetentionEnforceReply(vector.DeletedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		deletedCount, err := DecodeRetentionEnforceReply(got)
		if err != nil || deletedCount != vector.DeletedCount {
			t.Fatalf("decode = (%d, %v)", deletedCount, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		deletedCount, err := DecodeRetentionEnforceReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || deletedCount != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, deletedCount, err)
		}
	}
}

func TestEffectivenessDemoteMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[20]
	if operation.Request.ThresholdBits != EffectivenessDemoteThresholdBits ||
		math.Float64bits(0.3) != EffectivenessDemoteThresholdBits {
		t.Fatalf("threshold bits = %x, generated = %x",
			operation.Request.ThresholdBits, EffectivenessDemoteThresholdBits)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEffectivenessDemoteRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEffectivenessDemoteRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEffectivenessDemoteRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEffectivenessDemoteReply(vector.DemotedCount)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		demotedCount, err := DecodeEffectivenessDemoteReply(got)
		if err != nil || demotedCount != vector.DemotedCount {
			t.Fatalf("decode = (%d, %v)", demotedCount, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		demotedCount, err := DecodeEffectivenessDemoteReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || demotedCount != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, demotedCount, err)
		}
	}
}

func TestEffectivenessStatsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[21]
	if operation.Request.LowThresholdBits != EffectivenessStatsLowThresholdBits ||
		math.Float64bits(0.3) != EffectivenessStatsLowThresholdBits {
		t.Fatalf("low threshold bits = %x, generated = %x",
			operation.Request.LowThresholdBits, EffectivenessStatsLowThresholdBits)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEffectivenessStatsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEffectivenessStatsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEffectivenessStatsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		want := EffectivenessStats{
			AvgEffectiveness:      math.Float64frombits(vector.AvgEffectivenessBits),
			LowEffectivenessCount: vector.LowEffectivenessCount,
			HighImpactCount:       vector.HighImpactCount,
		}
		got, err := EncodeEffectivenessStatsReply(want)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		stats, err := DecodeEffectivenessStatsReply(got)
		if err != nil || stats != want {
			t.Fatalf("decode = (%+v, %v)", stats, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		stats, err := DecodeEffectivenessStatsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || stats != (EffectivenessStats{}) {
			t.Fatalf("negative reply %s = (%+v, %v)", vector.Mutation, stats, err)
		}
	}
	// The average is a probability, so neither bound may be encodable past its edge.
	for _, average := range []float64{-0.5, 1.5, math.NaN(), math.Inf(1)} {
		if _, err := EncodeEffectivenessStatsReply(EffectivenessStats{AvgEffectiveness: average}); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("average %v encoded: %v", average, err)
		}
	}
}

func TestL2MemoryIDsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[22]
	if operation.Request.MaximumIDs != L2MemoryIDsMax {
		t.Fatalf("maximum ids = %d, generated = %d", operation.Request.MaximumIDs, L2MemoryIDsMax)
	}
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeL2MemoryIDsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeL2MemoryIDsRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeL2MemoryIDsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeL2MemoryIDsReply(vector.MemoryIDs)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		memoryIDs, err := DecodeL2MemoryIDsReply(got)
		if err != nil || len(memoryIDs) != len(vector.MemoryIDs) {
			t.Fatalf("decode = (%v, %v)", memoryIDs, err)
		}
		for index, id := range memoryIDs {
			if id != vector.MemoryIDs[index] {
				t.Fatalf("decode[%d] = %d, want %d", index, id, vector.MemoryIDs[index])
			}
		}
	}
	for _, vector := range operation.Reply.Negative {
		memoryIDs, err := DecodeL2MemoryIDsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || memoryIDs != nil {
			t.Fatalf("negative reply %s = (%v, %v)", vector.Mutation, memoryIDs, err)
		}
	}
	// The declared bound is a ceiling, and identifiers stay positive.
	if _, err := EncodeL2MemoryIDsReply(make([]uint64, L2MemoryIDsMax+1)); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("over-bound list encoded: %v", err)
	}
	if _, err := EncodeL2MemoryIDsReply([]uint64{0}); !errors.Is(err, ErrMalformedEnvelope) {
		t.Fatalf("zero identifier encoded: %v", err)
	}
}

func TestEmbedderServingIDMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[8]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEmbedderServingIDRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	if err := DecodeEmbedderServingIDRequest(wantRequest); err != nil {
		t.Fatalf("positive request: %v", err)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEmbedderServingIDRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeEmbedderServingIDReply(vector.Result, vector.ServingID)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, servingID, err := DecodeEmbedderServingIDReply(got)
		if err != nil || result != vector.Result || servingID != vector.ServingID {
			t.Fatalf("decode = (%d, %q, %v)", result, servingID, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, servingID, err := DecodeEmbedderServingIDReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || servingID != "" {
			t.Fatalf("negative reply %s = (%d, %q, %v)", vector.Mutation, result, servingID, err)
		}
	}
}

func TestDimensionResetMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[9]
	wantRequest := decodeHex(t, operation.Request.Positive)
	gotRequest, err := EncodeDimensionResetRequest(384, 0, 1)
	if err != nil || string(gotRequest) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", gotRequest, err, wantRequest)
	}
	target, force, dryRun, err := DecodeDimensionResetRequest(gotRequest)
	if err != nil || target != 384 || force != 0 || dryRun != 1 {
		t.Fatalf("decoded request = (%d, %d, %d, %v)", target, force, dryRun, err)
	}
	for _, vector := range operation.Request.Negative {
		target, force, dryRun, err := DecodeDimensionResetRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || target != 0 || force != 0 || dryRun != 0 {
			t.Fatalf("negative request %s = (%d, %d, %d, %v)",
				vector.Mutation, target, force, dryRun, err)
		}
	}
	expected := DimensionReset{
		RecordedDimension: 768, TargetDimension: 384, TablesDiscovered: 6,
		RowsCleared: 1234,
	}
	for _, vector := range operation.Reply.Positive {
		status := expected
		if vector.Result == ResultInvalidState {
			status = DimensionReset{}
		}
		got, err := EncodeDimensionResetReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeDimensionResetReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeDimensionResetReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (DimensionReset{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestReembedClearMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[6]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeReembedClearRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeReembedClearRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		got, err := EncodeReembedClearReply(vector.Result)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, err := DecodeReembedClearReply(got)
		if err != nil || result != vector.Result {
			t.Fatalf("decode = (%d, %v)", result, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, err := DecodeReembedClearReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 {
			t.Fatalf("negative reply %s = (%d, %v)", vector.Mutation, result, err)
		}
	}
}

func TestReembedClearMaintenanceMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[7]
	wantRequest := decodeHex(t, operation.Request.Positive)
	gotRequest, err := EncodeReembedClearMaintenanceRequest(0)
	if err != nil || string(gotRequest) != string(wantRequest) {
		t.Fatalf("request = (%x, %v), want %x", gotRequest, err, wantRequest)
	}
	force, err := DecodeReembedClearMaintenanceRequest(gotRequest)
	if err != nil || force != 0 {
		t.Fatalf("decoded force = (%d, %v)", force, err)
	}
	for _, vector := range operation.Request.Negative {
		force, err := DecodeReembedClearMaintenanceRequest(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || force != 0 {
			t.Fatalf("negative request %s = (%d, %v)", vector.Mutation, force, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := ReembedClearMaintenance{
			WasInProgress: vector.WasInProgress, RecordedDimension: vector.RecordedDim,
			RunningDimension: vector.RunningDim,
		}
		got, err := EncodeReembedClearMaintenanceReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeReembedClearMaintenanceReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeReembedClearMaintenanceReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 ||
			status != (ReembedClearMaintenance{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestReembedStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[5]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeReembedStatusRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeReembedStatusRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := ReembedStatus{vector.TargetDim, vector.StartedEpoch}
		got, err := EncodeReembedStatusReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeReembedStatusReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeReembedStatusReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (ReembedStatus{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestPostgresStatusMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[4]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodePostgresStatusRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodePostgresStatusRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := PostgresStatus{vector.Available, vector.Active, vector.Maximum, vector.IsReplica, vector.ReplicaLag}
		got, err := EncodePostgresStatusReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodePostgresStatusReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodePostgresStatusReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (PostgresStatus{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
}

func TestEmbeddingRefusalsMatchesEverySharedCVector(t *testing.T) {
	operation := loadWireBaseline(t).Operations[3]
	wantRequest := decodeHex(t, operation.Request.Positive)
	if got := EncodeEmbeddingRefusalsRequest(); string(got) != string(wantRequest) {
		t.Fatalf("request = %x, want %x", got, wantRequest)
	}
	for _, vector := range operation.Request.Negative {
		if err := DecodeEmbeddingRefusalsRequest(decodeHex(t, vector.Hex)); !errors.Is(err, ErrMalformedEnvelope) {
			t.Fatalf("negative request %s: %v", vector.Mutation, err)
		}
	}
	for _, vector := range operation.Reply.Positive {
		status := EmbeddingRefusals{vector.RefusedCount, vector.LastOffered}
		got, err := EncodeEmbeddingRefusalsReply(vector.Result, status)
		if err != nil || string(got) != string(decodeHex(t, vector.Hex)) {
			t.Fatalf("positive reply = (%x, %v)", got, err)
		}
		result, decoded, err := DecodeEmbeddingRefusalsReply(got)
		if err != nil || result != vector.Result || decoded != status {
			t.Fatalf("decode = (%d, %+v, %v)", result, decoded, err)
		}
	}
	for _, vector := range operation.Reply.Negative {
		result, status, err := DecodeEmbeddingRefusalsReply(decodeHex(t, vector.Hex))
		if !errors.Is(err, ErrMalformedEnvelope) || result != 0 || status != (EmbeddingRefusals{}) {
			t.Fatalf("negative reply %s = (%d, %+v, %v)", vector.Mutation, result, status, err)
		}
	}
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
