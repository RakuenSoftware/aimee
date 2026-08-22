package db2

import (
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestAuditEventListRequiresAStart(t *testing.T) {
	// The required start is what keeps this from becoming a full-table scan, so
	// the cheapest way to ask for everything stays closed.
	//
	// The schema enforces it before the handler does: since_at declares a
	// minimum of one byte, so an empty start will not encode. That is the better
	// place for the rule -- a caller learns at the boundary rather than after a
	// round trip -- and it means the handler's own check is unreachable over the
	// wire and reachable only from a hand-built frame. Both are pinned, because
	// the second is what a Go implementation would quietly lose if the schema
	// bound ever relaxed.
	if _, err := db2contract.EncodeAuditEventListRequest("", "", "", 8); err == nil {
		t.Fatal("an empty start encoded; the schema no longer requires one")
	}

	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	valid, err := db2contract.EncodeAuditEventListRequest("2026-01-01", "", "", 8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	// A frame whose start was blanked after encoding: the length prefix says
	// nothing follows, which decodes to an empty string and reaches the handler.
	blanked := blankFirstStringField(t, valid)
	if _, status := handler(invocation(db2contract.StageAuditEventList), blanked); status ==
		bus.ModuleStatusOK {
		t.Fatal("a frame with an empty start was answered")
	}
	if store.lastSQL != "" {
		t.Fatal("a request with no start reached the store")
	}
}

// blankFirstStringField rewrites a request so its first utf8 field is empty,
// producing a frame the encoder would refuse but a decoder may still accept.
func blankFirstStringField(t *testing.T, request []byte) []byte {
	t.Helper()
	header := int(db2contract.EnvelopeHeaderLen)
	if len(request) < header+4 {
		t.Fatalf("request is too short to hold a string field")
	}
	length := int(request[header]) | int(request[header+1])<<8 |
		int(request[header+2])<<16 | int(request[header+3])<<24
	blanked := make([]byte, 0, len(request)-length)
	blanked = append(blanked, request[:header]...)
	blanked = append(blanked, 0, 0, 0, 0)
	blanked = append(blanked, request[header+4+length:]...)
	// The header carries the payload length, which just shrank.
	payload := uint32(len(blanked) - header)
	blanked[12] = byte(payload)
	blanked[13] = byte(payload >> 8)
	blanked[14] = byte(payload >> 16)
	blanked[15] = byte(payload >> 24)
	return blanked
}

func TestAuditEventListAddsOnlyThePredicatesItWasGiven(t *testing.T) {
	for _, testCase := range []struct {
		name       string
		until      string
		scopeKind  string
		wantUntil  bool
		wantScope  bool
		wantArgLen int
	}{
		{"start only", "", "", false, false, 2},
		{"start and end", "2026-12-31", "", true, false, 3},
		{"start and scope", "", "user", false, true, 3},
		{"all three", "2026-12-31", "user", true, true, 4},
	} {
		t.Run(testCase.name, func(t *testing.T) {
			store := &fakeStore{rows: &fakeRows{}}
			handler := NewDispatchHandler(store)
			request, err := db2contract.EncodeAuditEventListRequest(
				"2026-01-01", testCase.until, testCase.scopeKind, 8)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			if _, status := handler(invocation(db2contract.StageAuditEventList), request); status !=
				bus.ModuleStatusOK {
				t.Fatalf("status = %v", status)
			}
			if strings.Contains(store.lastSQL, "applied_at <=") != testCase.wantUntil {
				t.Errorf("end predicate present = %v, want %v",
					!testCase.wantUntil, testCase.wantUntil)
			}
			if strings.Contains(store.lastSQL, "scope_kind =") != testCase.wantScope {
				t.Errorf("scope predicate present = %v, want %v",
					!testCase.wantScope, testCase.wantScope)
			}
			if len(store.lastArgs) != testCase.wantArgLen {
				t.Errorf("args = %v, want %d", store.lastArgs, testCase.wantArgLen)
			}
			// However many predicates there are, the limit is always the last
			// placeholder -- which is the part a hand-numbered statement gets
			// wrong.
			if !strings.HasSuffix(store.lastSQL,
				"LIMIT $"+itoa(uint32(testCase.wantArgLen))) {
				t.Errorf("limit placeholder is not last: %q", store.lastSQL)
			}
		})
	}
}

func TestAuditEventListReadsRows(t *testing.T) {
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"replay-audit", "surface", "target", "op", "user", "replay",
			"2026-08-22T10:27:07Z", 0.5, true},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAuditEventListRequest("2026-01-01", "", "", 8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAuditEventList), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeAuditEventListReply(body)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(rows) != 1 || rows[0].EventID != "replay-audit" ||
		rows[0].AppliedConfidence != 0.5 || rows[0].FlaggedForReview != 1 {
		t.Fatalf("rows = %+v", rows)
	}
}

func TestDemotionCandidatesFloorsTheMinimumAtOne(t *testing.T) {
	// A HAVING of zero returns every group, which is not a candidate list.
	store := &fakeStore{rows: &fakeRows{}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionCandidatesRequest(0)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	if _, status := handler(invocation(db2contract.StageDemotionCandidates), request); status !=
		bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if len(store.lastArgs) != 2 || store.lastArgs[0] != 1 {
		t.Fatalf("args = %v, want a floor of 1", store.lastArgs)
	}
}

func TestDemotionCandidatesSkipsANonNumericScope(t *testing.T) {
	// The C implementation runs the identifier through atoll, which answers zero
	// for anything unparseable -- and zero names a row. Skipping is what that
	// loop means rather than what it does.
	store := &fakeStore{rows: &fakeRows{values: [][]any{
		{"4242", int64(3)},
		{"not-a-row", int64(9)},
		{"", int64(5)},
	}}}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeDemotionCandidatesRequest(1)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageDemotionCandidates), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	rows, err := db2contract.DecodeDemotionCandidatesReply(body)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(rows) != 1 || rows[0].CandidateRowID != 4242 || rows[0].AttributionCount != 3 {
		t.Fatalf("rows = %+v -- an unparseable scope was not skipped", rows)
	}
}
